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

#ifndef MINDSPORE_LITE_SRC_RUNTIME_DELEGATE_NPU_NPU_GRAPH_H_
#define MINDSPORE_LITE_SRC_RUNTIME_DELEGATE_NPU_NPU_GRAPH_H_

#include <vector>
#include <map>
#include <utility>
#include "include/kernel.h"
#include "src/delegate/npu/op/npu_op.h"
#include "src/delegate/npu/npu_executor.h"

namespace mindspore {
class NPUGraph : public kernel::Kernel {
 public:
  NPUGraph(std::vector<NPUOp *> npu_ops, NPUManager *npu_manager, const std::vector<tensor::MSTensor *> &inputs,
           const std::vector<tensor::MSTensor *> &outputs)
      : kernel::Kernel(inputs, outputs, nullptr, nullptr), npu_ops_(std::move(npu_ops)), npu_manager_(npu_manager) {}

  ~NPUGraph() override;

  int Init();

  int Prepare() override;

  int Execute() override;

  int ReSize() override {
    MS_LOG(ERROR) << "NPU does not support the resize function temporarily.";
    return lite::RET_ERROR;
  }

  void set_input(tensor::MSTensor *in_tensor, int index) override;

  void set_output(tensor::MSTensor *out_tensor, int index) override;

  int FindPreNextOps();

  std::vector<NPUOp *> *GetOps() { return &npu_ops_; }

  std::vector<tensor::MSTensor *> *GetInsertTensors() { return &insert_tensors_; }

 protected:
  std::vector<NPUOp *> FindPreOps(NPUOp *cur_op);

  std::vector<NPUOp *> FindNextOps(NPUOp *cur_op);

  std::vector<NPUOp *> FindSubgraphOps(NPUOp *head_op, std::map<const NPUOp *, bool> *is_visited);

  kernel::Kernel *CreateNPUSubgraphKernel(std::vector<NPUOp *> ops);

  kernel::Kernel *CreateNPUTransposeKernel(NPUOp *op);

  std::vector<NPUOp *> npu_ops_{};

  std::vector<kernel::Kernel *> all_kernels_{};

  std::vector<tensor::MSTensor *> insert_tensors_;

  NPUManager *npu_manager_ = nullptr;
};

}  // namespace mindspore

#endif  // MINDSPORE_LITE_SRC_RUNTIME_DELEGATE_NPU_NPU_GRAPH_H_
