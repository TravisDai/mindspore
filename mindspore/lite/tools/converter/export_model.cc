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

#include "tools/converter/export_model.h"
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "include/errorcode.h"
#include "include/version.h"
#include "ir/func_graph.h"
#include "tools/anf_exporter/anf_exporter.h"
#include "tools/converter/graphdef_transform.h"
#include "tools/converter/dump_graph_init.h"
#include "tools/optimizer/graph/unify_format_pass.h"
#include "tools/optimizer/graph/control_flow_pass.h"

namespace mindspore {
namespace lite {
namespace {
using NodesMap = std::map<std::string, std::vector<AnfNodePtr>>;
}
static converter::Flags *flags = nullptr;

void CloneGraphInputs(const FuncGraphPtr &origin, const FuncGraphPtr &mirror, NodesMap *origin_map,
                      NodesMap *mirror_map) {
  MS_ASSERT(origin != nullptr && mirror != nullptr);
  MS_ASSERT(origin_map != nullptr && mirror_map != nullptr);
  auto origin_inputs = origin->get_inputs();
  for (auto &input : origin_inputs) {
    auto mirror_input = mirror->add_parameter();
    if (input->abstract() != nullptr) {
      mirror_input->set_abstract(input->abstract()->Clone());
    }
    mirror_input->set_name(input->fullname_with_scope());
    (*origin_map)[input->fullname_with_scope()].push_back(input);
    (*mirror_map)[input->fullname_with_scope()].push_back(mirror_input);
  }
}

AnfNodePtr CloneParameterAndValueNode(const CNodePtr &cnode, size_t index, const FuncGraphPtr &mirror_graph) {
  MS_ASSERT(cnode != nullptr && mirror_graph != nullptr);
  if (index >= cnode->size()) {
    MS_LOG(ERROR) << "input index out of range.";
    return nullptr;
  }
  auto node = cnode->input(index);
  if (utils::isa<mindspore::CNode>(node)) {
    MS_LOG(ERROR) << "this func cannot copy cnode.";
    return nullptr;
  }
  if (utils::isa<ValueNode>(node)) {
    auto value_node = node->cast<ValueNodePtr>();
    auto value_ptr = value_node->value();
    MS_ASSERT(value_ptr != nullptr);
    if (utils::isa<Monad>(value_ptr)) {
      std::shared_ptr<Monad> mirror_monad;
      if (utils::isa<UMonad>(value_ptr)) {
        mirror_monad = std::make_shared<UMonad>();
      } else {
        mirror_monad = std::make_shared<IOMonad>();
      }
      auto monad_abs = mirror_monad->ToAbstract();
      auto mirror_value_node = NewValueNode(mirror_monad);
      mirror_value_node->set_abstract(monad_abs);
      return mirror_value_node;
    }
  }
  DataInfo data_info;
  STATUS status;
  if (utils::isa<Parameter>(node)) {
    status = FetchDataFromParameterNode(cnode, index, flags->fmk, flags->trainModel, &data_info);
  } else if (utils::isa<ValueNode>(node)) {
    status = FetchDataFromValueNode(cnode, index, flags->fmk, flags->trainModel, &data_info);
  } else {
    status = RET_ERROR;
  }
  if (status != RET_OK && status != RET_NO_CHANGE) {
    MS_LOG(ERROR) << "fetch data failed.";
    return nullptr;
  }
  if (opt::CheckPrimitiveType(cnode, prim::kPrimTupleGetItem) && !data_info.data_.empty()) {
    return NewValueNode(MakeValue<int>(*reinterpret_cast<int *>(data_info.data_.data())));
  }
  ShapeVector shape_vec(data_info.shape_.begin(), data_info.shape_.end());
  auto tensor_info = std::make_shared<tensor::Tensor>(static_cast<TypeId>(data_info.data_type_), shape_vec);
  if (!data_info.data_.empty()) {
    auto tensor_data = reinterpret_cast<uint8_t *>(tensor_info->data_c());
    if (memcpy_s(tensor_data, tensor_info->data().nbytes(), data_info.data_.data(), data_info.data_.size()) != EOK) {
      MS_LOG(ERROR) << "memcpy_s failed";
      return nullptr;
    }
  }
  auto mirror_parameter = mirror_graph->add_parameter();
  if (node->abstract() != nullptr) {
    mirror_parameter->set_abstract(node->abstract()->Clone());
  }
  mirror_parameter->set_name(node->fullname_with_scope());
  mirror_parameter->set_default_param(tensor_info);
  return mirror_parameter;
}

PrimitivePtr ClonePrimitive(const CNodePtr &cnode) {
  MS_ASSERT(cnode != nullptr);
  auto origin_prim = GetValueNode<PrimitivePtr>(cnode->input(0));
  MS_ASSERT(origin_prim != nullptr);
  PrimitivePtr prim;
  auto op_primc_fns = ops::OpPrimCRegister::GetInstance().GetPrimCMap();
  if (op_primc_fns.find(origin_prim->name()) != op_primc_fns.end()) {
    prim = op_primc_fns[origin_prim->name()]();
  } else {
    prim = std::make_shared<PrimitiveC>(origin_prim->name());
    prim->set_instance_name(origin_prim->name());
  }
  prim->SetAttrs(origin_prim->attrs());
  return prim;
}

FuncGraphPtr CloneFuncGraph(const FuncGraphPtr &graph) {
  MS_ASSERT(graph != nullptr);
  auto mirror_graph = std::make_shared<FuncGraph>();
  mirror_graph->set_attrs(graph->attrs());
  NodesMap origin_nodes;
  NodesMap mirror_nodes;
  CloneGraphInputs(graph, mirror_graph, &origin_nodes, &mirror_nodes);
  auto node_list = TopoSort(graph->get_return());
  for (auto &node : node_list) {
    if (!utils::isa<mindspore::CNode>(node)) {
      continue;
    }
    auto cnode = node->cast<CNodePtr>();
    auto mirrro_prim = ClonePrimitive(cnode);
    std::vector<AnfNodePtr> node_inputs;
    for (size_t i = 1; i < cnode->size(); ++i) {
      auto origin_input = cnode->input(i);
      AnfNodePtr mirror_input = nullptr;
      auto value = origin_nodes[origin_input->fullname_with_scope()];
      auto iter = std::find(value.begin(), value.end(), origin_input);
      if (iter != value.end()) {
        mirror_input = mirror_nodes[origin_input->fullname_with_scope()][iter - value.begin()];
      }
      if (mirror_input == nullptr) {
        if (IsValueNode<FuncGraph>(origin_input)) {
          auto sub_func_graph = GetValueNode<FuncGraphPtr>(origin_input);
          auto mirror_sub_graph = CloneFuncGraph(sub_func_graph);
          mirror_input = NewValueNode(mirror_sub_graph);
        } else {
          mirror_input = CloneParameterAndValueNode(cnode, i, mirror_graph);
        }
        if (mirror_input == nullptr) {
          MS_LOG(ERROR) << "node input cannot be found.";
          return nullptr;
        }
        origin_nodes[origin_input->fullname_with_scope()].push_back(origin_input);
        mirror_nodes[origin_input->fullname_with_scope()].push_back(mirror_input);
      }
      node_inputs.push_back(mirror_input);
    }
    auto mirror_cnode = mirror_graph->NewCNode(mirrro_prim, node_inputs);
    mirror_cnode->set_fullname_with_scope(cnode->fullname_with_scope());
    if (cnode->abstract() != nullptr) {
      mirror_cnode->set_abstract(cnode->abstract()->Clone());
    }
    origin_nodes[cnode->fullname_with_scope()].push_back(cnode);
    mirror_nodes[cnode->fullname_with_scope()].push_back(mirror_cnode);
    if (opt::CheckPrimitiveType(cnode, prim::kPrimReturn)) {
      mirror_graph->set_return(mirror_cnode);
    }
  }
  return mirror_graph;
}

STATUS ExportModel(const FuncGraphPtr &graph) {
  MS_ASSERT(graph != nullptr && flags != nullptr);
  auto mirror_graph = CloneFuncGraph(graph);
  if (mirror_graph == nullptr) {
    MS_LOG(ERROR) << "Clone funcGraph failed.";
    return RET_ERROR;
  }
  (void)Manage(mirror_graph, true);
  auto format_pass = std::make_shared<opt::UnifyFormatPass>();
  format_pass->Init(flags->fmk, flags->trainModel);
  if (!format_pass->Run(mirror_graph)) {
    MS_LOG(ERROR) << "Run format pass failed.";
    return RET_ERROR;
  }
  auto optimizer = std::make_shared<opt::GraphOptimizer>();
  auto graph_pm = std::make_shared<opt::PassManager>("anf graph pass manager", true);
  if (flags->fmk == lite::converter::FmkType_TFLITE || flags->fmk == lite::converter::FmkType_TF ||
      flags->fmk == lite::converter::FmkType_ONNX) {
    graph_pm->AddPass(std::make_shared<opt::ControlFlowPass>());
  }
  optimizer->AddPassManager(graph_pm);
  if (optimizer->Optimize(mirror_graph) == nullptr) {
    MS_LOG(ERROR) << "run  graph pass failed.";
    return RET_ERROR;
  }
  auto meta_graph = Export(mirror_graph);
  if (meta_graph == nullptr) {
    MS_LOG(ERROR) << "Export to meta graph return nullptr";
    return RET_ERROR;
  }
  auto metagraph_transform = std::make_unique<GraphDefTransform>();
  metagraph_transform->SetGraphDef(meta_graph);
  auto status = metagraph_transform->Transform(*flags);
  if (status != RET_OK) {
    MS_LOG(ERROR) << "Transform meta graph failed " << status;
    return RET_ERROR;
  }
  meta_graph->version = Version();
  status = Storage::Save(*meta_graph, "model");
  std::ostringstream oss;
  if (status != RET_OK) {
    oss << "SAVE GRAPH FAILED:" << status << " " << lite::GetErrorInfo(status);
    MS_LOG(ERROR) << oss.str();
    std::cout << oss.str() << std::endl;
    return status;
  }

  delete meta_graph;
  oss << "CONVERT RESULT SUCCESS:" << status;
  MS_LOG(INFO) << oss.str();
  std::cout << oss.str() << std::endl;
  return status;
}

void ExportModelInit(converter::Flags *flag) {
  MS_ASSERT(flag != nullptr);
  flags = flag;
  InitDumpGraphFunc(ExportModel);
}
}  // namespace lite
}  // namespace mindspore
