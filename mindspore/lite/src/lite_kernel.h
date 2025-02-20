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

#ifndef MINDSPORE_LITE_SRC_LITE_KERNEL_H_
#define MINDSPORE_LITE_SRC_LITE_KERNEL_H_
#include <string>
#include <vector>
#include <memory>
#include <utility>
#include <algorithm>
#include "src/common/utils.h"
#include "src/common/log_util.h"
#ifdef ENABLE_ARM
#include <arm_neon.h>
#endif
#include "nnacl/op_base.h"
#include "src/inner_context.h"
#include "src/tensor.h"
#include "include/errorcode.h"
#include "schema/model_generated.h"
#include "include/context.h"
#include "include/kernel.h"
#include "src/inner_kernel.h"
#include "include/delegate.h"

namespace mindspore::kernel {
enum KERNEL_ARCH { kCPU, kGPU, kAPU, kNPU, kCustom, kDelegate, kKernelArch_MIN = kCPU, kKernelArch_MAX = kAPU };
static const char *const kBuiltin = "Builtin";

struct KernelKey {
  KERNEL_ARCH arch = kCPU;
  TypeId data_type = kTypeUnknown;
  int type = 0;
  std::string kernel_arch;
  std::string provider{kBuiltin};
  std::shared_ptr<Delegate> delegate = nullptr;

  bool operator<(const KernelKey &dst) const {
    if (provider != dst.provider) {
      return provider < dst.provider;
    } else if (kernel_arch != dst.kernel_arch) {
      return kernel_arch < dst.kernel_arch;
    } else if (arch != dst.arch) {
      return arch < dst.arch;
    } else if (data_type != dst.data_type) {
      return data_type < dst.data_type;
    } else {
      return type < dst.type;
    }
  }
};

enum SubGraphType {
  kNotSubGraph = 0,
  kCpuFP32SubGraph,
  kCpuFP16SubGraph,
  kGpuSubGraph,
  kNpuSubGraph,
  kApuSubGraph,
  kCustomSubGraph
};

class LiteKernel {
 public:
  LiteKernel() {
    this->in_kernels_.clear();
    this->out_kernels_.clear();
  }

  explicit LiteKernel(std::shared_ptr<Kernel> kernel) : kernel_(kernel) {
    this->in_kernels_.clear();
    this->out_kernels_.clear();
  }

  virtual ~LiteKernel() = default;

  virtual int Execute() { return Execute(nullptr, nullptr); }

  virtual int Execute(const KernelCallBack &before, const KernelCallBack &after) {
    if (before != nullptr) {
      if (!before(TensorVectorCast(this->in_tensors()), TensorVectorCast(this->out_tensors()),
                  {this->name(), schema::EnumNamePrimitiveType(this->type())})) {
        MS_LOG(WARNING) << "run kernel before_callback failed, name: " << this->name();
      }
    }

    auto ret = kernel_->Execute();
    if ((ret == lite::RET_OK) && (desc_.provider != kBuiltin)) {
      for (auto *output : this->out_tensors()) {
        MS_ASSERT(output != nullptr);
        output->ResetRefCount();
      }
      for (auto &in_tensor : this->in_tensors()) {
        MS_ASSERT(in_tensor != nullptr);
        if (in_tensor->root_tensor() == in_tensor) {
          continue;
        }
        in_tensor->DecRefCount();
      }
    }

    if (after != nullptr) {
      if (!after(TensorVectorCast(this->in_tensors()), TensorVectorCast(this->out_tensors()),
                 {this->name(), schema::EnumNamePrimitiveType(this->type())})) {
        MS_LOG(WARNING) << "run kernel after_callback failed, name: " << this->name();
      }
    }
    return ret;
  }

  // called while compiling graph
  virtual int Prepare() {
    MS_ASSERT(kernel_ != nullptr);
    return kernel_->Prepare();
  }

  virtual int Init() {
    MS_ASSERT(kernel_ != nullptr);
    if (desc_.provider == kBuiltin) {
      return std::static_pointer_cast<InnerKernel>(kernel_)->Init();
    }
    return mindspore::lite::RET_OK;
  }

  virtual int ReSize() {
    MS_ASSERT(kernel_ != nullptr);
    return kernel_->ReSize();
  }

  virtual void FindInoutKernels(const std::vector<kernel::LiteKernel *> &scope_kernels);

