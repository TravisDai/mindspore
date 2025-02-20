#!/bin/bash
# Copyright 2021 Huawei Technologies Co., Ltd
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ============================================================================

if [ $# != 4 ]
then
    echo "============================================================================================================"
    echo "Please run the script as: "
    echo "bash run_eval.sh [TASK_NAME] [DEVICE_TARGET] [EVAL_DATA_DIR] [LOAD_CKPT_PATH]"
    echo "for example: bash run_eval.sh STS-B Ascend /path/sts-b/eval.tf_record /path/xxx.ckpt"
    echo "============================================================================================================"
exit 1
fi

echo "===============================================start evaling================================================"

task_name=$1
device_target=$2
eval_data_dir=$3
load_ckpt_path=$4

mkdir -p ms_log
PROJECT_DIR=$(cd "$(dirname "$0")"; pwd)
CUR_DIR=`pwd`
export GLOG_log_dir=${CUR_DIR}/ms_log
export GLOG_logtostderr=0

python ${PROJECT_DIR}/../eval.py \
    --task_name=$task_name \
    --device_target=$device_target \
    --device_id=0 \
    --load_ckpt_path=$load_ckpt_path \
    --eval_data_dir=$eval_data_dir \
    --do_quant=True > eval_log.txt 2>&1 &

