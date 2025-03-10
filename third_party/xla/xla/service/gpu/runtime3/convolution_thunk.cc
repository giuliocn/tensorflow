/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#include "xla/service/gpu/runtime3/convolution_thunk.h"

#include <memory>
#include <optional>

#include "absl/status/status.h"
#include "xla/service/gpu/gpu_conv_runner.h"
#include "xla/stream_executor/stream_executor.h"
#include "xla/util.h"

namespace xla {
namespace gpu {

ConvolutionThunk::ConvolutionThunk(
    ThunkInfo thunk_info, GpuConvConfig config,
    std::vector<BufferAllocation::Slice> operand_slices,
    std::vector<BufferAllocation::Slice> result_slices,
    BufferAllocation::Slice scratch_slice)
    : Thunk(Kind::kConvolution, thunk_info),
      operand_buffers_(std::move(operand_slices)),
      result_buffers_(std::move(result_slices)),
      scratch_buffer_(scratch_slice),
      config_(std::move(config)) {}

GenericConvRunner& ConvolutionThunk::GetOrCreateRunner(
    const stream_executor::Stream* stream) {
  absl::MutexLock lock(&mu_);
  auto it = runner_cache_.find(stream);
  if (it == runner_cache_.end()) {
    it = runner_cache_
             .insert({stream, std::make_unique<GenericConvRunner>(config_)})
             .first;
  }
  return *it->second;
}

absl::Status ConvolutionThunk::ExecuteOnStream(const ExecuteParams& params) {
  const auto& buffer_allocations = *params.buffer_allocations;

  std::vector<se::DeviceMemoryBase> operand_se_buffers, result_se_buffers;
  operand_se_buffers.reserve(operand_buffers_.size());
  for (BufferAllocation::Slice buffer : operand_buffers_) {
    operand_se_buffers.push_back(buffer_allocations.GetDeviceAddress(buffer));
  }

  result_se_buffers.reserve(result_buffers_.size());
  for (BufferAllocation::Slice buffer : result_buffers_) {
    result_se_buffers.push_back(buffer_allocations.GetDeviceAddress(buffer));
  }

  se::DeviceMemoryBase scratch =
      buffer_allocations.GetDeviceAddress(scratch_buffer_);

  RunConvOptions opts;
  opts.runner_cache = &GetOrCreateRunner(params.stream);

  TF_RETURN_IF_ERROR(RunGpuConv(config_, absl::MakeSpan(operand_se_buffers),
                                absl::MakeSpan(result_se_buffers), scratch,
                                params.stream, opts));

  // Note: Convolution has a tuple buffer as an output, but we don't need to
  // populate it as no one should be reading from the tuple directly.
  if (!params.stream->ok()) {
    return InternalError("ConvolutionThunk::ExecuteOnStream failed.");
  }
  return absl::OkStatus();
}

ConvolutionReorderThunk::ConvolutionReorderThunk(
    ThunkInfo thunk_info, absl::Span<int64_t> filter_nchw,
    std::vector<BufferAllocation::Slice> operand_slices,
    std::vector<BufferAllocation::Slice> result_slices)
    : Thunk(Kind::kConvolutionReorder, thunk_info),
      filter_descriptor_(CreateFilterDescriptor(filter_nchw)),
      operand_buffers_(std::move(operand_slices)),
      result_buffers_(std::move(result_slices)) {}

absl::Status ConvolutionReorderThunk::ExecuteOnStream(
    const ExecuteParams& params) {
  bool has_bias = operand_buffers_.size() > 1;
  CHECK_EQ(operand_buffers_.size(), result_buffers_.size());

  const auto& buffer_allocations = *params.buffer_allocations;

  auto filter_input = se::DeviceMemory<int8_t>(
      buffer_allocations.GetDeviceAddress(operand_buffers_[0]));
  auto filter_output = se::DeviceMemory<int8_t>(
      buffer_allocations.GetDeviceAddress(result_buffers_[0]));
  auto bias_input =
      has_bias ? std::make_optional(se::DeviceMemory<float>(
                     buffer_allocations.GetDeviceAddress(operand_buffers_[1])))
               : std::nullopt;
  auto bias_output =
      has_bias ? std::make_optional(se::DeviceMemory<float>(
                     buffer_allocations.GetDeviceAddress(result_buffers_[1])))
               : std::nullopt;

  TF_RETURN_IF_ERROR(params.stream->CudnnReorderConvolutionFilterAndBias(
      filter_descriptor_, filter_input, &filter_output, std::move(bias_input),
      std::move(bias_output)));

  if (!params.stream->ok()) {
    return InternalError("ConvolutionReorderThunk::ExecuteOnStream failed.");
  }
  return absl::OkStatus();
}

se::dnn::FilterDescriptor ConvolutionReorderThunk::CreateFilterDescriptor(
    absl::Span<int64_t> filter_nchw) {
  CHECK_EQ(filter_nchw.size(), 4);
  se::dnn::FilterDescriptor filter_desc(2);
  filter_desc.set_layout(se::dnn::FilterLayout::kOutputInputYX32);
  filter_desc.set_output_feature_map_count(filter_nchw[0]);
  filter_desc.set_input_feature_map_count(filter_nchw[1]);
  filter_desc.set_input_filter_height(filter_nchw[2]);
  filter_desc.set_input_filter_width(filter_nchw[3]);
  return filter_desc;
}

}  // namespace gpu
}  // namespace xla
