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

#include "src/delegate/tensorrt/op/elementwise_tensorrt.h"
#include "src/delegate/tensorrt/tensorrt_utils.h"

namespace mindspore::lite {
int ElementWiseTensorRT::IsSupport(const schema::Primitive *primitive,
                                   const std::vector<tensor::MSTensor *> &in_tensors,
                                   const std::vector<tensor::MSTensor *> &out_tensors) {
  std::map<schema::PrimitiveType, nvinfer1::ElementWiseOperation> element_wise_ops = {
    {schema::PrimitiveType_AddFusion, nvinfer1::ElementWiseOperation::kSUM},
    {schema::PrimitiveType_PowFusion, nvinfer1::ElementWiseOperation::kPOW},
    {schema::PrimitiveType_DivFusion, nvinfer1::ElementWiseOperation::kDIV},
    {schema::PrimitiveType_SubFusion, nvinfer1::ElementWiseOperation::kSUB},
  };
  auto iter = element_wise_ops.find(this->type_);
  if (iter == element_wise_ops.end()) {
    MS_LOG(ERROR) << "invalid PrimitiveType for ElementWiseTensorRT, PrimitiveType: " << this->type_;
    return RET_ERROR;
  }
  element_wise_op_ = iter->second;
  if (in_tensors.size() != 2) {
    MS_LOG(ERROR) << "invalid input tensort size: " << in_tensors.size();
    return RET_ERROR;
  }
  if (out_tensors.size() != 1) {
    MS_LOG(ERROR) << "invalid output tensort size: " << out_tensors.size();
    return RET_ERROR;
  }
  return RET_OK;
}
int ElementWiseTensorRT::AddInnerOp(nvinfer1::INetworkDefinition *network) {
  if (network == nullptr) {
    MS_LOG(ERROR) << "network or input tensor size is invalid";
    return RET_ERROR;
  }
  // create ITensor from MS scalar
  if (this->in_tensors_[1]->shape().size() == 0) {
    nvinfer1::ITensor *scalar_input =
      lite::ConvertScalarToITensor(network, this->in_tensors_[0]->shape().size(), this->in_tensors_[1]->data());
    if (scalar_input == nullptr) {
      MS_LOG(ERROR) << "create Itensor from scalar failed";
      return RET_ERROR;
    }
    this->AddInnerInTensors(scalar_input);
  }
  // add elementwise
  if (this->tensorrt_in_tensors_.size() != 2) {
    MS_LOG(ERROR) << "invalid inner in tensors cnt: " << this->tensorrt_in_tensors_.size();
    return RET_ERROR;
  }
  nvinfer1::IElementWiseLayer *cal_layer =
    network->addElementWise(*tensorrt_in_tensors_[0], *tensorrt_in_tensors_[1], element_wise_op_);

  if (cal_layer == nullptr) {
    MS_LOG(ERROR) << "addElementWise failed for TensorRT.";
    return RET_ERROR;
  }
  cal_layer->setName(op_name_.c_str());

  nvinfer1::ITensor *op_out_tensor = cal_layer->getOutput(0);
  if (op_out_tensor == nullptr) {
    MS_LOG(ERROR) << "addElementWise out tensor is nullptr.";
    return RET_ERROR;
  }
  // add activation
  nvinfer1::ITensor *activation_out_tensor = AddActivation(network, op_out_tensor);
  op_out_tensor = (activation_out_tensor == nullptr) ? op_out_tensor : activation_out_tensor;

  // scale and shift
  if (element_wise_op_ == nvinfer1::ElementWiseOperation::kPOW) {
    auto pow_op = op_primitive_->value_as_PowFusion();
    if (pow_op == nullptr) {
      MS_LOG(ERROR) << "PowFusion convert failed.";
      return RET_ERROR;
    }
    float scale = pow_op->scale();
    float shift = pow_op->shift();
    if (abs(scale - 1) >= 1.0e-05 || abs(shift - 0) >= 1.0e-05) {
      MS_LOG(WARNING) << "deal with scale and shift for pow op";
    }
  }

  op_out_tensor->setName(out_tensors_[0]->tensor_name().c_str());
  this->AddInnerOutTensors(op_out_tensor);
  return RET_OK;
}

nvinfer1::ITensor *ElementWiseTensorRT::AddActivation(nvinfer1::INetworkDefinition *network,
                                                      nvinfer1::ITensor *in_tensor) {
  schema::ActivationType activation = schema::ActivationType::ActivationType_NO_ACTIVATION;
  switch (element_wise_op_) {
    case nvinfer1::ElementWiseOperation::kSUM: {
      auto sum_op = op_primitive_->value_as_AddFusion();
      if (sum_op == nullptr) {
        MS_LOG(ERROR) << "AddFusion convert failed.";
        return nullptr;
      }
      activation = sum_op->activation_type();
      break;
    }
    case nvinfer1::ElementWiseOperation::kDIV: {
      auto div_op = op_primitive_->value_as_DivFusion();
      if (div_op == nullptr) {
        MS_LOG(ERROR) << "DivFusion convert failed.";
        return nullptr;
      }
      activation = div_op->activation_type();
      break;
    }
    case nvinfer1::ElementWiseOperation::kSUB: {
      auto sub_op = op_primitive_->value_as_SubFusion();
      if (sub_op == nullptr) {
        MS_LOG(ERROR) << "SubFusion convert failed.";
        return nullptr;
      }
      activation = sub_op->activation_type();
      break;
    }
    default:
      MS_LOG(INFO) << "no activation need for: " << op_name_;
  }
  nvinfer1::ITensor *activation_out_tensor = nullptr;
  if (activation != schema::ActivationType::ActivationType_NO_ACTIVATION) {
    MS_LOG(WARNING) << "op: " << op_name_ << " has activation";
  }
  return activation_out_tensor;
}
}  // namespace mindspore::lite
