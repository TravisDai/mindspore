/**
 * Copyright 2020-2021 Huawei Technologies Co., Ltd
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

#include "src/delegate/npu/op/cast_npu.h"
#include "src/delegate/npu/npu_converter_utils.h"

namespace mindspore {
int CastNPUOp::IsSupport(const schema::Primitive *primitive, const std::vector<tensor::MSTensor *> &in_tensors,
                         const std::vector<tensor::MSTensor *> &out_tensors) {
  if (in_tensors.size() >= 2 && in_tensors[1]->ElementsNum() == 1) {
    dst_type_ = static_cast<int *>(in_tensors[1]->data())[0];
  } else {
    MS_LOG(WARNING) << "NPU dst dtype is attribute.";
    return RET_NOT_SUPPORT;
  }
  return RET_OK;
}

int CastNPUOp::Init(const schema::Primitive *primitive, const std::vector<tensor::MSTensor *> &in_tensors,
                    const std::vector<tensor::MSTensor *> &out_tensors) {
  cast_ = new (std::nothrow) hiai::op::CastT(name_);
  if (cast_ == nullptr) {
    MS_LOG(ERROR) << name_ << " op is nullptr";
    return RET_ERROR;
  }
  cast_->set_attr_dst_dtype(ConverterToNPUDataType(static_cast<TypeId>(dst_type_)));
  cast_->set_attr_src_dtype(ConverterToNPUDataType(static_cast<TypeId>(in_tensors[0]->data_type())));
  return RET_OK;
}

int CastNPUOp::SetNPUInputs(const std::vector<tensor::MSTensor *> &in_tensors,
                            const std::vector<tensor::MSTensor *> &out_tensors,
                            const std::vector<ge::Operator *> &npu_inputs) {
  cast_->set_input_x(*npu_inputs[0]);
  return RET_OK;
}

ge::Operator *CastNPUOp::GetNPUOp() { return this->cast_; }

CastNPUOp::~CastNPUOp() {
  if (cast_ != nullptr) {
    delete cast_;
    cast_ = nullptr;
  }
}
}  // namespace mindspore
