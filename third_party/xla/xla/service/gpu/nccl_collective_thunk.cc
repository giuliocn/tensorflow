/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/service/gpu/nccl_collective_thunk.h"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "mlir/IR/Value.h"  // from @llvm-project
#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/layout_util.h"
#include "xla/primitive_util.h"
#include "xla/service/collective_ops_utils.h"
#include "xla/service/global_device_id.h"
#include "xla/service/gpu/buffer_allocations.h"
#include "xla/service/gpu/thunk.h"
#include "xla/shape.h"
#include "xla/status.h"
#include "xla/statusor.h"
#include "xla/stream_executor/gpu/gpu_activation.h"
#include "xla/util.h"
#include "tsl/platform/errors.h"

namespace xla {
namespace gpu {
namespace {

bool IsTypeSupportedByNccl(PrimitiveType element_type,
                           Thunk::Kind reduction_op) {
  switch (element_type) {
    case S8:
    case PRED:
    case U8:
    case S32:
    case U32:
    case S64:
    case U64:
    case F16:
    case F32:
    case F64:
#if defined(__CUDA_BF16_TYPES_EXIST__) || TENSORFLOW_USE_ROCM
    case BF16:
#endif
    case C64:
    case C128:
      return true;
    case S16:
    case U16:
      // 16-bit integer reductions are not directly supported by NCCL and cannot
      // be implicitly converted into other 16-bit types like ncclFloat16 as
      // they involve actual computation and not just data movement.
    case F8E5M2:
    case F8E4M3FN:
      return !IsReductionCollective(reduction_op);
    default:
      return false;
  }
}

}  // namespace

// This file runs collective ops (i.e. ops that communicate between multiple
// GPUs) using NCCL.
//
// Here's a high-level overview of how running an op works.
//
//  - Multiple threads call ExecuteOnStream.
//  - All threads that "go together" (i.e. are participating in the "same"
//    collective op) choose the same Rendezvous object from a global map.
//  - Once all threads have arrived at the Rendezvous, we know exactly which
//    GPUs are participating in the op, so we get or create a NcclClique
//    containing those GPUs.
//  - We perform the NCCL operation using the clique.

// Returns if the collective communication operation is degenerate because all
// the groups formed by the operation are singleton. A given op can be
// degenerate under several conditions, corresponding to the modes supported
// in GetParticipatingDevices().
//   1. no channel id, use_global_device_ids = false:
//         degenerate if replica_groups are singleton, or groups empty and
//         replica_count == 1.
//   2. channel_id is set, use_global_device_ids = false:
//         degenerate if replica_groups are singleton and num_partitions == 1,
//         or groups empty and num_replicas == 1 && num_partitions == 1.
//   3. channel_id is set, use_global_device_ids = true (flattened-ids):
//         degenerate if replica_groups are singleton (groups cannot be empty).
//   4. no channel_id, no use_global_device_ids:
//         identical to 1.
//   5. channel_id is set, no use_global_device_ids:
//         degenerate if replica_groups are singleton or group emty and
//         num_partitions == 1 (since replica groups contain partition ids).
//
bool NcclCollectiveConfig::IsDegenerate(int64_t replica_count,
                                        int64_t partition_count) const {
  bool groups_empty = replica_groups.empty();

  // check if all replica_groups are singleton. If not, then the operation is
  // not degenerate.
  bool all_groups_singleton =
      !groups_empty &&
      absl::c_all_of(replica_groups, [](const ReplicaGroup& group) {
        return group.replica_ids_size() == 1;
      });

  switch (group_mode) {
    case CollectiveOpGroupMode::kCrossReplica:
      return all_groups_singleton || (groups_empty && replica_count == 1);
    case CollectiveOpGroupMode::kCrossPartition:
      return all_groups_singleton || (groups_empty && partition_count == 1);
    case CollectiveOpGroupMode::kCrossReplicaAndPartition:
      return (all_groups_singleton && partition_count == 1) ||
             (groups_empty && replica_count == 1 && partition_count == 1);
    case CollectiveOpGroupMode::kFlattenedID:
      CHECK(!groups_empty)
          << "replica groups cannot be empty if use_global_device_ids = true";
      return all_groups_singleton;
    default:
      CHECK(0) << "Invalid collective op mode";
      return false;
  }
}

void NcclCollectiveConfig::SetCollectiveOpKindAndID(
    const HloCollectivePermuteInstruction* instr) {
  if (instr->channel_id().has_value()) {
    collective_op_kind = RendezvousKey::kCrossModule;
    op_id = instr->channel_id().value();
  } else {
    collective_op_kind = RendezvousKey::kCrossReplica;
    op_id = static_cast<int64_t>(instr->GetModule()->unique_id());
  }
}

NcclCollectiveConfig GetNcclCollectiveConfig(
    const HloInstruction* hlo, std::optional<bool> use_global_device_ids) {
  NcclCollectiveConfig config;
  config.operand_count = hlo->operands().size();
  config.operand_element_type.reserve(config.operand_count);
  for (int i = 0; i < config.operand_count; i++) {
    config.operand_element_type.push_back(
        hlo->operand(i)->shape().element_type());
  }
  config.replica_groups = hlo->replica_groups();

  if (hlo->channel_id().has_value()) {
    config.collective_op_kind = RendezvousKey::kCrossModule;
    config.op_id = *hlo->channel_id();
  } else {
    config.collective_op_kind = RendezvousKey::kCrossReplica;
    config.op_id = static_cast<int64_t>(hlo->GetModule()->unique_id());
  }

  config.group_mode = GetCollectiveOpGroupMode(hlo->channel_id().has_value(),
                                               use_global_device_ids)
                          .value();

  return config;
}

NcclCollectiveThunk::NcclCollectiveThunk(Kind kind, ThunkInfo thunk_info,
                                         bool is_sync)
    : Thunk(kind, thunk_info) {
  if (!is_sync) {
    async_ = std::make_unique<AsyncExecutor>();
  }
}

/* static */ bool NcclCollectiveThunk::NcclIsEnabled() {
#if XLA_ENABLE_XCCL
  return true;
#else
  return false;
#endif
}

/* static */ absl::Status NcclCollectiveThunk::CheckImplementable() {
  if (!NcclIsEnabled()) {
    return tsl::errors::Unimplemented("NCCL is not enabled");
  }
  return absl::OkStatus();
}

#if XLA_ENABLE_XCCL
absl::StatusOr<NcclComm::Lock> LockNcclComm(
    const NcclExecuteParams& params,
    const std::vector<ReplicaGroup>& replica_groups,
    CollectiveOpGroupMode group_mode, int64_t op_id, int64_t stream_id,
    bool enable_clique_optimization) {
  TF_ASSIGN_OR_RETURN(GlobalDeviceId global_device_id,
                      params.GetGlobalDeviceId());

  TF_ASSIGN_OR_RETURN(
      std::vector<GlobalDeviceId> participants,
      GetParticipatingDevices(global_device_id, *params.device_assn,
                              replica_groups, group_mode));

  if (IsGlobalNcclConfig() &&
      (participants.size() != params.device_assn->replica_count())) {
    return InvalidArgument(
        "Partial replica groups are not allowed when using NCCL_COMM_ID "
        "environment configuration.");
  }

  auto it = absl::c_find(participants, global_device_id);
  TF_RET_CHECK(it != participants.end());
  int rank = it - participants.begin();

  std::vector<GlobalDeviceId> local_devices;
  if (params.gpu_global_device_ids) {
    local_devices.reserve(params.gpu_global_device_ids->size());
    for (const auto& entry : *params.gpu_global_device_ids) {
      local_devices.push_back(entry.second);
    }
  }
  size_t num_local_participants = GetNumLocalParticipants(
      participants, params.gpu_global_device_ids ? &local_devices : nullptr);

  bool is_local = participants.size() == num_local_participants;
  TF_ASSIGN_OR_RETURN(
      const NcclUniqueIdCallback* unique_id_callback,
      GetNcclUniqueIdCallback(params.nccl_unique_id_callback, is_local));

  se::gpu::ScopedActivateExecutorContext scoped_context(params.stream_executor);

  return AcquireNcclComm(params.run_id, OpId(op_id), std::move(participants),
                         num_local_participants, *unique_id_callback, rank,
                         stream_id, enable_clique_optimization);
}
#endif  // XLA_ENABLE_XCCL

absl::StatusOr<std::vector<DeviceBufferPair>> ConvertToDeviceBuffers(
    const Thunk::ExecuteParams& params,
    const std::vector<NcclCollectiveThunk::Buffer>& buffers,
    const std::vector<PrimitiveType>& element_types) {
  return ConvertToDeviceBuffers(params.buffer_allocations, buffers,
                                element_types);
}

absl::StatusOr<std::vector<DeviceBufferPair>> ConvertToDeviceBuffers(
    const BufferAllocations* buffer_allocations,
    const std::vector<NcclCollectiveThunk::Buffer>& buffers,
    const std::vector<PrimitiveType>& element_types) {
  if (buffers.size() != element_types.size())
    return FailedPrecondition("Mismatch in operand buffer counts.");

  std::vector<DeviceBufferPair> device_buffers;
  device_buffers.reserve(buffers.size());
  for (int i = 0; i < buffers.size(); ++i) {
    device_buffers.emplace_back(DeviceBufferPair{
        element_types[i], buffers[i].element_count,
        buffer_allocations->GetDeviceAddress(buffers[i].source_buffer),
        buffer_allocations->GetDeviceAddress(buffers[i].destination_buffer)});
  }
  return device_buffers;
}

absl::Status NcclCollectiveThunk::ExecuteOnStream(const ExecuteParams& params) {
#if XLA_ENABLE_XCCL
  VLOG(1) << absl::StreamFormat("Starting %s %s.", IsAsync() ? "async" : "sync",
                                Thunk::KindToString(kind()));
  const int64_t stream_id = GetStreamId();
  TF_ASSIGN_OR_RETURN(
      NcclComm::Lock comm,
      LockNcclComm(params.nccl_params, config().replica_groups,
                   config().group_mode, config().op_id, stream_id,
                   /*enable_clique_optimization=*/false));

  // Run the collective on main stream or using the async executor.
  absl::Status status = [&]() {
    if (!IsAsync()) {
      return RunNcclCollective(params, *params.stream, *comm);
    }
    return async_->Execute(
        [this](const ExecuteParams& params, se::Stream& stream,
               ncclComm_t comm) {
          return RunNcclCollective(params, stream, comm);
        },
        params, *comm, GetAsyncStreamKind());
  }();
  TF_RETURN_IF_ERROR(status);

  // Block host on the first call to ensure that all devices have allocated the
  // required buffers for their communicators before allowing any device to
  // continue enqueuing operations. Otherwise, the allocations can cause
  // deadlock in the CUDA driver (b/215649390).
  if (first_call_to_execute_) {
    se::Stream* stream = IsAsync()
                             ? params.async_comms_streams[GetAsyncStreamKind()]
                             : params.stream;
    TF_RETURN_IF_ERROR(stream->BlockHostUntilDone());
    first_call_to_execute_ = false;
  }
  return absl::OkStatus();
#else   // XLA_ENABLE_XCCL
  return Unimplemented(
      "NCCL support is not available: this binary was not built with a CUDA "
      "compiler, which is necessary to build the NCCL source library.");
#endif  // XLA_ENABLE_XCCL
}

std::string NcclCollectiveThunk::GetDeviceString(
    const NcclExecuteParams& nccl_params) {
  int device_ordinal = nccl_params.stream_executor->device_ordinal();
  GlobalDeviceId global_device_id = nccl_params.GetGlobalDeviceId().value();
  DeviceAssignment::LogicalID logical_id =
      nccl_params.device_assn->LogicalIdForDevice(global_device_id).value();
  return absl::StrFormat("(r%d, p%d) : GlobalID %d, ord %d",
                         logical_id.replica_id, logical_id.computation_id,
                         global_device_id.value(), device_ordinal);
}

absl::Status NcclCollectiveThunk::AsyncExecutor::Execute(
    absl::FunctionRef<Status(const ExecuteParams&, se::Stream&, ncclComm_t)> fn,
    const ExecuteParams& params, ncclComm_t comm, AsyncStreamKind stream_kind) {
  se::Stream& async_comms_stream = *params.async_comms_streams[stream_kind];
  // Wait until compute inputs are ready.
  async_comms_stream.ThenWaitFor(params.stream);

  TF_RETURN_IF_ERROR(fn(params, async_comms_stream, comm));

  // Create an event on the async stream for the completion of the collective.
  se::Event done_event(async_comms_stream.parent());
  TF_RET_CHECK(done_event.Init());
  async_comms_stream.ThenRecordEvent(&done_event);

  int device_ordinal = async_comms_stream.parent()->device_ordinal();
  absl::MutexLock lock(&mu_);
  auto [_, was_inserted] =
      done_events_.insert({device_ordinal, std::move(done_event)});
  TF_RET_CHECK(was_inserted) << "done event has not been consumed";
  return absl::OkStatus();
}

absl::Status NcclCollectiveThunk::AsyncExecutor::Await(
    const ExecuteParams& params) {
  int device_ordinal = params.stream->parent()->device_ordinal();
  auto done_event = [this, device_ordinal] {
    absl::MutexLock lock(&mu_);
    return done_events_.extract(device_ordinal);
  }();
  TF_RET_CHECK(done_event) << "done event not found";
  params.stream->ThenWaitFor(&done_event.mapped());
  return absl::OkStatus();
}

NcclCollectiveDoneThunk::NcclCollectiveDoneThunk(
    Thunk::Kind kind, ThunkInfo thunk_info,
    NcclCollectiveThunk::AsyncExecutor& async)
    : Thunk(kind, std::move(thunk_info)), async_(async) {}

absl::Status NcclCollectiveDoneThunk::ExecuteOnStream(
    const ExecuteParams& params) {
  return async_.Await(params);
}

absl::Status IsValidOperand(mlir::Value operand, Thunk::Kind reduction_op) {
  Shape shape = GetShape(operand);
  return IsValidOperand(shape, reduction_op);
}

absl::Status IsValidOperand(Shape shape, Thunk::Kind reduction_op) {
  if (!LayoutUtil::IsDenseArray(shape)) {
    return tsl::errors::Unimplemented(
        absl::StrFormat("input is not a dense array: %s",
                        shape.ToString(/*print_layout=*/true)));
  }
  if (!IsTypeSupportedByNccl(shape.element_type(), reduction_op)) {
    return tsl::errors::Unimplemented(absl::StrFormat(
        "element type %s not suppored by NCCL",
        primitive_util::LowercasePrimitiveTypeName(shape.element_type())));
  }
  return absl::OkStatus();
}

}  // namespace gpu
}  // namespace xla
