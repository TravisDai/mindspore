/**
 * Copyright 2019-2021 Huawei Technologies Co., Ltd
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
#include "backend/session/ascend_session.h"
#include <algorithm>
#include <map>
#include <tuple>
#include <set>
#include <string>
#include <list>

#include "base/core_ops.h"
#include "base/base_ref_utils.h"
#include "ir/tensor.h"
#include "ir/anf.h"
#include "common/trans.h"
#include "runtime/device/kernel_runtime.h"
#include "runtime/device/ascend/kernel_select_ascend.h"
#include "runtime/device/ascend/kernel_build_ascend.h"
#include "runtime/device/ascend/ascend_kernel_runtime.h"
#include "runtime/device/ascend/profiling/profiling_manager.h"
#include "backend/optimizer/ascend/ascend_backend_optimization.h"
#include "backend/optimizer/common/common_backend_optimization.h"
#include "backend/optimizer/ascend/mindir/space_batch_nd_attr_update.h"
#include "backend/optimizer/ascend/mindir/dropout_unify_mindir.h"
#include "backend/optimizer/ascend/mindir/maxpool_to_maxpool_with_argmax.h"
#include "backend/optimizer/ascend/mindir/maxpool_with_argmax_unify_mindir.h"
#include "backend/optimizer/ascend/mindir/conv2d_unify_mindir.h"
#include "backend/optimizer/ascend/mindir/optimizer_unify_output.h"
#include "backend/optimizer/ascend/mindir/fake_learned_scale_quant_grad_unify_mindir.h"
#include "backend/optimizer/ascend/mindir/sparse_softmax_cross_entropy_with_logits_unify_mindir.h"
#include "backend/optimizer/ascend/mindir/slice_grad_unify_mindir.h"
#include "backend/optimizer/ascend/mindir/avg_pool_grad_unify_mindir.h"
#include "backend/optimizer/ascend/mindir/bn_grad_unify_mindir.h"
#include "runtime/device/kernel_adjust.h"
#include "runtime/device/ascend/ascend_stream_assign.h"
#include "backend/session/anf_runtime_algorithm.h"
#include "utils/ms_utils.h"
#include "utils/context/graph_kernel_flags.h"
#include "backend/optimizer/common/helper.h"
#include "runtime/device/kernel_runtime_manager.h"
#include "utils/config_manager.h"
#include "debug/data_dump/dump_json_parser.h"
#include "debug/tensor_load.h"
#include "debug/anf_ir_utils.h"
#include "backend/optimizer/graph_kernel/graph_kernel_optimization.h"
#include "backend/session/ascend_auto_monad.h"
#include "debug/data_dump/e2e_dump.h"
#include "debug/anf_ir_dump.h"
#include "debug/dump_proto.h"
#include "abstract/utils.h"
#ifdef ENABLE_DEBUGGER
#include "debug/debugger/proto_exporter.h"
#else
#include "debug/debugger/proto_exporter_stub.h"
#endif
#include "toolchain/adx_datadump_server.h"
#ifdef ENABLE_DUMP_IR
#include "debug/rdr/running_data_recorder.h"
#include "debug/rdr/recorder_manager.h"
#include "debug/rdr/graph_recorder.h"
#endif
#if ENABLE_CPU && ENABLE_D
#include "ps/util.h"
#include "ps/ps_cache/ps_cache_manager.h"
#endif
#include "runtime/device/ascend/ascend_bucket.h"
#include "profiler/device/common/memory_profiling.h"

using mindspore::device::ascend::ProfilingManager;
using mindspore::profiler::MemoryProfiling;

namespace mindspore {
namespace session {
const size_t kInvalidIndex = SIZE_MAX;
const size_t kLoopSinkTensorNum = 3;
const size_t kLoopSinkCurLoopIndex = 0;
const size_t kLoopSinkNextLoopIndex = 1;
const size_t kLoopSinkEpochIndex = 2;
constexpr char SR_TAG[] = "sr_tag";
constexpr char BACKWARD[] = "backward";
namespace {
void DumpGraphExeOrder(const std::vector<CNodePtr> &execution_order, const std::string &tag = "") {
  MS_LOG(INFO) << "Dump execution_order size " << execution_order.size();
  MS_LOG(INFO) << "[index][stream_label][graph_id][node string]";
  int i = 0;
  for (auto &cnode : execution_order) {
    MS_EXCEPTION_IF_NULL(cnode);
    MS_LOG(INFO) << "[ " << i << "]"
                 << "[" << AnfAlgo::GetStreamDistinctionLabel(cnode.get()) << "]"
                 << "[" << AnfAlgo::GetGraphId(cnode.get()) << "]"
                 << "[" << cnode->DebugString() << "]";
    i++;
  }

  std::stringstream buf;
  buf << "================== execution order ==================\n";
  if (!tag.empty()) {
    buf << tag << "\n";
  }
  buf << "execution_order size: " << execution_order.size() << "\n";
  i = 0;
  for (auto &cnode : execution_order) {
    MS_EXCEPTION_IF_NULL(cnode);
    buf << i << ":\n";
    buf << "\t" << cnode->DebugString() << "\n";
    buf << "\t" << AnfAlgo::GetStreamDistinctionLabel(cnode.get()) << "\n";
    buf << "\t" << AnfAlgo::GetGraphId(cnode.get()) << "\n";
    i++;
  }
  buf << "================== execution order ==================\n";
}

// Handle control flow by auto-monad.
void HandleControlFlow(NotNull<KernelGraphPtr> graph) {
  AscendAutoMonad auto_monad(graph);
  auto_monad.Run();
}

void SetStreamDistinctionLabel(const KernelGraphPtr &graph, uint32_t label, bool is_override) {
  MS_EXCEPTION_IF_NULL(graph);
  if (is_override || graph->stream_distinction_label() == kInvalidDistincLabel) {
    graph->set_stream_distinction_label(label);
  }
}

TensorPtr GetCNodeOutputStubTensor(const KernelWithIndex &kernel_with_index,
                                   const std::map<KernelWithIndex, OutputTensorInfo> &node_output_info,
                                   bool *output_is_weight) {
  MS_EXCEPTION_IF_NULL(output_is_weight);
  const auto &iter = node_output_info.find(kernel_with_index);
  if (iter == node_output_info.end()) {
    MS_LOG(EXCEPTION) << "Can not find output stub tensor of cnode " << kernel_with_index.first->DebugString();
  }
  *output_is_weight = iter->second.is_weight;
  return iter->second.output_stub_tensor;
}

void GenOpOutputStubTensor(const KernelGraphPtr &single_op_graph, const CNodePtr &kernel,
                           const std::map<KernelWithIndex, size_t> &cnode_refcount,
                           std::map<KernelWithIndex, OutputTensorInfo> *op_output_info) {
  MS_EXCEPTION_IF_NULL(single_op_graph);
  MS_EXCEPTION_IF_NULL(kernel);
  MS_EXCEPTION_IF_NULL(op_output_info);
  OutputTensorInfo output_tensor_info;
  size_t out_idx = 0;
  for (const auto &output : single_op_graph->outputs()) {
    KernelWithIndex kernel_with_index = std::make_pair(kernel, out_idx++);
    if (cnode_refcount.find(kernel_with_index) == cnode_refcount.end()) {
      continue;
    }
    const auto &output_kernel_with_index = AnfAlgo::VisitKernel(output, 0);
    const auto &output_node = output_kernel_with_index.first;
    const auto &output_index = output_kernel_with_index.second;
    auto out_abstract = output_node->abstract();
    MS_EXCEPTION_IF_NULL(out_abstract);
    if (out_abstract->isa<abstract::AbstractTuple>()) {
      out_abstract = out_abstract->cast<abstract::AbstractTuplePtr>()->elements()[output_index];
      MS_EXCEPTION_IF_NULL(out_abstract);
    }
    abstract::AbstractTensorPtr tensor_abstract = out_abstract->cast<abstract::AbstractTensorPtr>();
    MS_EXCEPTION_IF_NULL(tensor_abstract);
    const auto &infer_type = AnfAlgo::GetOutputInferDataType(output_node, output_index);
    tensor::TensorPtr stub_output_tensor =
      std::make_shared<tensor::Tensor>(infer_type, tensor_abstract->shape()->shape(), nullptr);
    const auto &output_type = AnfAlgo::GetOutputDeviceDataType(output_node, output_index);
    const auto &output_format = AnfAlgo::GetOutputFormat(output_node, output_index);
    tensor::DeviceInfo device_info;
    device_info.format_ = output_format;
    device_info.data_type_ = TypeIdToType(output_type);
    stub_output_tensor->set_device_info(device_info);
    device::DeviceAddressPtr device_address =
      std::make_shared<device::ascend::AscendDeviceAddress>(nullptr, 0, output_format, output_type);
    stub_output_tensor->set_device_address(device_address);
    output_tensor_info.output_stub_tensor = stub_output_tensor;
    auto kernel_info = dynamic_cast<const device::KernelInfo *>(output_node->kernel_info());
    MS_EXCEPTION_IF_NULL(kernel_info);
    output_tensor_info.is_weight = !(kernel_info->is_feature_map());
    (*op_output_info)[kernel_with_index] = output_tensor_info;
  }
}

size_t LoadCtrlInputTensor(const std::shared_ptr<KernelGraph> &graph, std::vector<tensor::TensorPtr> *inputs) {
  MS_EXCEPTION_IF_NULL(graph);
  MS_LOG(DEBUG) << "Load kInputCtrlTensors";
  auto inputs_params = graph->input_ctrl_tensors();
  if (inputs_params == nullptr) {
    return 0;
  }
  if (inputs_params->size() < kLoopSinkTensorNum) {
    MS_LOG(EXCEPTION) << "Illegal inputs_params size";
  }
  // update current loop tensor to 0 per iterator
  auto cur_loop_tensor = (*inputs_params)[kLoopSinkCurLoopIndex];
  MS_EXCEPTION_IF_NULL(cur_loop_tensor);
  auto *cur_val = static_cast<int32_t *>(cur_loop_tensor->data_c());
  MS_EXCEPTION_IF_NULL(cur_val);
  *cur_val = 0;
  cur_loop_tensor->set_sync_status(kNeedSyncHostToDevice);
  // set loop_count to zero
  MS_EXCEPTION_IF_NULL(inputs);
  inputs->push_back(cur_loop_tensor);

  // update next loop tensor to 0 per iterator
  auto next_loop_tensor = (*inputs_params)[kLoopSinkNextLoopIndex];
  MS_EXCEPTION_IF_NULL(next_loop_tensor);
  auto *next_val = static_cast<int32_t *>(next_loop_tensor->data_c());
  MS_EXCEPTION_IF_NULL(next_val);
  *next_val = 0;
  next_loop_tensor->set_sync_status(kNeedSyncHostToDevice);
  // set loop_count to zero
  MS_EXCEPTION_IF_NULL(inputs);
  inputs->push_back(next_loop_tensor);

  auto epoch_tensor = (*inputs_params)[kLoopSinkEpochIndex];
  MS_EXCEPTION_IF_NULL(epoch_tensor);
  auto *epoch_val = static_cast<int32_t *>(epoch_tensor->data_c());
  MS_EXCEPTION_IF_NULL(epoch_val);
  *epoch_val = graph->current_epoch();
  epoch_tensor->set_sync_status(kNeedSyncHostToDevice);
  inputs->push_back(epoch_tensor);
  MS_LOG(DEBUG) << "Load epoch_val:" << *epoch_val;
  graph->set_current_epoch(graph->current_epoch() + 1);
  return inputs_params->size();
}

bool TensorNeedSync(const AnfNodePtr &parameter, const tensor::TensorPtr &tensor) {
  auto ms_context = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(ms_context);
  auto device_address = AnfAlgo::GetMutableOutputAddr(parameter, 0);
  if (ms_context->get_param<bool>(MS_CTX_ENABLE_PYNATIVE_INFER)) {
    return tensor->device_address().get() == nullptr || tensor->device_address() != device_address;
  }
  if (tensor->NeedSyncHostToDevice()) {
    return true;
  }
  auto tensor_address = tensor->device_address();
  if (tensor_address != device_address) {
    tensor->data_sync(false);
    return true;
  }
  return false;
}
}  // namespace

void AscendSession::Init(uint32_t device_id) { InitExecutor(kAscendDevice, device_id); }

void AscendSession::UnifyMindIR(const KernelGraphPtr &graph) {
  SessionBasic::UnifyMindIR(graph);
  auto context_ptr = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(context_ptr);
  bool save_graphs = context_ptr->get_param<bool>(MS_CTX_SAVE_GRAPHS_FLAG);
  if (save_graphs) {
    std::string file_name = "hwopt_d_before_unify_mindir_graph_" + std::to_string(graph->graph_id()) + ".ir";
    DumpIR(file_name, graph);
    DumpIRProto(graph, "before_unify_mindir_hwopt_" + std::to_string(graph->graph_id()));
  }
  auto optimizer = std::make_shared<opt::GraphOptimizer>();
  auto unify_mindir_pm = std::make_shared<opt::PassManager>("unify_mindir_pm");
  unify_mindir_pm->AddPass(std::make_shared<opt::SpaceToBatchNDAttrUpdate>());
  unify_mindir_pm->AddPass(std::make_shared<opt::BatchToSpaceNDAttrUpdate>());
  unify_mindir_pm->AddPass(std::make_shared<opt::MaxPool2MaxPoolWithArgmax>());
  unify_mindir_pm->AddPass(std::make_shared<opt::MaxPoolWithArgmaxUnifyMindIR>());
  unify_mindir_pm->AddPass(std::make_shared<opt::MaxPoolGradWithArgmaxUnifyMindIR>());
  unify_mindir_pm->AddPass(std::make_shared<opt::Conv2DUnifyMindIR>());
  unify_mindir_pm->AddPass(std::make_shared<opt::Conv2DBackpropInputUnifyMindIR>());
  unify_mindir_pm->AddPass(std::make_shared<opt::Conv2DBackpropFilterUnifyMindIR>());
  unify_mindir_pm->AddPass(std::make_shared<opt::SliceGradUnifyMindIR>());
  unify_mindir_pm->AddPass(std::make_shared<opt::AvgPoolGradUnifyMindIR>());
  unify_mindir_pm->AddPass(std::make_shared<opt::FtrlUnifyOutput>());
  unify_mindir_pm->AddPass(std::make_shared<opt::MomentumUnifyOutput>());
  unify_mindir_pm->AddPass(std::make_shared<opt::RMSPropUnifyOutput>());
  unify_mindir_pm->AddPass(std::make_shared<opt::CenteredRMSPropUnifyOutput>());
  unify_mindir_pm->AddPass(std::make_shared<opt::FakeLearnedScaleQuantPerLayerGradUnifyMindIR>());
  unify_mindir_pm->AddPass(std::make_shared<opt::FakeLearnedScaleQuantPerChannelGradUnifyMindIR>());
  auto ms_context = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(ms_context);
  if (ms_context->get_param<int>(MS_CTX_EXECUTION_MODE) == kGraphMode) {
    unify_mindir_pm->AddPass(std::make_shared<opt::DropoutAndDropoutGradUnifyMindIR>());
    unify_mindir_pm->AddPass(std::make_shared<opt::DropoutUnifyMindIR0>());
    unify_mindir_pm->AddPass(std::make_shared<opt::GradSparseSoftmaxCrossEntropyWithLogitsUnifyMindIR>());
    unify_mindir_pm->AddPass(std::make_shared<opt::GradSparseSoftmaxCrossEntropyWithLogitsUnifyMindIRV2>());
    unify_mindir_pm->AddPass(std::make_shared<opt::SparseSoftmaxCrossEntropyWithLogitsUnifyMindIR>());
  } else {
    // Add PynativeGradSparseSoftmaxCrossEntropyWithLogitsUnifyMindIR pass first to avoid the backward loss function
    // from the python frontend matching the pattern defined in PynativeSparseSoftmaxCrossEntropyWithLogitsUnifyMindIR.
    unify_mindir_pm->AddPass(std::make_shared<opt::PynativeGradSparseSoftmaxCrossEntropyWithLogitsUnifyMindIR>());
    unify_mindir_pm->AddPass(std::make_shared<opt::PynativeSparseSoftmaxCrossEntropyWithLogitsUnifyMindIR>());
  }
  unify_mindir_pm->AddPass(std::make_shared<opt::DropoutUnifyMindIR1>());
  unify_mindir_pm->AddPass(std::make_shared<opt::DropoutGradUnifyMindIR>());
  unify_mindir_pm->AddPass(std::make_shared<opt::BatchNormGradUnifyMindIR>());

  optimizer->AddPassManager(unify_mindir_pm);
  (void)optimizer->Optimize(graph);
  graph->SetExecOrderByDefault();
  if (save_graphs) {
    std::string file_name = "hwopt_d_after_unify_mindir_graph_" + std::to_string(graph->graph_id()) + ".ir";
    DumpIR(file_name, graph);
  }
}

void AscendSession::LoadInputData(const std::shared_ptr<KernelGraph> &kernel_graph,
                                  const std::vector<tensor::TensorPtr> &inputs_const) const {
  std::vector<tensor::TensorPtr> inputs(inputs_const);
  size_t input_ctrl_size = kLoopSinkTensorNum;
  MS_EXCEPTION_IF_NULL(kernel_graph);
  if (kernel_graph->input_ctrl_tensors()) {
    input_ctrl_size = LoadCtrlInputTensor(kernel_graph, &inputs);
  }
  auto &input_nodes = kernel_graph->input_nodes();
  if ((inputs.size() + input_ctrl_size) - kLoopSinkTensorNum != input_nodes.size()) {
    MS_LOG(EXCEPTION) << "Tensor input:" << inputs.size() << " is not equal graph inputs:" << input_nodes.size()
                      << ", input_ctrl_size:" << input_ctrl_size;
  }
  auto ms_context = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(ms_context);
  for (size_t i = 0; i < inputs.size(); ++i) {
    auto tensor = inputs[i];
    MS_EXCEPTION_IF_NULL(tensor);
    auto input_node = input_nodes[i];
    MS_EXCEPTION_IF_NULL(input_node);
    auto size = LongToSize(tensor->data().nbytes());
    if (!input_node->isa<Parameter>()) {
      continue;
    }
    auto input_param = input_node->cast<ParameterPtr>();
    MS_EXCEPTION_IF_NULL(input_param);
    if (!input_param->IsUsedByRealKernelInGraph(kernel_graph->graph_id())) {
      tensor->set_sync_status(kNoNeedSync);
      continue;
    } else if (input_param->has_dynamic_shape()) {
      auto tensor_shape = tensor->shape();
      std::vector<size_t> shape_tmp;
      (void)std::transform(tensor_shape.begin(), tensor_shape.end(), std::back_inserter(shape_tmp), IntToSize);
      AnfAlgo::SetOutputInferTypeAndShape({AnfAlgo::GetOutputInferDataType(input_node, 0)}, {shape_tmp},
                                          input_node.get());
      size = abstract::ShapeSize(shape_tmp) * abstract::TypeIdSize(tensor->data_type());
    }
    if (AnfAlgo::OutputAddrExist(input_node, 0) && TensorNeedSync(input_node, tensor)) {
#if (ENABLE_CPU && !_WIN32)
      const std::string &param_name = input_node->fullname_with_scope();
      if (ps::ps_cache_instance.IsHashTable(param_name)) {
        continue;
      }
#endif
      auto device_address = AnfAlgo::GetMutableOutputAddr(input_node, 0);
      MS_EXCEPTION_IF_NULL(device_address);
      if (size != 0 &&
          !device_address->SyncHostToDevice(trans::GetRuntimePaddingShape(input_node, 0), size, tensor->data_type(),
                                            tensor->data_c(), tensor->device_info().host_format_)) {
        MS_LOG(EXCEPTION) << "SyncHostToDevice failed.";
      }
      if (ms_context->get_param<int>(MS_CTX_EXECUTION_MODE) == kPynativeMode ||
          AnfAlgo::IsParameterWeight(input_param)) {
        tensor->set_device_address(device_address);
      }
      if (kernel_graph->IsUpdatedParameter(input_param)) {
        tensor->SetIsUpdateByDevice();
      }
    }
    tensor->set_sync_status(kNoNeedSync);
  }
}

GraphId AscendSession::CompileGraphImpl(const AnfNodePtrList &lst, const AnfNodePtrList &outputs) {
  MS_LOG(INFO) << "Start";
  // construct graph, if successfully, graph_sum_ + 1
  auto graph = ConstructKernelGraph(lst, outputs);
  auto graph_id = graph->graph_id();
  InitAllBucket(graph);
  MS_LOG(INFO) << "Compile graph " << graph_id << " success";
  return graph_id;
}

GraphId AscendSession::CompileGraphImpl(NotNull<FuncGraphPtr> func_graph) {
  MS_LOG(INFO) << "Start";
  std::vector<KernelGraphPtr> all_graphs;
  auto root_graph = ConstructKernelGraph(func_graph, &all_graphs);
  for (const auto &graph : all_graphs) {
    graph->set_root_graph_id(root_graph->graph_id());
  }
  UnifyMindIR(root_graph);
  // Update Graph Dynamic Shape Attr
  UpdateAllGraphDynamicShapeAttr(all_graphs);
  opt::BackendCommonOptimization(root_graph);
  // empty graph dont entry to backend
  if (root_graph->execution_order().empty()) {
    MS_LOG(INFO) << root_graph->ToString() << " is empty graph.";
    AnfAlgo::InsertMakeTupleForOutput(NOT_NULL(root_graph));
    root_graph->set_executable(false);
    InitRuntimeResource();
    return root_graph->graph_id();
  }

  // Handle control flow by auto-monad.
  HandleControlFlow(NOT_NULL(root_graph));

  // resource initialize
  InitRuntimeResource();

  std::set<KernelGraphPtr> memo;
  IrFusionPass(NOT_NULL(root_graph), NOT_NULL(&memo));
  memo.clear();
  SelectKernel(NOT_NULL(root_graph));
  memo.clear();

  HardwareOptimize(NOT_NULL(root_graph), NOT_NULL(&memo));
  memo.clear();
  // load graphs to debugger.
  if (debugger_ && debugger_->DebuggerBackendEnabled()) {
    LoadGraphsToDbg(NOT_NULL(root_graph), NOT_NULL(&memo));
  }
  memo.clear();
  UpdateRefOutputMap(NOT_NULL(root_graph), NOT_NULL(&memo));
  memo.clear();
  // add make_tuple to the output graph
  AnfAlgo::InsertMakeTupleForOutput(NOT_NULL(root_graph));
  // root root_graph valiate,include genearte execute order and so on
  RootGraphExecutorValidate(NOT_NULL(root_graph));
  // dump graph before remove nop nodes
  auto context_ptr = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(context_ptr);
  bool save_graphs = context_ptr->get_param<bool>(MS_CTX_SAVE_GRAPHS_FLAG);
  if (save_graphs) {
    DumpIRProto(root_graph, "before_removeNop_" + std::to_string(graph_sum_));
  }

  // adjust kernel
  AdjustKernel(root_graph);
#if ENABLE_CPU && ENABLE_D
  InitPsWorker(root_graph);
#endif
  // assign stream
  AssignStream(NOT_NULL(root_graph));
  // insert profiling point
  device::KernelAdjust::GetInstance().Profiling(NOT_NULL(root_graph.get()));
  // build kernel
  BuildKernel(root_graph);
  if (debugger_ && debugger_->partial_memory()) {
    debugger_->PreExecute(root_graph, graph_sum_);
  }
  SetSummaryNodes(root_graph.get());
  // Alloc memory for child graph's inputs
  AssignStaticMemory(NOT_NULL(root_graph), NOT_NULL(&memo));
  memo.clear();
  // Alloc memory for root graph's inputs and node's outputs, workspace
  MemoryAlloc(root_graph.get());
  // generate and load task into device
  Load(root_graph);
  root_graph->SetInputNodes();
  root_graph->SetOptimizerFlag();
  DumpAllGraphs(all_graphs);
  // Save memory profiling data to proto file
  auto profiling_instance = MemoryProfiling::GetInstance();
  if (profiling_instance.IsMemoryProfilingEnable()) {
    auto runtime_instance = device::KernelRuntimeManager::Instance().GetKernelRuntime(kAscendDevice, device_id_);
    MS_EXCEPTION_IF_NULL(runtime_instance);
    uint64_t mem_size = runtime_instance->GetAvailableMemMaxSize();
    profiling_instance.SetDeviceMemSize(mem_size);
    profiling_instance.SaveMemoryProfiling();
  }
  // return the root_graph id to backend
  auto graph_id = root_graph->graph_id();
  return graph_id;
}

void AscendSession::SetFinalGraphSummaryFlag(const std::shared_ptr<KernelGraph> &kernel_graph) {
  MS_EXCEPTION_IF_NULL(kernel_graph);
  auto graph_order = GetGraphOrder(kernel_graph->graph_id());
  for (auto graph_id : graph_order) {
    auto child_graph = GetGraph(graph_id);
    if (child_graph == nullptr) {
      continue;
    }
    if (child_graph->summary_node_exist()) {
      kernel_graph->set_summary_node_exist(true);
      return;
    }
  }
  kernel_graph->set_summary_node_exist(false);
}

void AscendSession::BuildGraphImpl(GraphId graph_id) {
  MS_LOG(INFO) << "Start";
  auto graph = GetGraph(graph_id);
  MS_EXCEPTION_IF_NULL(graph);
  // resource initialize
  InitRuntimeResource();
  // multiple graph handle
  if (graph_id == final_graph_id_) {
    if (!graph->executable()) {
      return;
    }
    SetFinalGraphSummaryFlag(graph);
    // OptChildGraphs
    auto graph_order = GetGraphOrder(final_graph_id_);
    auto &graph_type = GetGraphOrderType(final_graph_id_);
    for (size_t i = 0; i < graph_order.size(); i++) {
      if (!(graph_type[i] == BRANCH_END || graph_type[i] == BRANCH_START)) {
        auto child_graph = GetGraph(graph_order[i]);
        CompileChildGraph(child_graph);
      }
    }
    SetSummaryNodes(graph.get());
    // merge child graph
    MergeGraphExecOrder();
  } else {
    auto single_graph = GetGraph(graph_id);
    MS_EXCEPTION_IF_NULL(single_graph);
    CompileChildGraph(single_graph);
    // set the distinction label of single graph
    single_graph->set_stream_distinction_label(graph_id);
    single_graph->UpdateExecuteKernelStreamLabel();
  }
  // adjust execution order because  merge child graph and other special operations
  AdjustKernel(graph);
#if ENABLE_CPU && ENABLE_D
  InitPsWorker(graph);
#endif
  // Assign streams for control sink and hccl and so on
  AssignStream(NOT_NULL(graph));

  device::KernelAdjust::GetInstance().Profiling(NOT_NULL(graph.get()));
  // build kernel if node is cnode
  BuildKernel(graph);
  auto ms_context = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(ms_context);
  if (debugger_ && debugger_->partial_memory()) {
    debugger_->PreExecute(graph, graph_sum_);
  }
  if (ms_context->get_param<bool>(MS_CTX_PRECOMPILE_ONLY)) {
    MS_LOG(INFO) << "Precompile only, stop in build kernel step";
  } else {
    // alloc memory, including static memory and dynamic memory
    MemoryAlloc(graph.get());
    // generate and load task info to device if it is sink mode
    Load(graph);
  }
  // sync the initial const tensor to device
  SyncInitialTenosrToDevice();
  DumpAllGraphs({graph});
  MS_LOG(INFO) << "End";
}

void AscendSession::CompileChildGraph(const KernelGraphPtr &child_graph) {
  MS_EXCEPTION_IF_NULL(child_graph);
  MS_LOG(INFO) << "CompileChildGraph " << child_graph->ToString();
  opt::AscendBackendIRFusionOptimization(child_graph);
  child_graph->SetExecOrderByDefault();
  auto context_ptr = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(context_ptr);
  bool save_graphs = context_ptr->get_param<bool>(MS_CTX_SAVE_GRAPHS_FLAG);
  if (save_graphs) {
    std::string file_name = "select_kernel_before_graph_" + std::to_string(child_graph->graph_id()) + ".ir";
    DumpIR(file_name, child_graph);
  }
  // select kernel build info
  SelectKernel(*child_graph);
  if (save_graphs) {
    std::string file_name = "select_kernel_after_graph_" + std::to_string(child_graph->graph_id()) + ".ir";
    DumpIR(file_name, child_graph);
  }
  // optimize graph
  HardwareOptimize(child_graph);
  // assign static memory of parameters
  auto runtime_instance = device::KernelRuntimeManager::Instance().GetKernelRuntime(kAscendDevice, device_id_);
  MS_EXCEPTION_IF_NULL(runtime_instance);
  runtime_instance->AssignStaticMemoryInput(child_graph.get());
  runtime_instance->AssignStaticMemoryValueNode(child_graph.get());
}

bool AscendSession::IsSupportSummary() { return !device::KernelAdjust::NeedInsertSwitch(); }

void AscendSession::PreExecuteGraph(const std::shared_ptr<KernelGraph> &kernel_graph,
                                    const std::vector<tensor::TensorPtr> &inputs, VectorRef *const) {
  if (debugger_) {
    debugger_->PreExecute(kernel_graph, graph_sum_);
  }
#if ENABLE_CPU && ENABLE_D
  // Initialize parameter server
  InitPSParamAndOptim(kernel_graph, inputs);
  std::string channel_name;
  if (ps::PsDataPrefetch::GetInstance().cache_enable() && IsGetNextGraph(kernel_graph, &channel_name)) {
    ps::ps_cache_instance.IncreaseGraphStep(channel_name);
  }
#endif
}

void AscendSession::PostExecuteGraph(const std::shared_ptr<KernelGraph> &kernel_graph,
                                     const std::vector<tensor::TensorPtr> &inputs, VectorRef *const) {
  // summary
  Summary(kernel_graph.get());
  // load tensor from device for debugger
  if (debugger_ && debugger_->debugger_enabled()) {
    LoadTensor(kernel_graph);
  }
  // debugger post-execution processing
  if (debugger_) {
    debugger_->PostExecute();
  }
}

void AscendSession::ExecuteGraph(const std::shared_ptr<KernelGraph> &kernel_graph) { Execute(kernel_graph, true); }

void AscendSession::RunOpHardwareOptimize(const std::shared_ptr<session::KernelGraph> &kernel_graph) const {
  MS_LOG(INFO) << "HardwareOptimize Start";
  opt::RunOpAscendBackendOptimization(kernel_graph);
  MS_LOG(INFO) << "HardwareOptimize Finish";
}

bool AscendSession::GraphCacheExist(const GraphInfo &graph_info) const {
  return run_op_graphs_.find(graph_info) != run_op_graphs_.end();
}

void AscendSession::BuildOpImpl(const OpRunInfo &op_run_info, const GraphInfo &graph_info,
                                const std::vector<tensor::TensorPtr> &input_tensors,
                                const std::vector<int64_t> &tensors_mask) {
  if (GraphCacheExist(graph_info)) {
    return;
  }

  const auto &graph = PreBuildOp(op_run_info, input_tensors, tensors_mask);
  MS_EXCEPTION_IF_NULL(graph);
  // init runtime resource
  InitRuntimeResource();
  // build kernel
  RunOpAdjustKernel(graph);
  BuildKernel(graph);
  run_op_graphs_[graph_info] = graph;
}

void AscendSession::RunOpImpl(const GraphInfo &graph_info, OpRunInfo *op_run_info,
                              std::vector<tensor::TensorPtr> *input_tensors, VectorRef *outputs,
                              const std::vector<int64_t> &tensors_mask) {
  MS_EXCEPTION_IF_NULL(input_tensors);
  MS_EXCEPTION_IF_NULL(op_run_info);
  BuildOpImpl(*op_run_info, graph_info, *input_tensors, tensors_mask);
  EraseValueNodeTensor(tensors_mask, input_tensors);

  // wait for allreduce
  for (auto &tensor : *input_tensors) {
    if (tensor->NeedWaitDevice()) {
      tensor->WaitDevice();
    }
  }
  // Run op
  auto graph = run_op_graphs_[graph_info];
  MS_EXCEPTION_IF_NULL(graph);
  // malloc mem
  RunOpRemoveNopNode(graph);
  RunOpMemoryAlloc(*input_tensors, graph.get());
  RunOpGenKernelEvent(graph.get());
  // Build dynamic kernel
  if (op_run_info->is_dynamic_shape) {
    BuildDynamicKernel(graph);
  }
  // load input data to device
  LoadInputData(graph, *input_tensors);
  // run op
  Execute(graph, false);
  // get output
  UpdateOutputs(graph, outputs, *input_tensors);
  // update output abstract of dynamic op to op_run_info
  if (op_run_info->is_dynamic_shape) {
    UpdateOutputAbstract(graph, op_run_info);
  }
  RunOpMemoryClear(graph.get());
}

KernelGraphPtr AscendSession::PreBuildOp(const OpRunInfo &op_run_info,
                                         const std::vector<tensor::TensorPtr> &input_tensors,
                                         const std::vector<int64_t> &tensors_mask) {
  // Construct graph include one op
  auto graph = ConstructSingleOpGraph(op_run_info, input_tensors, tensors_mask, true);
  MS_EXCEPTION_IF_NULL(graph);
  opt::RunOpAscendBackendIRFusionOptimization(graph);
  SelectKernel(*graph);
  RunOpHardwareOptimize(graph);
  return graph;
}

void AscendSession::GetOpInputStubTensors(const CNodePtr &cnode, const std::map<AnfNodePtr, size_t> &parameter_index,
                                          const std::vector<tensor::TensorPtr> &graph_inputs,
                                          const std::map<KernelWithIndex, OutputTensorInfo> &node_output_info,
                                          InputTensorInfo *input_tensor_info) {
  MS_EXCEPTION_IF_NULL(cnode);
  MS_EXCEPTION_IF_NULL(input_tensor_info);
  const auto input_tensor_num = AnfAlgo::GetInputTensorNum(cnode);
  for (size_t i = 1; i <= input_tensor_num; i += 1) {
    const auto &input = cnode->input(i);
    auto kernel_with_index = AnfAlgo::VisitKernel(input, 0);
    auto real_input = kernel_with_index.first;
    MS_EXCEPTION_IF_NULL(real_input);
    tensor::TensorPtr tensor = nullptr;
    if (real_input->isa<ValueNode>()) {
      tensor = GetValueNodeOutputTensor(real_input, kernel_with_index.second);
      input_tensor_info->input_tensors_mask.emplace_back(kParameterDataTensorMask);
    } else if (real_input->isa<Parameter>()) {
      tensor = GetParameterOutputTensor(real_input, parameter_index, graph_inputs);
      auto parameter = real_input->cast<ParameterPtr>();
      MS_EXCEPTION_IF_NULL(parameter);
      input_tensor_info->input_tensors_mask.emplace_back(parameter->has_default() ? kParameterWeightTensorMask
                                                                                  : kParameterDataTensorMask);
    } else if (real_input->isa<CNode>()) {
      bool output_is_weight = false;
      tensor = GetCNodeOutputStubTensor(kernel_with_index, node_output_info, &output_is_weight);
      input_tensor_info->input_tensors_mask.emplace_back(output_is_weight ? kParameterWeightTensorMask
                                                                          : kParameterDataTensorMask);
    } else {
      MS_LOG(EXCEPTION) << "Invalid input node, node = " << real_input->DebugString();
    }
    MS_EXCEPTION_IF_NULL(tensor);
    MS_LOG(DEBUG) << "Get" << i << "th input tensor of " << cnode->fullname_with_scope() << " from "
                  << real_input->fullname_with_scope() << "-" << kernel_with_index.second;
    input_tensor_info->input_tensors.emplace_back(tensor);
  }
}

void AscendSession::BuildOpsInGraph(const GraphId &graph_id, const std::map<AnfNodePtr, size_t> &parameter_index,
                                    const std::vector<tensor::TensorPtr> &graph_inputs,
                                    const std::map<KernelWithIndex, size_t> &cnode_refcount) {
  if (built_graph_id_.find(graph_id) != built_graph_id_.end()) {
    return;
  }
  auto graph = GetGraph(graph_id);
  MS_EXCEPTION_IF_NULL(graph);
  std::map<KernelWithIndex, OutputTensorInfo> op_output_info;
  std::vector<CNodePtr> kernels;
  std::unordered_map<KernelGraphPtr, GraphInfo> single_op_graphs;
  // Collect kernels need to be built in single op graphs
  for (const auto &kernel : graph->execution_order()) {
    // Generate fake input tensors, tensor masks and input kernel with index
    InputTensorInfo input_tensor_info;
    GetOpInputStubTensors(kernel, parameter_index, graph_inputs, op_output_info, &input_tensor_info);
    // Get OpRunInfo and GraphInfo
    OpRunInfo op_run_info;
    GetSingleOpRunInfo(kernel, &op_run_info);
    if (op_run_info.is_dynamic_shape) {
      MS_LOG(INFO) << "BuildOpsInGraph stop, op " << op_run_info.op_name << " is dynamic shape.";
      break;
    }
    const GraphInfo &graph_info = GetSingleOpGraphInfo(kernel, input_tensor_info.input_tensors);
    const auto &single_op_graph_iter = run_op_graphs_.find(graph_info);
    if (single_op_graph_iter != run_op_graphs_.end()) {
      // if graph of same single op exists, the output tensor of current op should be generated
      GenOpOutputStubTensor(single_op_graph_iter->second, kernel, cnode_refcount, &op_output_info);
      continue;
    }
    const auto &single_op_graph =
      PreBuildOp(op_run_info, input_tensor_info.input_tensors, input_tensor_info.input_tensors_mask);
    MS_EXCEPTION_IF_NULL(single_op_graph);
    GenOpOutputStubTensor(single_op_graph, kernel, cnode_refcount, &op_output_info);
    opt::HideNopNode(single_op_graph.get());
    // The graph info could have been changed in PreBuildOp
    const GraphInfo &new_graph_info = GetSingleOpGraphInfo(kernel, input_tensor_info.input_tensors);
    single_op_graphs.emplace(single_op_graph, new_graph_info);
    const auto &execution_order = single_op_graph->execution_order();
    std::copy(execution_order.begin(), execution_order.end(), std::back_inserter(kernels));
  }
  InitRuntimeResource();
  // Compile all kernels parallel
  BuildKernel(kernels);
  // Some new kernel may be added after KernelBuildPreprocess, so collect and build kernels again
  kernels.clear();
  for (const auto &graph_item : single_op_graphs) {
    device::ascend::KernelBuildPreprocess(graph_item.first.get());
    const auto &execution_order = graph_item.first->execution_order();
    std::copy(execution_order.begin(), execution_order.end(), std::back_inserter(kernels));
  }
  BuildKernel(kernels);
  // Record single op graphs in run_op_graphs_ so that these graphs can be reused in BuildOpImpl
  for (const auto &graph_item : single_op_graphs) {
    RunOpMemoryClear(graph_item.first.get());
    run_op_graphs_[graph_item.second] = graph_item.first;
    MS_LOG(DEBUG) << "Pre build op finished, graph info: " << graph_item.second;
  }
  built_graph_id_.insert(graph_id);
}

// compile graph steps
void AscendSession::SelectKernel(const KernelGraph &kernel_graph) const {
  MS_LOG(INFO) << "Start!";
  size_t raise_precision_count = 0;
  size_t reduce_precision_count = 0;
  for (const auto &cnode : kernel_graph.execution_order()) {
    auto status = device::ascend::SelectKernelInfo(cnode);
    AnfAlgo::EraseNodeAttr(kAttrPynativeNextOpName, cnode);
    AnfAlgo::EraseNodeAttr(kAttrPynativeNextIndex, cnode);
    if (status == device::ascend::kStatusRaisePrecision) {
      raise_precision_count++;
    } else if (status == device::ascend::kStatusReducePrecision) {
      reduce_precision_count++;
    }
    MS_LOG(INFO) << "Select ApplyKernel: " << cnode->DebugString();
  }
  auto ms_context = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(ms_context);
  if (ms_context->get_param<int>(MS_CTX_EXECUTION_MODE) == kGraphMode) {
    if (raise_precision_count > 0) {
      MS_LOG(WARNING) << "There has " << raise_precision_count
                      << " node/nodes used raise precision to selected the kernel!";
    }
    if (reduce_precision_count > 0) {
      MS_LOG(WARNING) << "There has " << reduce_precision_count
                      << " node/nodes used reduce precision to selected the kernel!";
    }
  }
  MS_LOG(INFO) << "Finish!";
}

void DumpInit(uint32_t device_id) {
  auto &json_parser = DumpJsonParser::GetInstance();
  json_parser.Parse();
  json_parser.CopyJsonToDir(device_id);
  json_parser.CopyHcclJsonToDir(device_id);
  json_parser.CopyMSCfgJsonToDir(device_id);
  if (json_parser.async_dump_enabled()) {
    if (AdxDataDumpServerInit() != 0) {
      MS_LOG(EXCEPTION) << "Adx data dump server init failed";
    }
  }
}

void AscendSession::InitRuntimeResource() {
  MS_LOG(INFO) << "Start!";
  auto runtime_instance = device::KernelRuntimeManager::Instance().GetKernelRuntime(kAscendDevice, device_id_);
  MS_EXCEPTION_IF_NULL(runtime_instance);
  if (!runtime_instance->Init()) {
    MS_LOG(EXCEPTION) << "Kernel runtime init error.";
  }
  auto env_table_file = common::GetEnv("RANK_TABLE_FILE");
  auto env_rank_id = common::GetEnv("RANK_ID");
  if (!(env_table_file.empty() || env_rank_id.empty())) {
    // get actual rank id if it's distribution training case.
    rank_id_ = GetRankId();
  }
  DumpInit(rank_id_);
  MS_LOG(INFO) << "Finish!";
}

void AscendSession::HardwareOptimize(const std::shared_ptr<KernelGraph> &kernel_graph) const {
  MS_LOG(INFO) << "HardwareOptimize start!";
  opt::AscendBackendOptimization(kernel_graph);
  FinalOptimize(kernel_graph);
  GraphKernelOptimize(kernel_graph);
  MS_EXCEPTION_IF_NULL(kernel_graph);
  kernel_graph->SetExecOrderByDefault();
  MS_LOG(INFO) << "HardwareOptimize Finish!";
}

void AscendSession::GraphKernelOptimize(const std::shared_ptr<KernelGraph> &kernel_graph) const {
  if (!context::GraphKernelFlags::GetInstance().IsEnableGraphKernel()) {
    return;
  }
  opt::GraphKernelOptimize(kernel_graph);
  kernel_graph->SetExecOrderByDefault();
}

void AscendSession::AdjustKernel(const std::shared_ptr<KernelGraph> &kernel_graph) const {
  MS_LOG(INFO) << "Start!";
  opt::HideNopNode(kernel_graph.get());
  // Insert CLearZero op
  // prepare for next step from json get atomic info
  BuildKernel(kernel_graph);
  device::ascend::KernelBuildPreprocess(kernel_graph.get());
  device::KernelAdjust::GetInstance().InsertSwitchLoop(kernel_graph);
  auto context_ptr = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(context_ptr);
  bool save_graphs = context_ptr->get_param<bool>(MS_CTX_SAVE_GRAPHS_FLAG);
  if (save_graphs) {
    DumpIR("after_adjust_kernel.ir", kernel_graph);
  }
  MS_LOG(INFO) << "Finish!";
}

void AscendSession::RunOpAdjustKernel(const std::shared_ptr<KernelGraph> &kernel_graph) const {
  MS_LOG(INFO) << "Start!";
  RunOpHideNopNode(kernel_graph);
  // Insert CLearZero op
  // prepare for next step from json get atomic info
  BuildKernel(kernel_graph);
  device::ascend::KernelBuildPreprocess(kernel_graph.get());
  MS_LOG(INFO) << "Finish!";
}

void AscendSession::AssignStream(NotNull<KernelGraphPtr> kernel_graph) const {
  MS_LOG(INFO) << "Start!";
  device::ascend::AscendStreamAssign::GetInstance().AssignStream(kernel_graph);
  MS_LOG(INFO) << "Finish!";
}

void AscendSession::BuildKernel(const std::shared_ptr<KernelGraph> &kernel_graph) const {
  BuildKernel(kernel_graph->execution_order());
}

void AscendSession::BuildKernel(const std::vector<CNodePtr> &kernels) const {
  MS_LOG(INFO) << "Start!";
  struct timeval start_time, end_time;
  (void)gettimeofday(&start_time, nullptr);
  auto ret = device::ascend::KernelBuild(kernels);
  if (!ret) {
    MS_LOG(EXCEPTION) << "Kernel build error.";
  }
  (void)gettimeofday(&end_time, nullptr);
  const uint64_t kUSecondInSecond = 1000000;
  uint64_t cost = kUSecondInSecond * static_cast<uint64_t>(end_time.tv_sec - start_time.tv_sec);
  cost += static_cast<uint64_t>(end_time.tv_usec - start_time.tv_usec);
  MS_LOG(INFO) << "KernelBuild run in  " << PRIu64 << " us " << cost;
  MS_LOG(INFO) << "Finish!";
}

void AscendSession::BuildDynamicKernel(const std::shared_ptr<KernelGraph> &kernel_graph) const {
  MS_LOG(DEBUG) << "Start!";
  MS_EXCEPTION_IF_NULL(kernel_graph);
  const auto &kernels = kernel_graph->execution_order();
  auto iter = std::find_if(kernels.begin(), kernels.end(), [](const CNodePtr &kernel) {
    return AnfAlgo::GetBooleanAttr(kernel, kAttrOutputIsDynamicShape);
  });
  if (iter == kernels.end()) {
    return;
  }
  auto runtime_instance = device::KernelRuntimeManager::Instance().GetKernelRuntime(kAscendDevice, device_id_);
  MS_EXCEPTION_IF_NULL(runtime_instance);
  if (!runtime_instance->GenDynamicKernel(kernel_graph.get())) {
    MS_LOG(DEBUG) << "Graph:" << kernel_graph->graph_id() << " failed to generate dynamic kernel!";
  }
  MS_LOG(DEBUG) << "Finish!";
}

static CNodePtr GetNextLabelSet(const std::vector<CNodePtr> &kernel_nodes, uint32_t index) {
  uint32_t node_sizes = kernel_nodes.size();
  if (index >= node_sizes - 1) {
    MS_LOG(EXCEPTION) << "there is no node after this node:" << kernel_nodes[index]->DebugString();
  }
  auto kernel = kernel_nodes[index + 1];
  if (AnfAlgo::GetCNodeName(kernel) != kLabelSetOpName) {
    MS_LOG(EXCEPTION) << "the node is not labelset follow labelgoto/labelswitch, node: "
                      << kernel_nodes[index]->DebugString();
  }
  return kernel;
}

static std::vector<CNodePtr> HandleRecursiveCall(const std::vector<CNodePtr> &kernel_cnodes, const uint32_t &back_label,
                                                 uint32_t *index, std::vector<CNodePtr> *back) {
  MS_EXCEPTION_IF_NULL(index);
  MS_EXCEPTION_IF_NULL(back);
  std::vector<CNodePtr> front;
  std::vector<CNodePtr> back_temp;
  bool back_flag = false;
  for (uint32_t i = *index; i < kernel_cnodes.size(); i++) {
    if (!back_flag) {
      front.emplace_back(kernel_cnodes[i]);
    } else {
      back->emplace_back(kernel_cnodes[i]);
    }
    if (AnfAlgo::HasNodeAttr(kAttrRecursiveEnd, kernel_cnodes[i])) {
      *index = i;
      back->insert(back->end(), back_temp.begin(), back_temp.end());
      return front;
    }
    if (AnfAlgo::HasNodeAttr(kAttrRecursive, kernel_cnodes[i])) {
      back_flag = true;
      if (AnfAlgo::IsLabelIndexInNode(kernel_cnodes[i], back_label)) {
        continue;
      } else {
        auto temp = HandleRecursiveCall(kernel_cnodes, back_label, &(++i), &back_temp);
        front.insert(front.end(), temp.begin(), temp.end());
        continue;
      }
    }
  }
  return front;
}

static void UnfoldRecursiveExecOrder(KernelGraph *kernel_graph) {
  MS_EXCEPTION_IF_NULL(kernel_graph);
  if (!kernel_graph->recursive_call()) {
    return;
  }
  auto kernel_cnodes = kernel_graph->mem_reuse_exec_order();
  std::vector<CNodePtr> mem_reuse_order;
  mem_reuse_order.reserve(kernel_cnodes.size());
  for (uint32_t i = 0; i < kernel_cnodes.size(); i++) {
    if (!AnfAlgo::HasNodeAttr(kAttrRecursiveStart, kernel_cnodes[i])) {
      mem_reuse_order.emplace_back(kernel_cnodes[i]);
      continue;
    }
    auto label_id = AnfAlgo::GetNodeAttr<uint32_t>(kernel_cnodes[i], kAttrLabelIndex);
    std::vector<CNodePtr> back;
    auto front = HandleRecursiveCall(kernel_cnodes, label_id, &i, &back);
    mem_reuse_order.insert(mem_reuse_order.end(), front.begin(), front.end());
    mem_reuse_order.insert(mem_reuse_order.end(), back.begin(), back.end());
  }
  kernel_graph->set_mem_reuse_exec_order(mem_reuse_order);
}

static void GetSubGraphExecOrder(const KernelGraph *kernel_graph, uint32_t index, const CNodePtr &back_node,
                                 std::vector<CNodePtr> *mem_reuse_order) {
  MS_EXCEPTION_IF_NULL(kernel_graph);
  MS_EXCEPTION_IF_NULL(mem_reuse_order);
  auto label_id = AnfAlgo::GetNodeAttr<uint32_t>(back_node, kAttrLabelIndex);
  auto kernel_cnodes = kernel_graph->execution_order();
  for (auto i = index; i < kernel_cnodes.size(); i++) {
    mem_reuse_order->emplace_back(kernel_cnodes[i]);
    if (AnfAlgo::IsLabelIndexInNode(kernel_cnodes[i], label_id)) {
      return;
    }
  }
}

void InitMemReuseExecOrder(KernelGraph *kernel_graph) {
  MS_EXCEPTION_IF_NULL(kernel_graph);
  if (!kernel_graph->subgraph_multi_call()) {
    return;
  }
  std::unordered_map<uint32_t, uint32_t> label_id_index_map;
  auto kernel_cnodes = kernel_graph->execution_order();
  std::vector<CNodePtr> mem_reuse_order;
  for (size_t i = 0; i < kernel_cnodes.size(); i++) {
    mem_reuse_order.emplace_back(kernel_cnodes[i]);
    if (AnfAlgo::CheckPrimitiveType(kernel_cnodes[i], prim::kPrimLabelSwitch) &&
        !AnfAlgo::HasNodeAttr(kAttrRecursive, kernel_cnodes[i]) &&
        !AnfAlgo::HasNodeAttr(kAttrReturn, kernel_cnodes[i])) {
      auto label_list = AnfAlgo::GetNodeAttr<std::vector<uint32_t>>(kernel_cnodes[i], kAttrLabelSwitchList);
      for (auto label_id : label_list) {
        if (label_id_index_map.find(label_id) == label_id_index_map.end()) {
          continue;
        }
        auto back_node = GetNextLabelSet(kernel_cnodes, i);
        GetSubGraphExecOrder(kernel_graph, label_id_index_map[label_id], back_node, &mem_reuse_order);
      }
      continue;
    }
    if (AnfAlgo::CheckPrimitiveType(kernel_cnodes[i], prim::kPrimLabelGoto) &&
        !AnfAlgo::HasNodeAttr(kAttrRecursive, kernel_cnodes[i]) &&
        !AnfAlgo::HasNodeAttr(kAttrReturn, kernel_cnodes[i])) {
      auto label_id = AnfAlgo::GetNodeAttr<uint32_t>(kernel_cnodes[i], kAttrLabelIndex);
      if (label_id_index_map.find(label_id) == label_id_index_map.end()) {
        continue;
      }
      auto back_node = GetNextLabelSet(kernel_cnodes, i);
      GetSubGraphExecOrder(kernel_graph, label_id_index_map[label_id], back_node, &mem_reuse_order);
      continue;
    }
    if (AnfAlgo::CheckPrimitiveType(kernel_cnodes[i], prim::kPrimLabelSet) &&
        !AnfAlgo::HasNodeAttr(kAttrRecursive, kernel_cnodes[i])) {
      auto label_id = AnfAlgo::GetNodeAttr<uint32_t>(kernel_cnodes[i], kAttrLabelIndex);
      if (label_id_index_map.find(label_id) != label_id_index_map.end()) {
        MS_LOG(EXCEPTION) << "Two labelsets with same label id.";
      }
      label_id_index_map[label_id] = i;
      continue;
    }
  }
  kernel_graph->set_mem_reuse_exec_order(mem_reuse_order);
  UnfoldRecursiveExecOrder(kernel_graph);
}

void AscendSession::MemoryAlloc(KernelGraph *kernel_graph) const {
  MS_LOG(INFO) << "Start!";
  MS_EXCEPTION_IF_NULL(kernel_graph);
  InitMemReuseExecOrder(kernel_graph);
  auto runtime_instance = device::KernelRuntimeManager::Instance().GetKernelRuntime(kAscendDevice, device_id_);
  MS_EXCEPTION_IF_NULL(runtime_instance);
  runtime_instance->AssignMemory(kernel_graph);
  MS_LOG(INFO) << "Finish!";
}

void AscendSession::RunOpMemoryAlloc(const std::vector<tensor::TensorPtr> &input_tensors,
                                     KernelGraph *kernel_graph) const {
  MS_EXCEPTION_IF_NULL(kernel_graph);
  auto runtime_instance = device::KernelRuntimeManager::Instance().GetKernelRuntime(kAscendDevice, device_id_);
  MS_EXCEPTION_IF_NULL(runtime_instance);
  runtime_instance->RunOpAssignMemory(input_tensors, kernel_graph);
}

void AscendSession::RunOpGenKernelEvent(const KernelGraph *graph) const {
  MS_EXCEPTION_IF_NULL(graph);
  auto runtime_instance = device::KernelRuntimeManager::Instance().GetKernelRuntime(kAscendDevice, device_id_);
  MS_EXCEPTION_IF_NULL(runtime_instance);
  runtime_instance->GenKernelEvents(graph);
}

void AscendSession::RunOpMemoryClear(const KernelGraph *kernel_graph) const {
  MS_EXCEPTION_IF_NULL(kernel_graph);
  auto runtime_instance = device::KernelRuntimeManager::Instance().GetKernelRuntime(kAscendDevice, device_id_);
  MS_EXCEPTION_IF_NULL(runtime_instance);
  runtime_instance->RunOpClearMemory(kernel_graph);
}

void AscendSession::Load(const std::shared_ptr<KernelGraph> &kernel_graph) const {
  MS_LOG(INFO) << "Start!";
  auto context_ptr = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(context_ptr);
  bool is_task_sink = context_ptr->get_param<bool>(MS_CTX_ENABLE_TASK_SINK);
  (void)device::KernelAdjust::GetInstance().StepLoadCtrlInputs(kernel_graph);
  auto runtime_instance = device::KernelRuntimeManager::Instance().GetKernelRuntime(kAscendDevice, device_id_);
  MS_EXCEPTION_IF_NULL(runtime_instance);
  bool ret_ok = runtime_instance->Load(kernel_graph.get(), is_task_sink);
  if (!ret_ok) {
    MS_LOG(EXCEPTION) << "Load task error!";
  }
  MS_LOG(INFO) << "Finish!";
}

void AscendSession::Execute(const std::shared_ptr<KernelGraph> &kernel_graph, bool is_task) const {
  MS_LOG(DEBUG) << "Start!";
  bool is_task_sink = false;
  if (is_task) {
    auto context_ptr = MsContext::GetInstance();
    MS_EXCEPTION_IF_NULL(context_ptr);
    is_task_sink = context_ptr->get_param<bool>(MS_CTX_ENABLE_TASK_SINK);
  }
  auto runtime_instance = device::KernelRuntimeManager::Instance().GetKernelRuntime(kAscendDevice, device_id_);
  MS_EXCEPTION_IF_NULL(runtime_instance);
  DumpSetup(kernel_graph);
  bool ret_ok = runtime_instance->Run(kernel_graph.get(), is_task_sink);
  Dump(kernel_graph);
  if (!ret_ok) {
#ifdef ENABLE_DUMP_IR
    mindspore::RDR::TriggerAll();
#endif
    MS_LOG(EXCEPTION) << "run task error!";
  }
  MS_LOG(DEBUG) << "Finish!";
}

void AscendSession::DumpSetup(const std::shared_ptr<KernelGraph> &kernel_graph) const {
  MS_LOG(DEBUG) << "Start!";
  MS_EXCEPTION_IF_NULL(kernel_graph);
  E2eDump::DumpSetup(kernel_graph.get(), rank_id_);
  MS_LOG(DEBUG) << "Finish!";
}

void AscendSession::Dump(const std::shared_ptr<KernelGraph> &kernel_graph) const {
  MS_LOG(DEBUG) << "Start!";
  MS_EXCEPTION_IF_NULL(kernel_graph);
  E2eDump::DumpData(kernel_graph.get(), rank_id_);
  MS_LOG(DEBUG) << "Finish!";
}

void AscendSession::DumpAllGraphs(const std::vector<KernelGraphPtr> &all_graphs) {
#ifdef ENABLE_DUMP_IR
  auto context_ptr = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(context_ptr);
  bool save_graphs = context_ptr->get_param<bool>(MS_CTX_SAVE_GRAPHS_FLAG);
  auto &json_parser = DumpJsonParser::GetInstance();
  json_parser.Parse();
  if (!save_graphs && !json_parser.e2e_dump_enabled() && !json_parser.async_dump_enabled() &&
      !mindspore::RecorderManager::Instance().RdrEnable()) {
    return;
  }
  auto kernel_runtime = device::KernelRuntimeManager::Instance().GetKernelRuntime(kAscendDevice, device_id_);
  MS_EXCEPTION_IF_NULL(kernel_runtime);
  for (auto &graph : all_graphs) {
    MS_EXCEPTION_IF_NULL(graph);
    std::string name = "graph_build." + std::to_string(graph->graph_id());
    DumpGraphParams dump_params = {true, static_cast<int>(kWholeStack)};
    mindspore::RDR::RecordAnfGraph(SUBMODULE_ID, name, graph, dump_params, ".ir;.pb");
    if (save_graphs) {
      std::string file_name = "graph_build_" + std::to_string(graph->graph_id()) + ".ir";
      DumpIR(file_name, graph, true, kWholeStack);
      DumpIRProto(graph, "vm_build_" + std::to_string(graph->graph_id()));
      DumpIR("trace_code_graph", graph, true, kWholeStack);
    }
    std::string final_graph = "trace_code_graph_" + std::to_string(graph->graph_id());
    if (json_parser.e2e_dump_enabled() || json_parser.async_dump_enabled()) {
      std::string root_dir = json_parser.path() + "/rank_" + std::to_string(rank_id_);
      std::string target_dir = root_dir + "/graphs";
      std::string ir_file_path = target_dir + "/" + "ms_output_" + final_graph + ".ir";
      DumpIRProtoWithSrcInfo(graph, final_graph, target_dir, kDebugWholeStack);
      DumpIR("trace_code_graph", graph, true, kWholeStack, ir_file_path);
      DumpGraphExeOrder("ms_execution_order_graph_" + std::to_string(graph->graph_id()) + ".csv", root_dir,
                        graph->execution_order());
    }
  }
#endif
}

void AscendSession::LoadTensor(const std::shared_ptr<KernelGraph> &kernel_graph) const {
  MS_LOG(INFO) << "Start!";
  MS_EXCEPTION_IF_NULL(kernel_graph);
  auto runtime_instance = device::KernelRuntimeManager::Instance().GetKernelRuntime(kAscendDevice, device_id_);
  MS_EXCEPTION_IF_NULL(runtime_instance);
  (void)runtime_instance->LoadData(kernel_graph.get());
  MS_LOG(INFO) << "Finish!";
}

void AscendSession::RecurseSetSummaryNodes(KernelGraph *graph,
                                           std::map<std::string, std::pair<AnfNodePtr, int>> *summary) {
  MS_EXCEPTION_IF_NULL(graph);
  MS_EXCEPTION_IF_NULL(summary);
  // if final graph have no child graph
  auto graph_order_iter = graph_execute_orders_.find(graph->graph_id());
  if (graph_order_iter == graph_execute_orders_.end()) {
    SessionBasic::SetSummaryNodes(graph);
    auto summary_nodes = graph->summary_nodes();
    summary->insert(summary_nodes.begin(), summary_nodes.end());
    return;
  }
  // for every child graph, find summary nodes
  auto graph_order = GetGraphOrder(graph->graph_id());
  for (size_t i = 0; i < graph_order.size(); i++) {
    auto child_graph = GetGraph(graph_order[i]);
    if (child_graph == nullptr) {
      continue;
    }
    SessionBasic::SetSummaryNodes(child_graph.get());
    auto child_graph_summary = child_graph->summary_nodes();
    summary->insert(child_graph_summary.begin(), child_graph_summary.end());
    RecurseSetSummaryNodes(child_graph.get(), summary);
  }
  graph->set_summary_nodes(*summary);
}

void AscendSession::SetSummaryNodes(KernelGraph *graph) {
  MS_LOG(DEBUG) << "Update summary Start";
  MS_EXCEPTION_IF_NULL(graph);
  auto summary_nodes = graph->summary_nodes();
  std::map<std::string, std::pair<AnfNodePtr, int>> summary;
  summary.insert(summary_nodes.begin(), summary_nodes.end());
  RecurseSetSummaryNodes(graph, &summary);
  graph->set_summary_nodes(summary);
  MS_LOG(DEBUG) << "Update summary end size: " << summary.size();
}

void AscendSession::MergeGraphExecOrder() {
  MS_LOG(INFO) << "Start!";
  // merge graph order
  auto &graph_order = GetGraphOrder(final_graph_id_);
  auto &graph_type = GetGraphOrderType(final_graph_id_);
  auto final_graph = GetGraph(final_graph_id_);
  MS_EXCEPTION_IF_NULL(final_graph);
  if (graph_order.empty()) {
    MS_LOG(WARNING) << "Graph output is a lonely variable not linked to any op!";
    return;
  }
  if (graph_order.size() > 1) {
    auto context_ptr = MsContext::GetInstance();
    MS_EXCEPTION_IF_NULL(context_ptr);
    if (!context_ptr->get_param<bool>(MS_CTX_ENABLE_TASK_SINK)) {
      MS_LOG(EXCEPTION) << "Control sink network should run with task-sink mode!";
    }
  }
  // if first graph is common,the final graph has no label,then set the stream of final graph same with the first graph
  SetStreamDistinctionLabel(final_graph, graph_order[0], false);
  std::vector<CNodePtr> final_exec_order = final_graph->execution_order();
  KernelGraphPtr last_graph = nullptr;
  for (size_t i = 0; i < graph_order.size(); i++) {
    auto graph_id = graph_order[i];
    if (graph_type[i] == BRANCH_END || graph_type[i] == BRANCH_START) {
      continue;
    }
    auto child_graph = GetGraph(graph_id);
    last_graph = child_graph;
    MS_EXCEPTION_IF_NULL(child_graph);
    auto exec_order = child_graph->execution_order();
    MS_LOG(INFO) << "Merge graph,graph_id " << graph_id;
    (void)std::transform(exec_order.begin(), exec_order.end(), std::back_inserter(final_exec_order),
                         [&](CNodePtr node) -> CNodePtr {
                           AnfAlgo::SetStreamDistinctionLabel(child_graph->stream_distinction_label(), node.get());
                           return node;
                         });
    // add all value nodes of child graphs to final graph
    for (auto &value_node : child_graph->graph_value_nodes()) {
      final_graph->AddValueNodeToGraph(value_node);
    }
    // copy ref map to final graph
    auto child_ref_map = child_graph->GetRefMap();
    for (auto &item : child_ref_map) {
      if (final_graph->IsInRefOutputMap(item.first)) {
        MS_LOG(EXCEPTION) << "The ref pair is already in final graph!";
      }
      final_graph->AddRefCorrespondPairs(item.first, item.second);
    }
  }
  // set final_exec_order into final graph
  MS_EXCEPTION_IF_NULL(final_graph);
  DumpGraphExeOrder(final_exec_order);
  final_graph->set_execution_order(final_exec_order);
}

const std::vector<GraphId> &AscendSession::GetGraphOrder(GraphId final_graph_id) const {
  auto graph_order_iter = graph_execute_orders_.find(final_graph_id);
  if (graph_order_iter == graph_execute_orders_.end()) {
    MS_LOG(EXCEPTION) << "Final graph" << final_graph_id << "has no child graph";
  }
  return graph_order_iter->second;
}

const std::vector<GraphType> &AscendSession::GetGraphOrderType(GraphId final_graph_id) const {
  auto graph_type_iter = graph_order_types_.find(final_graph_id);
  if (graph_type_iter == graph_order_types_.end()) {
    MS_LOG(EXCEPTION) << "Final graph" << final_graph_id << "has no graph_order_types_";
  }
  return graph_type_iter->second;
}

void AscendSession::SyncInitialTenosrToDevice() {
  for (auto &item : initial_tenosrs_) {
    auto to_graph_id = item.first.first;
    auto input_idx = item.first.second;
    auto front_tensor = item.second;
    auto to_graph = GetGraph(to_graph_id);
    MS_EXCEPTION_IF_NULL(to_graph);
    std::vector<AnfNodePtr> graph_inputs = to_graph->inputs();
    if (input_idx >= graph_inputs.size()) {
      MS_LOG(EXCEPTION) << "Input_index " << input_idx << " out of range size " << graph_inputs.size();
    }
    auto backend_parameter = graph_inputs[input_idx];
    // sync data from host to device
    MS_EXCEPTION_IF_NULL(front_tensor);
    size_t tensor_size = front_tensor->data().nbytes();
    auto addr = AnfAlgo::GetOutputAddr(backend_parameter, 0);
    MS_EXCEPTION_IF_NULL(addr);
    if (!addr->SyncHostToDevice(trans::GetRuntimePaddingShape(backend_parameter, 0), tensor_size,
                                front_tensor->data_type(), front_tensor->data_c(),
                                front_tensor->device_info().host_format_)) {
      MS_LOG(EXCEPTION) << "Tensor SyncHostToDevice fail!";
    }
  }
}

void AscendSession::RootGraphExecutorValidate(NotNull<KernelGraphPtr> graph) {
  AscendAutoMonad auto_monad(graph);
  auto_monad.GenerateExecuteOrder();
}

void AscendSession::IrFusionPass(const NotNull<KernelGraphPtr> graph, NotNull<std::set<KernelGraphPtr> *> memo) {
  if (memo->find(graph) != memo->end()) {
    return;
  }
  memo->insert(graph.get());
  opt::AscendBackendIRFusionOptimization(graph);
  graph->SetExecOrderByDefault();

  auto context_ptr = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(context_ptr);
  bool save_graphs = context_ptr->get_param<bool>(MS_CTX_SAVE_GRAPHS_FLAG);
  if (save_graphs) {
    std::string file_name = "select_kernel_before_graph_" + std::to_string(graph->graph_id()) + ".ir";
    DumpIR(file_name, graph.get());
  }

  for (auto &child_graph : graph->child_graph_order()) {
    IrFusionPass(NOT_NULL(child_graph.lock()), memo);
  }
}

void AscendSession::SelectKernel(NotNull<KernelGraphPtr> root_graph) {
  MS_LOG(INFO) << "Start select kernel.";
  size_t raise_precision_count = 0;
  size_t reduce_precision_count = 0;

  std::set<KernelGraphPtr> memo;
  (void)RecurseSelectKernelInfo(root_graph, NOT_NULL(&memo), &raise_precision_count, &reduce_precision_count);
  memo.clear();

  auto ms_context = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(ms_context);
  if (ms_context->get_param<int>(MS_CTX_EXECUTION_MODE) == kGraphMode) {
    if (raise_precision_count > 0) {
      MS_LOG(WARNING) << "There are " << raise_precision_count
                      << " node/nodes used raise precision to selected the kernel!";
    }
    if (reduce_precision_count > 0) {
      MS_LOG(WARNING) << "There are " << reduce_precision_count
                      << " node/nodes used reduce precision to selected the kernel!";
    }
  }
  MS_LOG(INFO) << "Finish!";
}

void AscendSession::RecurseSelectKernelInfo(NotNull<KernelGraphPtr> graph,
                                            NotNull<std::set<KernelGraphPtr> *> const memo,
                                            size_t *const raise_precision_count,
                                            size_t *const reduce_precision_count) const {
  if (memo->find(graph) != memo->end()) {
    return;
  }
  memo->insert(graph.get());
  MS_LOG(INFO) << "Start to select kernel info in graph: " << graph->graph_id();

  for (const auto &cnode : graph->execution_order()) {
    if (AnfAlgo::IsCondControlKernel(cnode)) {
      std::vector<KernelGraphPtr> child_graphs;
      if (AnfAlgo::HasNodeAttr(kAttrChildGraph, cnode)) {
        child_graphs = AnfAlgo::GetNodeAttr<std::vector<KernelGraphPtr>>(cnode, kAttrChildGraph);
      }
      for (auto &child_graph : child_graphs) {
        RecurseSelectKernelInfo(NOT_NULL(child_graph), memo, raise_precision_count, reduce_precision_count);
      }
    }

    auto status = device::ascend::SelectKernelInfo(cnode);
    if (status == device::ascend::kStatusRaisePrecision) {
      (*raise_precision_count)++;
    } else if (status == device::ascend::kStatusReducePrecision) {
      (*reduce_precision_count)++;
    }
  }

  auto context_ptr = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(context_ptr);
  bool save_graphs = context_ptr->get_param<bool>(MS_CTX_SAVE_GRAPHS_FLAG);
  if (save_graphs) {
    std::string file_name = "select_kernel_after_graph_" + std::to_string(graph->graph_id()) + ".ir";
    DumpIR(file_name, graph.get());
  }
  MS_LOG(INFO) << "Finish selecting kernel info in graph: " << graph->graph_id();
}

void AscendSession::HardwareOptimize(NotNull<KernelGraphPtr> graph,
                                     NotNull<std::set<KernelGraphPtr> *> const memo) const {
  if (memo->find(graph) != memo->end()) {
    return;
  }
  memo->insert(graph.get());

  MS_LOG(INFO) << "Start to do HardwareOptimize in graph: " << graph->graph_id();

  HardwareOptimize(graph.get());
  for (auto &child_graph : graph->child_graph_order()) {
    HardwareOptimize(NOT_NULL(child_graph.lock()), memo);
  }
  MS_LOG(INFO) << "Finish doing HardwareOptimize in graph: " << graph->graph_id();
}

void AscendSession::LoadGraphsToDbg(NotNull<KernelGraphPtr> graph,
                                    NotNull<std::set<KernelGraphPtr> *> const memo) const {
  if (memo->find(graph) != memo->end()) {
    return;
  }
  memo->insert(graph.get());

  MS_LOG(INFO) << "Start to do LoadGraphsToDbg in graph: " << graph->graph_id();

  debugger_->LoadGraphs(graph);
  MS_LOG(INFO) << "graph_sum_: " << graph_sum_;
  for (auto &child_graph : graph->child_graph_order()) {
    LoadGraphsToDbg(NOT_NULL(child_graph.lock()), memo);
  }
  MS_LOG(INFO) << "Finish doing LoadGraphsToDbg in graph: " << graph->graph_id();
}

void AscendSession::AssignStaticMemory(NotNull<KernelGraphPtr> graph,
                                       NotNull<std::set<KernelGraphPtr> *> const memo) const {
  if (memo->find(graph) != memo->end()) {
    return;
  }
  memo->insert(graph.get());

  MS_LOG(INFO) << "Start to assign static memory for parameter in graph: " << graph->graph_id();
  // assign static memory for parameters
  auto runtime_instance = device::KernelRuntimeManager::Instance().GetKernelRuntime(kAscendDevice, device_id_);
  MS_EXCEPTION_IF_NULL(runtime_instance);
  runtime_instance->ClearGlobalIdleMem();
  runtime_instance->AssignStaticMemoryInput(graph.get().get());
  runtime_instance->AssignStaticMemoryValueNode(graph.get().get());
  for (auto &child_graph : graph->child_graph_order()) {
    AssignStaticMemory(NOT_NULL(child_graph.lock()), memo);
  }
  MS_LOG(INFO) << "Finish assigning static memory for parameter in graph: " << graph->graph_id();
}

void AscendSession::UpdateRefOutputMap(NotNull<KernelGraphPtr> graph,
                                       NotNull<std::set<KernelGraphPtr> *> const memo) const {
  if (memo->find(graph) != memo->end()) {
    return;
  }
  memo->insert(graph.get());

  for (auto &child_graph : graph->child_graph_order()) {
    std::shared_ptr<KernelGraph> child_graph_ptr = child_graph.lock();
    MS_EXCEPTION_IF_NULL(child_graph_ptr);
    UpdateRefOutputMap(NOT_NULL(child_graph_ptr), memo);
    // copy ref map to final graph
    auto child_ref_map = child_graph_ptr->GetRefMap();
    for (auto &item : child_ref_map) {
      if (graph->IsInRefOutputMap(item.first)) {
        MS_LOG(WARNING) << "The ref pair <" << item.first.first->DebugString() << ", " << item.first.second
                        << "> is already in " << graph->ToString();
        continue;
      }
      graph->AddRefCorrespondPairs(item.first, item.second);
    }
  }
}

void AscendSession::SyncStream() {
  auto runtime_instance = device::KernelRuntimeManager::Instance().GetKernelRuntime(kAscendDevice, device_id_);
  MS_EXCEPTION_IF_NULL(runtime_instance);
  auto ret = runtime_instance->SyncStream();
  if (!ret) {
    MS_LOG(EXCEPTION) << "Sync stream error!";
  }
}

std::shared_ptr<device::Bucket> AscendSession::CreateBucket(uint32_t bucket_id, uint32_t bucket_size) {
  auto bucket = std::make_shared<device::ascend::AscendBucket>(bucket_id, bucket_size);

  auto kernel_runtime = device::KernelRuntimeManager::Instance().GetCurrentKernelRuntime();
  MS_EXCEPTION_IF_NULL(kernel_runtime);
  auto compute_stream = kernel_runtime->compute_stream();
  auto communication_stream = kernel_runtime->communication_stream();
  MS_EXCEPTION_IF_NULL(compute_stream);
  MS_EXCEPTION_IF_NULL(communication_stream);

  MS_EXCEPTION_IF_NULL(bucket);
  bucket->Init({compute_stream}, {communication_stream});
  return bucket;
}
}  // namespace session
}  // namespace mindspore
