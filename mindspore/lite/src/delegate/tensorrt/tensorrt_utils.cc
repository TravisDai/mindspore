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

#include "src/delegate/tensorrt/tensorrt_utils.h"
#include <map>

namespace mindspore::lite {
nvinfer1::Dims ConvertCudaDims(const std::vector<int> &shape) {
  nvinfer1::Dims dims{};
  if (!shape.empty()) {
    dims.nbDims = shape.size();
    for (int i = 0; i < dims.nbDims; i++) {
      dims.d[i] = shape[i];
    }
  }
  return dims;
}
nvinfer1::Dims ConvertCudaDims(int data, size_t size) {
  nvinfer1::Dims dims{};
  dims.nbDims = size;
  for (size_t i = 0; i < size; i++) {
    dims.d[i] = data;
  }
  return dims;
}

nvinfer1::Dims ConvertCudaDims(void *data, size_t size) {
  nvinfer1::Dims dims{};
  dims.nbDims = size;
  int *dims_data = reinterpret_cast<int *>(data);
  for (size_t i = 0; i < size; i++) {
    dims.d[i] = *(dims_data + i);
  }
  return dims;
}

nvinfer1::IShuffleLayer *SetTranspose(nvinfer1::INetworkDefinition *network, const nvinfer1::ITensor &input,
                                      nvinfer1::Permutation permutation) {
  nvinfer1::IShuffleLayer *layer = network->addShuffle(const_cast<nvinfer1::ITensor &>(input));
  if (layer == nullptr) {
    MS_LOG(ERROR) << "failed to create ShuffleLayer when create transpose op.";
    return nullptr;
  }
  layer->setFirstTranspose(permutation);
  return layer;
}

nvinfer1::DataType ConvertDataType(TypeId type_id) {
  std::map<TypeId, nvinfer1::DataType> data_type_map = {{TypeId::kNumberTypeInt8, nvinfer1::DataType::kINT8},
                                                        {TypeId::kNumberTypeInt32, nvinfer1::DataType::kINT32},
                                                        {TypeId::kNumberTypeFloat32, nvinfer1::DataType::kFLOAT},
                                                        {TypeId::kNumberTypeFloat16, nvinfer1::DataType::kHALF}};
  auto iter = data_type_map.find(type_id);
  nvinfer1::DataType data_type;
  if (iter != data_type_map.end()) {
    data_type = iter->second;
  } else {
    data_type = nvinfer1::DataType::kFLOAT;
    MS_LOG(WARNING) << "invalid data_type for TensorRT, need check";
  }
  return data_type;
}

nvinfer1::IShuffleLayer *NHWC2NCHW(nvinfer1::INetworkDefinition *network, const nvinfer1::ITensor &input) {
  // NHWC 0123 NCHW 0312
  nvinfer1::Permutation perm{{0, 3, 1, 2}};
  return SetTranspose(network, input, perm);
}

nvinfer1::IShuffleLayer *NCHW2NHWC(nvinfer1::INetworkDefinition *network, const nvinfer1::ITensor &input) {
  // NCHW 0123 NHWC 0231
  nvinfer1::Permutation perm{{0, 2, 3, 1}};
  return SetTranspose(network, input, perm);
}

nvinfer1::ITensor *ConvertConstantTensor(nvinfer1::INetworkDefinition *network, tensor::MSTensor *ms_tensor) {
  if (network == nullptr) {
    MS_LOG(ERROR) << "network is null for ConvertConstantTensor";
    return nullptr;
  }
  nvinfer1::Dims dims = ConvertCudaDims(ms_tensor->shape());
  nvinfer1::DataType data_type = ConvertDataType(ms_tensor->data_type());

  nvinfer1::Weights weights{data_type, ms_tensor->data(), ms_tensor->ElementsNum()};
  nvinfer1::IConstantLayer *constant_tensor = network->addConstant(dims, weights);
  if (constant_tensor == nullptr) {
    MS_LOG(ERROR) << "create constant_tensor failed.";
    return nullptr;
  }
  auto name = ms_tensor->tensor_name() + "_constant_layer";
  constant_tensor->setName(name.c_str());
  return constant_tensor->getOutput(0);
}

nvinfer1::ITensor *ConvertScalarToITensor(nvinfer1::INetworkDefinition *network, size_t shape_size, void *value) {
  nvinfer1::Dims dims = ConvertCudaDims(1, shape_size);
  nvinfer1::Weights weights{nvinfer1::DataType::kFLOAT, value, 1};
  nvinfer1::IConstantLayer *constant_tensor = network->addConstant(dims, weights);
  if (constant_tensor == nullptr) {
    MS_LOG(ERROR) << "create constant_tensor failed.";
    return nullptr;
  }
  return constant_tensor->getOutput(0);
}

nvinfer1::ActivationType ConvertActivationType(schema::ActivationType activation_type) {
  std::map<schema::ActivationType, nvinfer1::ActivationType> action_map = {
    {schema::ActivationType_RELU, nvinfer1::ActivationType::kRELU},
    {schema::ActivationType_SIGMOID, nvinfer1::ActivationType::kSIGMOID},
    {schema::ActivationType_TANH, nvinfer1::ActivationType::kTANH},
    {schema::ActivationType_LEAKY_RELU, nvinfer1::ActivationType::kLEAKY_RELU},
    {schema::ActivationType_ELU, nvinfer1::ActivationType::kELU},
    {schema::ActivationType_SELU, nvinfer1::ActivationType::kSELU},
    {schema::ActivationType_SOFTSIGN, nvinfer1::ActivationType::kSOFTSIGN},
    {schema::ActivationType_SOFTPLUS, nvinfer1::ActivationType::kSOFTPLUS},
    {schema::ActivationType_THRESHOLDRELU, nvinfer1::ActivationType::kTHRESHOLDED_RELU}};
  auto iter = action_map.find(activation_type);
  nvinfer1::ActivationType action_code = nvinfer1::ActivationType::kRELU;
  if (iter != action_map.end()) {
    action_code = iter->second;
  } else {
    MS_LOG(WARNING) << "Unsupported op action type for TensorRT: " << activation_type;
  }
  return action_code;
}

nvinfer1::ITensor *ConvertTensorWithExpandDims(nvinfer1::INetworkDefinition *network, tensor::MSTensor *ms_tensor,
                                               size_t expand_shape_size) {
  if (network == nullptr) {
    MS_LOG(ERROR) << "network is null for ConvertConstantTensor";
    return nullptr;
  }
  std::vector<int> shape(expand_shape_size);
  size_t shape_size = ms_tensor->shape().size();
  size_t expand_size = expand_shape_size - shape_size;
  for (size_t i = 0; i < expand_shape_size; ++i) {
    if (i < expand_size) {
      shape[i] = 1;
    } else {
      shape[i] = ms_tensor->shape()[i - expand_size];
    }
  }
  nvinfer1::Dims dims = ConvertCudaDims(shape);
  nvinfer1::DataType data_type = ConvertDataType(ms_tensor->data_type());

  nvinfer1::Weights weights{data_type, ms_tensor->data(), ms_tensor->ElementsNum()};
  nvinfer1::IConstantLayer *constant_tensor = network->addConstant(dims, weights);
  if (constant_tensor == nullptr) {
    MS_LOG(ERROR) << "create constant_tensor failed.";
    return nullptr;
  }
  auto name = ms_tensor->tensor_name() + "_constant_layer";
  constant_tensor->setName(name.c_str());
  return constant_tensor->getOutput(0);
}
}  // namespace mindspore::lite
