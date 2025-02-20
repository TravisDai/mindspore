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

#include "fl/server/kernel/apply_momentum_kernel.h"

namespace mindspore {
namespace ps {
namespace server {
namespace kernel {
REG_OPTIMIZER_KERNEL(ApplyMomentum,
                     ParamsInfo()
                       .AddInputNameType(kWeight, kNumberTypeFloat32)
                       .AddInputNameType(kAccumulation, kNumberTypeFloat32)
                       .AddInputNameType(kLearningRate, kNumberTypeFloat32)
                       .AddInputNameType(kGradient, kNumberTypeFloat32)
                       .AddInputNameType(kMomentum, kNumberTypeFloat32),
                     ApplyMomentumKernel, float)
}  // namespace kernel
}  // namespace server
}  // namespace ps
}  // namespace mindspore
