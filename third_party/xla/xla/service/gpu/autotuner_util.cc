/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

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

#include "xla/service/gpu/autotuner_util.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/base/const_init.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "xla/autotune_results.pb.h"
#include "xla/autotuning.pb.h"
#include "xla/hlo/ir/hlo_clone_context.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_opcode.h"
#include "xla/service/compilation_environments.h"
#include "xla/service/gpu/gpu_asm_opts_util.h"
#include "xla/service/gpu/stream_executor_util.h"
#include "xla/shape.h"
#include "xla/shape_util.h"
#include "xla/status.h"
#include "xla/status_macros.h"
#include "xla/statusor.h"
#include "xla/stream_executor/device_memory.h"
#include "xla/stream_executor/gpu/redzone_allocator.h"
#include "xla/stream_executor/stream.h"
#include "xla/util.h"
#include "tsl/platform/env.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/logging.h"
#include "tsl/platform/path.h"
#include "tsl/platform/protobuf.h"  // IWYU pragma: keep
#include "tsl/platform/statusor.h"

namespace xla {
namespace gpu {

using AutotuneCacheMap = absl::flat_hash_map<AutotuneCacheKey, AutotuneResult>;

static absl::Mutex autotune_cache_mu(absl::kConstInit);
static auto& autotune_cache ABSL_GUARDED_BY(autotune_cache_mu) =
    *new AutotuneCacheMap();

/*static*/ absl::Status AutotunerUtil::SerializeAutotuneResults(
    AutotuneResults* results) {
  absl::MutexLock lock(&autotune_cache_mu);
  for (const auto& [k, result] : autotune_cache) {
    auto& entry = *results->add_results();
    entry.set_device(std::string(k.GetModelStr()));
    entry.set_hlo(std::string(k.GetHlo()));
    *entry.mutable_result() = result;
  }

  // Sort the results so that they're deterministic.
  std::sort(results->mutable_results()->pointer_begin(),
            results->mutable_results()->pointer_end(),
            [](const auto* a, const auto* b) {
              return std::make_pair(absl::string_view(a->device()),
                                    absl::string_view(a->hlo())) <
                     std::make_pair(absl::string_view(b->device()),
                                    absl::string_view(b->hlo()));
            });

  return absl::OkStatus();
}

/*static*/ absl::Status AutotunerUtil::LoadAutotuneResults(
    const AutotuneResults& results) {
  absl::MutexLock lock(&autotune_cache_mu);
  for (const auto& result : results.results()) {
    autotune_cache[AutotuneCacheKey(result.device(), result.hlo())] =
        result.result();
  }
  return absl::OkStatus();
}

/*static*/ void AutotunerUtil::ClearAutotuneResults() {
  absl::MutexLock lock(&autotune_cache_mu);
  autotune_cache.clear();
}

/* static*/ absl::StatusOr<se::DeviceMemoryBase> AutotunerUtil::CreateBuffer(
    se::RedzoneAllocator& allocator, const Shape& shape,
    const AutotuneConfig& config, int64_t& rng_state) {
  TF_ASSIGN_OR_RETURN(se::DeviceMemoryBase buffer,
                      allocator.AllocateBytes(ShapeUtil::ByteSizeOf(shape)));
  if (config.should_init_buffers()) {
    InitializeBuffer(allocator.stream(), shape.element_type(), &rng_state,
                     buffer);
  }
  return buffer;
}

static std::string ToCanonicalString(const HloInstruction* instr) {
  auto options = HloPrintOptions::Canonical();
  if (instr->opcode() != HloOpcode::kFusion) {
    options.set_print_backend_config(true);
    return instr->ToString(options);
  }
  options.set_print_subcomputation_mode(
      HloPrintOptions::PrintSubcomputationMode::kOff);
  options.set_print_infeed_outfeed_config(false);
  options.set_print_only_essential_constants(true);
  options.set_print_operand_shape(true);
  options.set_print_ids(false);
  options.set_canonicalize_computations(true);

  // TODO(b/266210099): This is unsound. We should probably do the fingerprint
  // of the HLO computation proto instead.
  return instr->called_computations()[0]->ToString(options);
}

AutotuneCacheKey::AutotuneCacheKey(absl::string_view model_str,
                                   const HloInstruction& instr)
    : AutotuneCacheKey(model_str, ToCanonicalString(&instr)) {}

static AutotuneResult* TryFindInCache(const AutotuneCacheKey& key) {
  absl::MutexLock lock(&autotune_cache_mu);
  auto it = autotune_cache.find(key);
  if (it != autotune_cache.end()) {
    VLOG(1) << "Autotune cache hit";
    return &it->second;
  }
  return nullptr;
}

/*static*/ AutotuneCacheKey AutotunerUtil::GetKey(
    const HloInstruction* instr, const AutotuneConfig& config) {
  return AutotuneCacheKey(config.GetModelStr(), *instr);
}

/*static*/ bool AutotunerUtil::IsInCache(const AutotuneCacheKey& key) {
  return TryFindInCache(key) != nullptr;
}

/*static*/ bool AutotunerUtil::AddResult(const AutotuneCacheKey& key,
                                         AutotuneResult result) {
  absl::MutexLock lock(&autotune_cache_mu);
  auto [_, inserted] = autotune_cache.emplace(key, std::move(result));
  return inserted;
}

/*static*/ absl::StatusOr<AutotuneResult> AutotunerUtil::Autotune(
    const HloInstruction* instr, const AutotuneConfig& config,
    const AutotuneNoCacheFn& autotune_fn) {
  AutotuneCacheKey key = GetKey(instr, config);
  if (AutotuneResult* res = TryFindInCache(key)) {
    return *res;
  }

  TF_ASSIGN_OR_RETURN(AutotuneResult autotune_result, autotune_fn());

  absl::MutexLock lock(&autotune_cache_mu);
  auto [it, inserted] = autotune_cache.emplace(key, autotune_result);
  return it->second;
}

namespace {

// Bump this version whenever you change the structure of the results.
// LINT.IfChange(version)
constexpr int kVersion = 2;
// LINT.ThenChange()

bool IsTextProtoPath(absl::string_view file_path) {
  return absl::EndsWith(file_path, ".txt") ||
         absl::EndsWith(file_path, ".textproto") ||
         absl::EndsWith(file_path, ".prototxt");
}

}  // anonymous namespace

/*static*/ absl::Status AutotunerUtil::LoadAutotuneResults(
    absl::string_view data, bool as_textproto) {
  AutotuneResults results;
  // The cast here is necessary for MacOS builds.
  bool parse_success =
      as_textproto ? tsl::protobuf::TextFormat::ParseFromString(
                         std::string(data), &results)             // NOLINT
                   : results.ParseFromString(std::string(data));  // NOLINT
  if (!parse_success) {
    return absl::InvalidArgumentError(
        "Failed to parse autotune results string.");
  }
  if (results.version() != kVersion) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Version mismatch in autotune results. Expected %d but was %d",
        kVersion, results.version()));
  }

  TF_RETURN_IF_ERROR(LoadAutotuneResults(results));
  return absl::OkStatus();
}

