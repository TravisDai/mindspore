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

#ifndef MINDSPORE_CCSRC_PS_WORKER_FL_WORKER_H_
#define MINDSPORE_CCSRC_PS_WORKER_FL_WORKER_H_

#include <memory>
#include <string>
#include <vector>
#include "proto/comm.pb.h"
#include "schema/fl_job_generated.h"
#include "ps/ps_context.h"
#include "ps/core/worker_node.h"
#include "ps/core/cluster_metadata.h"
#include "ps/core/communicator/tcp_communicator.h"

namespace mindspore {
namespace ps {
using FBBuilder = flatbuffers::FlatBufferBuilder;

// The step number for worker to judge whether to communicate with server.
constexpr uint32_t kTrainBeginStepNum = 1;
constexpr uint32_t kTrainEndStepNum = 0;

// The worker has to sleep for a while before the networking is completed.
constexpr uint32_t kWorkerSleepTimeForNetworking = 1000;

// The time duration between retrying when server is in safemode.
constexpr uint32_t kWorkerRetryDurationForSafeMode = 500;

enum class IterationState {
  // This iteration is still in process.
  kRunning,
  // This iteration is completed and the next iteration is not started yet.
  kCompleted
};

namespace worker {
// This class is used for hybrid training mode for now. In later version, parameter server mode will also use this class
// as worker.
class FLWorker {
 public:
  static FLWorker &GetInstance() {
    static FLWorker instance;
    return instance;
  }
  void Run();
  void Finalize();
  bool SendToServer(uint32_t server_rank, void *data, size_t size, core::TcpUserCommand command,
                    std::shared_ptr<std::vector<unsigned char>> *output = nullptr);

  uint32_t server_num() const;
  uint32_t worker_num() const;
  uint64_t worker_step_num_per_iteration() const;

  // These methods set the worker's iteration state.
  void SetIterationRunning();
  void SetIterationCompleted();

 private:
  FLWorker()
      : server_num_(0),
        worker_num_(0),
        scheduler_ip_(""),
        scheduler_port_(0),
        worker_node_(nullptr),
        worker_step_num_per_iteration_(1),
        server_iteration_state_(IterationState::kCompleted),
        worker_iteration_state_(IterationState::kCompleted),
        safemode_(false) {}
  ~FLWorker() = default;
  FLWorker(const FLWorker &) = delete;
  FLWorker &operator=(const FLWorker &) = delete;

  // Initialize the scaler for worker
  void InitializeFollowerScaler();

  // The handlers for the iteration state events.
  void HandleIterationRunningEvent();
  void HandleIterationCompletedEvent();

  // The barriers before scaling operations.
  void ProcessBeforeScalingOut();
  void ProcessBeforeScalingIn();

  // The handlers after scheduler's scaling operations are done.
  void ProcessAfterScalingOut();
  void ProcessAfterScalingIn();

  uint32_t server_num_;
  uint32_t worker_num_;
  std::string scheduler_ip_;
  uint16_t scheduler_port_;
  std::shared_ptr<core::WorkerNode> worker_node_;

  // The worker standalone training step number before communicating with server. This used in hybrid training mode for
  // now.
  uint64_t worker_step_num_per_iteration_;

  // The iteration state is either running or completed.
  // This variable represents the server iteration state and should be changed by events
  // kIterationRunning/kIterationCompleted. triggered by server.
  std::atomic<IterationState> server_iteration_state_;

  // The variable represents the worker iteration state and should be changed by worker training process.
  std::atomic<IterationState> worker_iteration_state_;

  // The flag that represents whether worker is in safemode, which is decided by both worker and server iteration state.
  std::atomic_bool safemode_;
};
}  // namespace worker
}  // namespace ps
}  // namespace mindspore
#endif  // MINDSPORE_CCSRC_PS_WORKER_FL_WORKER_H_
