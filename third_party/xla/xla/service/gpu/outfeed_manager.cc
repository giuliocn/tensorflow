/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

#include "xla/service/gpu/outfeed_manager.h"

#include <memory>

#include "absl/status/status.h"
#include "xla/map_util.h"
#include "xla/shape_util.h"
#include "tsl/platform/logging.h"

#if GOOGLE_CUDA || TENSORFLOW_USE_ROCM
#include "xla/service/gpu/xla_executor_state.h"
#include "xla/stream_executor/gpu/gpu_executor.h"
#endif  // GOOGLE_CUDA || TENSORFLOW_USE_ROCM

namespace xla {
namespace gpu {

OutfeedManager *GetOrCreateOutfeedManager(se::StreamExecutor *executor) {
#if GOOGLE_CUDA || TENSORFLOW_USE_ROCM
  stream_executor::gpu::GpuExecutor *gpu_executor =
      stream_executor::gpu::ExtractGpuExecutor(executor);
  auto *xla_state =
      gpu_executor->getOrCreateXLAState<GpuExecutorXLAState>(executor);
  return xla_state->getOrCreateOutfeedManager(executor);
#else   // GOOGLE_CUDA || TENSORFLOW_USE_ROCM
  return nullptr;
#endif  // GOOGLE_CUDA || TENSORFLOW_USE_ROCM
}

absl::Status OutfeedManager::TransferLiteralFromOutfeed(
    se::StreamExecutor* executor, MutableBorrowingLiteral literal) {
  ShapeTree<std::unique_ptr<gpu::OutfeedBuffer>> outfeed_buffers(
      &literal.shape());

  for (auto& leaf : outfeed_buffers.leaves()) {
    const Shape& shape = ShapeUtil::GetSubshape(literal.shape(), leaf.first);
    CHECK(shape.IsArray()) << ShapeUtil::HumanStringWithLayout(shape);
    leaf.second =
        std::make_unique<gpu::OutfeedBuffer>(ShapeUtil::ByteSizeOf(shape));
    leaf.second->set_destination(
        std::make_unique<MutableBorrowingLiteral>(literal, leaf.first));
  }

  // Give the tree of buffers to the outfeed manager. The device will fill it
  // while we're waiting for it below.
  gpu::OutfeedManager* outfeed_manager =
      gpu::GetOrCreateOutfeedManager(executor);
  outfeed_manager->EnqueueDestination(&outfeed_buffers);

  // Now wait till all the buffers are written.
  for (auto& leaf : outfeed_buffers.leaves()) {
    const Shape& shape = ShapeUtil::GetSubshape(literal.shape(), leaf.first);
    CHECK(shape.IsArray()) << ShapeUtil::HumanStringWithLayout(shape);
    leaf.second->WaitUntilAvailable();
  }

  return absl::OkStatus();
}

}  // namespace gpu
}  // namespace xla