  OpParameter *op_parameter() const {
    MS_ASSERT(kernel_ != nullptr);
    if (desc_.provider == kBuiltin) {
      return std::static_pointer_cast<InnerKernel>(kernel_)->op_parameter();
    }
    return nullptr;
  }

  std::string name() const {
    MS_ASSERT(kernel_ != nullptr);
    return kernel_->name();
  }

  void set_name(const std::string &name) {
    MS_ASSERT(kernel_ != nullptr);
    kernel_->set_name(name);
  }

  virtual int Train() {
    MS_ASSERT(kernel_ != nullptr);
    if (desc_.provider == kBuiltin) {
      return std::static_pointer_cast<InnerKernel>(kernel_)->Train();
    }
    return mindspore::lite::RET_OK;
  }

  virtual bool IsTrain() const {
    MS_ASSERT(kernel_ != nullptr);
    if (desc_.provider == kBuiltin) {
      return std::static_pointer_cast<InnerKernel>(kernel_)->IsTrain();
    }
    return false;
  }

  virtual int Eval() {
    MS_ASSERT(kernel_ != nullptr);
    if (desc_.provider == kBuiltin) {
      return std::static_pointer_cast<InnerKernel>(kernel_)->Eval();
    }
    return mindspore::lite::RET_OK;
  }

  virtual bool IsEval() const {
    MS_ASSERT(kernel_ != nullptr);
    if (desc_.provider == kBuiltin) {
      return std::static_pointer_cast<InnerKernel>(kernel_)->IsEval();
    }
    return false;
  }

  virtual void SetTrainable(bool trainable = true) {
    MS_ASSERT(kernel_ != nullptr);
    if (desc_.provider == kBuiltin) {
      std::static_pointer_cast<InnerKernel>(kernel_)->SetTrainable(trainable);
    }
  }

  virtual bool IsTrainable() const {
    MS_ASSERT(kernel_ != nullptr);
    if (desc_.provider == kBuiltin) {
      return std::static_pointer_cast<InnerKernel>(kernel_)->IsTrainable();
    }
    return false;
  }

  void set_is_model_output(bool is_model_output) { this->is_model_output_ = is_model_output; }

  bool is_model_output() const { return this->is_model_output_; }

  bool InferShapeDone() const {
    auto shape = out_tensors().front()->shape();
    if (std::find(shape.begin(), shape.end(), -1) != shape.end()) {
      return false;
    }
    return true;
  }

  schema::PrimitiveType type() const {
    MS_ASSERT(kernel_ != nullptr);
    return kernel_->type();
  }

  std::string type_str() const { return schema::EnumNamePrimitiveType(this->type()); }

  void set_in_tensors(const std::vector<lite::Tensor *> &in_tensors) {
    MS_ASSERT(kernel_ != nullptr);
    if (desc_.provider == kBuiltin) {
      std::static_pointer_cast<InnerKernel>(kernel_)->set_in_tensors(in_tensors);
    } else {
      std::vector<mindspore::tensor::MSTensor *> ms_tensors(in_tensors.begin(), in_tensors.end());
      kernel_->set_inputs(ms_tensors);
    }
  }

  void set_in_tensor(lite::Tensor *in_tensor, int index) {
    MS_ASSERT(kernel_ != nullptr);
    if (desc_.provider == kBuiltin) {
      std::static_pointer_cast<InnerKernel>(kernel_)->set_in_tensor(in_tensor, index);
    } else {
      MS_ASSERT(index < kernel_->inputs().size());
      mindspore::tensor::MSTensor *ms_tensors(in_tensor);
      kernel_->set_input(ms_tensors, index);
    }
  }

  void set_out_tensors(const std::vector<lite::Tensor *> &out_tensors) {
    MS_ASSERT(kernel_ != nullptr);
    if (desc_.provider == kBuiltin) {
      std::static_pointer_cast<InnerKernel>(kernel_)->set_out_tensors(out_tensors);
    } else {
      std::vector<mindspore::tensor::MSTensor *> ms_tensors(out_tensors.begin(), out_tensors.end());
      kernel_->set_outputs(ms_tensors);
    }
  }

  virtual void set_out_tensor(lite::Tensor *out_tensor, int index) {
    MS_ASSERT(kernel_ != nullptr);
    if (desc_.provider == kBuiltin) {
      std::static_pointer_cast<InnerKernel>(kernel_)->set_out_tensor(out_tensor, index);
    } else {
      MS_ASSERT(index < kernel_->outputs().size());
      mindspore::tensor::MSTensor *ms_tensors(out_tensor);
      kernel_->set_output(ms_tensors, index);
    }
  }

