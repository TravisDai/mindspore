/**
 * Copyright 2021 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "backend/kernel_compiler/cpu/masked_select_cpu_kernel.h"
#include "runtime/device/cpu/cpu_device_address.h"

namespace mindspore {
namespace kernel {

template <typename T>
void MaskedSelectCPUKernel<T>::InitKernel(const CNodePtr &kernel_node) {
  size_t input_num = AnfAlgo::GetInputTensorNum(kernel_node);
  if (input_num != 2) {
    MS_LOG(EXCEPTION) << "Input number is " << input_num << ", but MaskedSelectCPUKernel needs 2 input.";
  }
  size_t output_num = AnfAlgo::GetOutputTensorNum(kernel_node);
  if (output_num != 1) {
    MS_LOG(EXCEPTION) << "Output number is " << output_num << ", but MaskedSelectCPUKernel needs 1 output.";
  }
  input_shape_a_ = AnfAlgo::GetInputDeviceShape(kernel_node, 0);
  input_shape_b_ = AnfAlgo::GetInputDeviceShape(kernel_node, 1);
  output_shape_ = CPUKernelUtils::GetBroadcastShape(input_shape_a_, input_shape_b_);
  for (const uint64_t &d : output_shape_) {
    tensor_size_ *= d;
  }
  node_wpt_ = kernel_node;
}

template <typename T>
bool MaskedSelectCPUKernel<T>::Launch(const std::vector<kernel::AddressPtr> &inputs,
                                      const std::vector<kernel::AddressPtr> &,
                                      const std::vector<kernel::AddressPtr> &outputs) {
  auto x = reinterpret_cast<T *>(inputs[0]->addr);
  auto mask = reinterpret_cast<bool *>(inputs[1]->addr);
  auto y = reinterpret_cast<T *>(outputs[0]->addr);
  uint64_t j = 0;
  if (input_shape_a_ == input_shape_b_) {
    for (uint64_t i = 0; i < tensor_size_; ++i) {
      if (mask[i]) {
        y[j++] = x[i];
      }
    }
  } else {  // Broadcast
    BroadcastIterator iter(input_shape_a_, input_shape_b_, output_shape_);
    iter.SetPos(0);
    for (uint64_t i = 0; i < tensor_size_; ++i) {
      if (mask[iter.GetInputPosB()]) {
        y[j++] = x[iter.GetInputPosA()];
      }
      iter.GenNextPos();
    }
  }
  if (!node_wpt_.expired()) {
    auto node_ = node_wpt_.lock();
    if (!node_) {
      MS_LOG(EXCEPTION) << "node_wpt_ is expired.";
    }
    std::vector<size_t> out_shape;
    out_shape.emplace_back(j);
    std::vector<TypeId> dtypes;
    size_t output_num = AnfAlgo::GetOutputTensorNum(node_);
    for (size_t i = 0; i < output_num; i++) {
      dtypes.push_back(AnfAlgo::GetOutputDeviceDataType(node_, i));
    }
    AnfAlgo::SetOutputInferTypeAndShape(dtypes, {out_shape}, node_.get());
  }
  return true;
}
}  // namespace kernel
}  // namespace mindspore
