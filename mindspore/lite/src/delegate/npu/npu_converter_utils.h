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

#ifndef MINDSPORE_LITE_SRC_RUNTIME_DELEGATE_NPU_NPU_CONVERTER_UITLS_H_
#define MINDSPORE_LITE_SRC_RUNTIME_DELEGATE_NPU_NPU_CONVERTER_UITLS_H_
#include <string>
#include <memory>
#include <vector>
#include "schema/ops_generated.h"
#include "include/graph/tensor.h"
#include "include/graph/op/array_defs.h"
#include "include/ms_tensor.h"

namespace mindspore {

std::shared_ptr<ge::Tensor> ConverterToNPUTensor(tensor::MSTensor *src);

hiai::op::Data *ConverterToNPUData(tensor::MSTensor *src, const std::string &name);

ge::Format ConverterToNPUFormat(schema::Format format);

ge::DataType ConverterToNPUDataType(TypeId type_id);

ge::Shape ConverterToNPUShape(const std::vector<int> &src_shape);

int ConverterToNPUActMode(schema::ActivationType type);

int ConverterToNPUEltwiseMode(schema::EltwiseMode mode);

int TransFormAxis(int axis);

bool IsContainMSTensor(const std::vector<tensor::MSTensor *> &tensor_vec, const tensor::MSTensor *tensor);
}  // namespace mindspore
#endif  // MINDSPORE_LITE_SRC_RUNTIME_DELEGATE_NPU_NPU_CONVERTER_UITLS_H_
