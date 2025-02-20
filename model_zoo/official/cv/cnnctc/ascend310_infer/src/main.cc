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
#include <sys/time.h>
#include <gflags/gflags.h>
#include <dirent.h>
#include <math.h>
#include <iostream>
#include <string>
#include <algorithm>
#include <iosfwd>
#include <vector>
#include <fstream>
#include <sstream>

#include "include/api/model.h"
#include "include/api/context.h"
#include "include/api/types.h"
#include "include/api/serialization.h"
#include "include/dataset/transforms.h"
#include "include/dataset/vision_ascend.h"
#include "include/dataset/execute.h"
#include "include/dataset/vision.h"
#include "inc/utils.h"

using mindspore::Context;
using mindspore::Serialization;
using mindspore::Model;
using mindspore::Status;
using mindspore::ModelType;
using mindspore::GraphCell;
using mindspore::kSuccess;
using mindspore::MSTensor;
using mindspore::DataType;
using mindspore::dataset::Execute;
using mindspore::dataset::InterpolationMode;
using mindspore::dataset::TensorTransform;
using mindspore::dataset::vision::Pad;
using mindspore::dataset::vision::Resize;
using mindspore::dataset::vision::HWC2CHW;
using mindspore::dataset::vision::Normalize;
using mindspore::dataset::vision::Decode;


DEFINE_string(mindir_path, "", "mindir path");
DEFINE_string(dataset_path, ".", "dataset path");
DEFINE_int32(device_id, 0, "device id");
DEFINE_int32(image_height, 32, "image height");
DEFINE_int32(image_width, 100, "image width");

int PadImage(const MSTensor &input, MSTensor *output) {
  auto normalize = Normalize({127.5, 127.5, 127.5}, {127.5, 127.5, 127.5});
  Execute composeNormalize(normalize);
  std::vector<int64_t> shape = input.Shape();
  auto imgResize = MSTensor();
  auto imgNormalize = MSTensor();
  int paddingSize;
  int NewWidth;
  float ratio;
  ratio = static_cast<float> (shape[1]) / static_cast<float> (shape[0]);
  NewWidth = ceil(FLAGS_image_height * ratio);
  paddingSize = FLAGS_image_width - NewWidth;
  if (NewWidth > FLAGS_image_width) {
    auto resize = Resize({FLAGS_image_height, FLAGS_image_width}, InterpolationMode::kArea);
    Execute composeResize(resize);
    composeResize(input, &imgResize);
    composeNormalize(imgResize, output);
  } else {
    auto resize = Resize({FLAGS_image_height, NewWidth}, InterpolationMode::kArea);
    Execute composeResize(resize);
    composeResize(input, &imgResize);
    composeNormalize(imgResize, &imgNormalize);
    auto pad = Pad({0, 0, paddingSize, 0});
    Execute composePad(pad);
    composePad(imgNormalize, output);
  }
  return 0;
}

int main(int argc, char **argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  if (RealPath(FLAGS_mindir_path).empty()) {
    std::cout << "Invalid mindir" << std::endl;
    return 1;
  }

  auto context = std::make_shared<Context>();
  auto ascend310 = std::make_shared<mindspore::Ascend310DeviceInfo>();
  ascend310->SetDeviceID(FLAGS_device_id);
  ascend310->SetPrecisionMode("allow_fp32_to_fp16");
  ascend310->SetOpSelectImplMode("high_precision");
  ascend310->SetBufferOptimizeMode("off_optimize");
  context->MutableDeviceInfo().push_back(ascend310);
  mindspore::Graph graph;
  Serialization::Load(FLAGS_mindir_path, ModelType::kMindIR, &graph);
  Model model;
  Status ret = model.Build(GraphCell(graph), context);
  if (ret != kSuccess) {
    std::cout << "ERROR: Build failed." << std::endl;
    return 1;
  }

  auto all_files = GetAllFiles(FLAGS_dataset_path);
  std::map<double, double> costTime_map;
  size_t size = all_files.size();
  auto decode = Decode();
  auto hwc2chw = HWC2CHW();
  Execute composeDecode(decode);
  Execute composeTranspose(hwc2chw);
  for (size_t i = 0; i < size; ++i) {
    struct timeval start = {0};
    struct timeval end = {0};
    double startTimeMs;
    double endTimeMs;
    std::vector<MSTensor> inputs;
    std::vector<MSTensor> outputs;
    auto imgDecode = MSTensor();
    auto imgTranspose = MSTensor();
    auto imgPad = MSTensor();
    auto img = MSTensor();
    composeDecode(ReadFileToTensor(all_files[i]), &imgDecode);
    std::vector<int64_t> shape = imgDecode.Shape();
    float ratio;
    int NewWidth;
    ratio = static_cast<float> (shape[1]) / static_cast<float> (shape[0]);
    NewWidth = ceil(FLAGS_image_height * ratio);
    PadImage(imgDecode, &imgPad);
    composeTranspose(imgPad, &img);
    if (NewWidth < FLAGS_image_width) {
      int img_size = FLAGS_image_width * FLAGS_image_height * 3;
      float *netOutput;
      netOutput = static_cast<float *>(img.MutableData());
      for (int j = 0; j < img_size; j += FLAGS_image_width) {
        float temp = 0;
        netOutput = netOutput + NewWidth - 1;
        temp = *netOutput;
        int n = NewWidth;
        netOutput++;
        while (n < FLAGS_image_width) {
          *netOutput = temp;
          netOutput++;
          n++;
        }
      }
    }

    std::vector<MSTensor> model_inputs = model.GetInputs();
    inputs.emplace_back(model_inputs[0].Name(), model_inputs[0].DataType(), model_inputs[0].Shape(),
                     img.Data().get(), img.DataSize());
    gettimeofday(&start, nullptr);
    ret = model.Predict(inputs, &outputs);
    gettimeofday(&end, nullptr);
    if (ret != kSuccess) {
      std::cout << "Predict " << all_files[i] << " failed." << std::endl;
      return 1;
    }
    startTimeMs = (1.0 * start.tv_sec * 1000000 + start.tv_usec) / 1000;
    endTimeMs = (1.0 * end.tv_sec * 1000000 + end.tv_usec) / 1000;
    costTime_map.insert(std::pair<double, double>(startTimeMs, endTimeMs));
    WriteResult(all_files[i], outputs);
  }
  double average = 0.0;
  int inferCount = 0;

  for (auto iter = costTime_map.begin(); iter != costTime_map.end(); iter++) {
    double diff = 0.0;
    diff = iter->second - iter->first;
    average += diff;
    inferCount++;
  }
  average = average / inferCount;
  std::stringstream timeCost;
  timeCost << "NN inference cost average time: "<< average << " ms of infer_count " << inferCount << std::endl;
  std::cout << "NN inference cost average time: "<< average << "ms of infer_count " << inferCount << std::endl;
  std::string fileName = "./time_Result" + std::string("/test_perform_static.txt");
  std::ofstream fileStream(fileName.c_str(), std::ios::trunc);
  fileStream << timeCost.str();
  fileStream.close();
  costTime_map.clear();
  return 0;
}
