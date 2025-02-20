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

#include "src/runtime/kernel/arm/fp16_grad/pooling_fp16_grad.h"
#include "schema/model_generated.h"
#include "src/kernel_registry.h"
#include "nnacl/fp16/pooling_fp16.h"
#include "nnacl/fp16_grad/pooling_grad.h"
#include "include/errorcode.h"

using mindspore::kernel::KERNEL_ARCH::kCPU;
using mindspore::lite::KernelRegistrar;
using mindspore::lite::RET_ERROR;
using mindspore::lite::RET_OK;
using mindspore::schema::PrimitiveType_AvgPoolGrad;
using mindspore::schema::PrimitiveType_MaxPoolGrad;

namespace mindspore::kernel {
int PoolingGradCPUKernelFp16::ReSize() {
  PoolingParameter *pool_param = reinterpret_cast<PoolingParameter *>(op_parameter_);

  auto in_shape = in_tensors_.at(0)->shape();
  auto out_shape = in_tensors_.at(1)->shape();

  if (pool_param->pool_mode_ == PoolMode_AvgPool) {
    out_shape = in_tensors_.at(2)->shape();
  }

  int input_h = in_shape.at(1);
  int input_w = in_shape.at(2);

  if (pool_param->global_) {
    pool_param->window_w_ = input_w;
    pool_param->window_h_ = input_h;
  }

  pool_param->input_h_ = in_shape[kNHWC_H];
  pool_param->input_w_ = in_shape[kNHWC_W];
  pool_param->input_batch_ = in_shape[kNHWC_N];
  pool_param->input_channel_ = in_shape[kNHWC_C];
  pool_param->output_h_ = out_shape[kNHWC_H];
  pool_param->output_w_ = out_shape[kNHWC_W];
  pool_param->output_batch_ = out_shape[kNHWC_N];
  pool_param->output_channel_ = out_shape[kNHWC_C];

  return RET_OK;
}

int PoolingGradCPUKernelFp16::Init() { return ReSize(); }

int PoolingGradCPUKernelFp16::Execute(int task_id) {
  PoolingParameter *pool_param = reinterpret_cast<PoolingParameter *>(op_parameter_);
  auto input_ptr = reinterpret_cast<float16_t *>(in_tensors_.at(0)->data_c());
  auto output_ptr = reinterpret_cast<float16_t *>(out_tensors_.at(0)->data_c());
  int stride = UP_DIV(pool_param->output_batch_, thread_num_);
  int count = MSMIN(stride, pool_param->output_batch_ - stride * task_id);

  if (count > 0) {
    int in_batch_size = pool_param->input_h_ * pool_param->input_w_ * pool_param->input_channel_;
    int out_batch_size = pool_param->output_h_ * pool_param->output_w_ * pool_param->input_channel_;
    std::fill(output_ptr + task_id * stride * in_batch_size, output_ptr + ((task_id * stride) + count) * in_batch_size,
              0.f);
    if (pool_param->pool_mode_ == PoolMode_MaxPool) {
      auto dy_ptr = reinterpret_cast<float16_t *>(in_tensors_.at(2)->data_c());
      MaxPoolingFp16Grad(input_ptr + task_id * stride * in_batch_size, dy_ptr + task_id * stride * out_batch_size,
                         output_ptr + task_id * stride * in_batch_size, count, pool_param);
    } else {
      input_ptr = reinterpret_cast<float16_t *>(in_tensors_.at(2)->data_c());
      AvgPoolingFp16Grad(input_ptr + task_id * stride * out_batch_size, output_ptr + task_id * stride * in_batch_size,
                         count, pool_param);
    }
  }
  return RET_OK;
}

int PoolingFp16GradImpl(void *cdata, int task_id, float lhs_scale, float rhs_scale) {
  MS_ASSERT(cdata != nullptr);
  auto pooling = reinterpret_cast<PoolingGradCPUKernelFp16 *>(cdata);
  auto error_code = pooling->Execute(task_id);
  if (error_code != RET_OK) {
    MS_LOG(ERROR) << "Pooling Run error task_id[" << task_id << "] error_code[" << error_code << "]";
    return RET_ERROR;
  }
  return RET_OK;
}

int PoolingGradCPUKernelFp16::Run() {
  thread_num_ = context_->thread_num_;
  int error_code = ParallelLaunch(this->context_, PoolingFp16GradImpl, this, thread_num_);
  if (error_code != RET_OK) {
    MS_LOG(ERROR) << "pooling error error_code[" << error_code << "]";
    return RET_ERROR;
  }
  return RET_OK;
}

REG_KERNEL(kCPU, kNumberTypeFloat16, PrimitiveType_AvgPoolGrad, LiteKernelCreator<PoolingGradCPUKernelFp16>)
REG_KERNEL(kCPU, kNumberTypeFloat16, PrimitiveType_MaxPoolGrad, LiteKernelCreator<PoolingGradCPUKernelFp16>)
}  // namespace mindspore::kernel