  const std::vector<lite::Tensor *> &in_tensors() const {
    MS_ASSERT(kernel_ != nullptr);
    if (desc_.provider == kBuiltin) {
      return std::static_pointer_cast<InnerKernel>(kernel_)->in_tensors();
    } else {
      auto &ms_tensors = kernel_->inputs();
      mutable_in_tensors_.resize(ms_tensors.size());
      (void)std::transform(ms_tensors.begin(), ms_tensors.end(), mutable_in_tensors_.begin(),
                           [](mindspore::tensor::MSTensor *tensor) { return static_cast<lite::Tensor *>(tensor); });

      return mutable_in_tensors_;
    }
  }

  const std::vector<lite::Tensor *> &out_tensors() const {
    MS_ASSERT(kernel_ != nullptr);
    if (desc_.provider == kBuiltin) {
      return std::static_pointer_cast<InnerKernel>(kernel_)->out_tensors();
    } else {
      auto &ms_tensors = kernel_->outputs();
      mutable_out_tensors_.resize(ms_tensors.size());
      (void)std::transform(ms_tensors.begin(), ms_tensors.end(), mutable_out_tensors_.begin(),
                           [](mindspore::tensor::MSTensor *tensor) { return static_cast<lite::Tensor *>(tensor); });
      return mutable_out_tensors_;
    }
  }

  void AddInKernel(LiteKernel *kernel) {
    if (!lite::IsContain(this->in_kernels_, kernel)) {
      this->in_kernels_.emplace_back(kernel);
    }
  }

  void AddOutKernel(LiteKernel *kernel) {
    if (!lite::IsContain(this->out_kernels_, kernel)) {
      this->out_kernels_.emplace_back(kernel);
    }
  }

  void set_in_kernels(const std::vector<LiteKernel *> &kernel) { this->in_kernels_ = kernel; }

  void set_out_kernels(const std::vector<LiteKernel *> &kernel) { this->out_kernels_ = kernel; }

  const std::vector<LiteKernel *> &in_kernels() const { return this->in_kernels_; }

  const std::vector<LiteKernel *> &out_kernels() const { return this->out_kernels_; }

  virtual bool IsReady(const std::vector<lite::Tensor *> &in_tensor);

  virtual void InitOutTensorInitRefCount();

  KernelKey desc() const { return desc_; }

  void set_desc(const KernelKey kernel_key) { desc_ = kernel_key; }

  SubGraphType subgraph_type() const { return this->subgraph_type_; }

  const lite::InnerContext *Context() const {
    MS_ASSERT(kernel_ != nullptr);
    return static_cast<const lite::InnerContext *>(kernel_->context());
  }

  virtual std::string ToString() const;

  Kernel *kernel() { return kernel_.get(); }

 protected:
  std::shared_ptr<Kernel> kernel_ = nullptr;
  KernelKey desc_;
  // tensor will free in ~lite_session()
  std::vector<LiteKernel *> in_kernels_;
  std::vector<LiteKernel *> out_kernels_;
  mutable std::vector<lite::Tensor *> mutable_in_tensors_;
  mutable std::vector<lite::Tensor *> mutable_out_tensors_;
  bool is_model_output_ = false;
  SubGraphType subgraph_type_ = kNotSubGraph;
};

typedef InnerKernel *(*KernelCreator)(const std::vector<lite::Tensor *> &inputs,
                                      const std::vector<lite::Tensor *> &outputs, OpParameter *parameter,
                                      const lite::Context *ctx, const KernelKey &desc);

template <class T>
kernel::InnerKernel *LiteKernelCreator(const std::vector<lite::Tensor *> &inputs,
                                       const std::vector<lite::Tensor *> &outputs, OpParameter *parameter,
                                       const lite::Context *ctx, const kernel::KernelKey &desc) {
  if (parameter == nullptr) {
    MS_LOG(ERROR) << "parameter is nullptr.";
    return nullptr;
  }
  auto *kernel = new (std::nothrow) T(parameter, inputs, outputs, static_cast<const lite::InnerContext *>(ctx));
  if (kernel == nullptr) {
    MS_LOG(ERROR) << "kernel: " << parameter->name_ << "is nullptr.";
    free(parameter);
    return nullptr;
  }
  return kernel;
}
}  // namespace mindspore::kernel

#endif  // MINDSPORE_LITE_SRC_INNER_KERNEL_H_