/*static*/ absl::StatusOr<std::string> AutotunerUtil::SerializeAutotuneResults(
    bool as_textproto) {
  AutotuneResults results;
  results.set_version(kVersion);
  TF_RETURN_IF_ERROR(SerializeAutotuneResults(&results));
  if (as_textproto) {
    std::string textproto;
    if (tsl::protobuf::TextFormat::PrintToString(results, &textproto)) {
      return textproto;
    } else {
      return InternalError("Failed to serialize autotune results.");
    }
  }
  return results.SerializeAsString();
}

/*static*/ absl::Status AutotunerUtil::SerializeAutotuneResultsToFile(
    absl::string_view file_path) {
  TF_RET_CHECK(!file_path.empty());

  std::string resolved_path;
  if (!tsl::io::ResolveTestPrefixes(file_path, resolved_path)) {
    return FailedPrecondition("File path can not be resolved: %s", file_path);
  }

  TF_ASSIGN_OR_RETURN(std::string autotune_results_str,
                      SerializeAutotuneResults(IsTextProtoPath(resolved_path)));
  TF_RETURN_IF_ERROR(tsl::WriteStringToFile(tsl::Env::Default(), resolved_path,
                                            autotune_results_str));
  LOG(INFO) << "Autotune results serialized to file: " << resolved_path;

  return absl::OkStatus();
}

