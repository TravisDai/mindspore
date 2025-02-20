/**
 * Copyright 2020 Huawei Technologies Co., Ltd
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
#ifndef MINDSPORE_INCLUDE_API_SERIALIZATION_H
#define MINDSPORE_INCLUDE_API_SERIALIZATION_H

#include <string>
#include <vector>
#include <map>
#include <memory>
#include "include/api/status.h"
#include "include/api/types.h"
#include "include/api/model.h"
#include "include/api/graph.h"
#include "include/api/dual_abi_helper.h"

namespace mindspore {
using Key = struct Key {
  const size_t max_key_len = 32;
  size_t len;
  unsigned char key[32];
  Key() : len(0) {}
};

class MS_API Serialization {
 public:
  static Status Load(const void *model_data, size_t data_size, ModelType model_type, Graph *graph);
  inline static Status Load(const void *model_data, size_t data_size, ModelType model_type, Graph *graph,
                            const Key &dec_key, const std::string &dec_mode);
  inline static Status Load(const std::string &file, ModelType model_type, Graph *graph);
  inline static Status Load(const std::string &file, ModelType model_type, Graph *graph, const Key &dec_key,
                            const std::string &dec_mode);
  inline static Status Load(const std::vector<std::string> &files, ModelType model_type, std::vector<Graph> *graphs,
                            const Key &dec_key = {}, const std::string &dec_mode = "AES-GCM");
  static Status LoadCheckPoint(const std::string &ckpt_file, std::map<std::string, Buffer> *parameters);
  static Status SetParameters(const std::map<std::string, Buffer> &parameters, Model *model);
  static Status ExportModel(const Model &model, ModelType model_type, Buffer *model_data);
  static Status ExportModel(const Model &model, ModelType model_type, const std::string &model_file);

 private:
  static Status Load(const void *model_data, size_t data_size, ModelType model_type, Graph *graph, const Key &dec_key,
                     const std::vector<char> &dec_mode);
  static Status Load(const std::vector<char> &file, ModelType model_type, Graph *graph);
  static Status Load(const std::vector<char> &file, ModelType model_type, Graph *graph, const Key &dec_key,
                     const std::vector<char> &dec_mode);
  static Status Load(const std::vector<std::vector<char>> &files, ModelType model_type, std::vector<Graph> *graphs,
                     const Key &dec_key, const std::vector<char> &dec_mode);
};

Status Serialization::Load(const void *model_data, size_t data_size, ModelType model_type, Graph *graph,
                           const Key &dec_key, const std::string &dec_mode) {
  return Load(model_data, data_size, model_type, graph, dec_key, StringToChar(dec_mode));
}

Status Serialization::Load(const std::string &file, ModelType model_type, Graph *graph) {
  return Load(StringToChar(file), model_type, graph);
}

Status Serialization::Load(const std::string &file, ModelType model_type, Graph *graph, const Key &dec_key,
                           const std::string &dec_mode) {
  return Load(StringToChar(file), model_type, graph, dec_key, StringToChar(dec_mode));
}

Status Serialization::Load(const std::vector<std::string> &files, ModelType model_type, std::vector<Graph> *graphs,
                           const Key &dec_key, const std::string &dec_mode) {
  return Load(VectorStringToChar(files), model_type, graphs, dec_key, StringToChar(dec_mode));
}
}  // namespace mindspore
#endif  // MINDSPORE_INCLUDE_API_SERIALIZATION_H