/*static*/ absl::Status AutotunerUtil::LoadAutotuneResultsFromFile(
    absl::string_view file_path) {
  TF_RET_CHECK(!file_path.empty());

  std::string resolved_path;
  if (!tsl::io::ResolveTestPrefixes(file_path, resolved_path)) {
    return FailedPrecondition("File path can not be resolved: %s", file_path);
  }

  if (!tsl::Env::Default()->FileExists(resolved_path).ok()) {
    return FailedPrecondition("Autotune results file does not exist: %s",
                              resolved_path);
  }
  std::string autotune_results_str;
  TF_RETURN_IF_ERROR(tsl::ReadFileToString(tsl::Env::Default(), resolved_path,
                                           &autotune_results_str));

  TF_RETURN_IF_ERROR(LoadAutotuneResults(autotune_results_str,
                                         IsTextProtoPath(resolved_path)));

  LOG(INFO) << "Autotune results loaded from file: " << resolved_path;

  return absl::OkStatus();
}

/*static*/ std::unique_ptr<HloModule>
AutotunerUtil::ExtractInstructionIntoNewModule(const HloInstruction& hlo) {
  auto new_hlo_module = std::make_unique<HloModule>(
      "extracted", HloModuleConfig{},
      std::make_unique<CompilationEnvironments>(hlo.GetModule()->comp_envs()));
  int parameter_number = 0;
  HloComputation::Builder builder("entry_computation");
  HloCloneContext clone_context(new_hlo_module.get());
  std::vector<HloInstruction*> new_operands;
  for (const HloInstruction* operand : hlo.operands()) {
    std::unique_ptr<HloInstruction> new_parameter =
        HloInstruction::CreateParameter(parameter_number, operand->shape(),
                                        operand->name());
    ++parameter_number;
    new_operands.push_back(builder.AddInstruction(std::move(new_parameter)));
  }
  std::unique_ptr<HloInstruction> new_instruction =
      hlo.CloneWithNewOperands(hlo.shape(), new_operands, &clone_context);
  builder.AddInstruction(std::move(new_instruction));
  new_hlo_module->AddEntryComputationWithLayouts(builder.Build());
  return new_hlo_module;
}

/*static*/ std::unique_ptr<HloModule>
AutotunerUtil::ExtractComputationIntoNewModule(
    const HloComputation& computation) {
  auto new_hlo_module =
      std::make_unique<HloModule>("extracted", HloModuleConfig{},
                                  std::make_unique<CompilationEnvironments>(
                                      computation.parent()->comp_envs()));
  HloCloneContext clone_context(new_hlo_module.get());
  new_hlo_module->AddEntryComputationWithLayouts(
      computation.CloneInContext(clone_context));
  return new_hlo_module;
}

/*static*/ absl::StatusOr<se::RedzoneAllocator>
AutotunerUtil::CreateRedzoneAllocator(const AutotuneConfig& config,
                                      const DebugOptions& opts,
                                      se::Stream* force_stream) {
  se::Stream* stream = force_stream;
  if (stream == nullptr) {
    TF_ASSIGN_OR_RETURN(stream, config.GetStream());
  }
  return se::RedzoneAllocator(
      stream, config.GetAllocator(), PtxOptsFromDebugOptions(opts),
      /*memory_limit=*/std::numeric_limits<int64_t>::max(),
      /*redzone_size=*/config.should_check_correctness()
          ? opts.xla_gpu_redzone_padding_bytes()
          : 0);
}

}  // namespace gpu
}  // namespace xla
