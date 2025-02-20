# Contents

- [ICNet Description](#ICNet-description)
- [Model Architecture](#ICNet-Architeture)
- [Dataset](#ICNet-Dataset)
- [Environmental Requirements](#Environmental)
- [Script Description](#script-description)
    - [Script and Sample Code](#script-and-sample-code)
    - [Script Parameters](#script-parameters)
    - [Training Process](#training-process)
        - [Distributed Training](#distributed-training)
        - [Training Results](#training-results)
    - [Evaluation Process](#evaluation-process)
        - [Evaluation](#evaluation)
        - [Evaluation Result](#evaluation-result)
- [Model Description](#model-description)
- [Description of Random Situation](#description-of-random-situation)
- [ModelZoo Homepage](#modelzoo-homepage)

# [ICNet Description](#Contents)

ICNet(Image Cascade Network) propose a full convolution network which incorporates multi-resolution branches under proper label guidance to address the challenge of real-time semantic segmentation.

[paper](https://arxiv.org/abs/1704.08545)ECCV2018

# [Model Architecture](#Contents)

ICNet takes cascade image inputs (i.e., low-, medium- and high resolution images), adopts cascade feature fusion unit and is trained with cascade label guidance.The input image with full resolution (e.g., 1024×2048 in Cityscapes) is downsampled by factors of 2 and 4, forming cascade input to medium- and high-resolution branches.

# [Dataset](#Content)

used Dataset :[Cityscape Dataset Website](https://www.cityscapes-dataset.com/)

It contains 5,000 finely annotated images split into training, validation and testing sets with 2,975, 500, and 1,525 images respectively.

# [Environmental requirements](#Contents)

- Hardware :(Ascend)
    - Prepare ascend processor to build hardware environment
- frame:
    - [Mindspore](https://www.mindspore.cn/install)
- For details, please refer to the following resources:
    - [MindSpore course](https://www.mindspore.cn/tutorial/training/zh-CN/master/index.html)
    - [MindSpore Python API](https://www.mindspore.cn/doc/api_python/zh-CN/master/index.html)

# [Scription Description](#Content)

## Script and Sample Code

```python
.
└─ICNet
  ├─configs
    ├─icnet.yaml                          # config file
  ├─models
    ├─base_models
      ├─resnt50_v1.py                     # used resnet50
    ├─__init__.py
    ├─icnet.py                            # validation network
    ├─icnet_dc.py                         # training network
  ├─scripts
    ├─run_distribute_train8p.sh           # Multi card distributed training in ascend
    ├─run_eval.sh                         # validation script
  ├─utils
    ├─__init__.py
    ├─logger.py                           # logger
    ├─loss.py                             # loss
    ├─losses.py                           # SoftmaxCrossEntropyLoss
    ├─lr_scheduler.py                     # lr
    └─metric.py                           # metric
  ├─eval.py                               # validation
  ├─train.py                              # train
  └─visualize.py                          # inference visualization
```

## Script Parameters

Set script parameters in configs/icnet.yaml .

### Model

```bash
name: "icnet"
backbone: "resnet50"
base_size: 1024    # during augmentation, shorter size will be resized between [base_size*0.5, base_size*2.0]
crop_size: 960     # end of augmentation, crop to training
```

### Optimizer

```bash
init_lr: 0.02
momentum: 0.9
weight_decay: 0.0001
```

### Training

```bash
train_batch_size_percard: 4
valid_batch_size: 1
cityscapes_root: "/data/cityscapes/"
epochs: 160
val_epoch: 1        # run validation every val-epoch
ckpt_dir: "./ckpt/" # ckpt and training log will be saved here
mindrecord_dir: '/root/bigpingping/mindrecord'
save_checkpoint_epochs: 5
keep_checkpoint_max: 10
```

### Valid

```bash
ckpt_path: ""       # set the pretrained model path correctly
```

## Training Process

### Distributed Training

- Run distributed train in ascend processor environment

```shell
    bash scripts/run_distribute_train.sh [RANK_TABLE_FILE] [PROJECT_PATH]
```

- Notes:

The hccl.json file specified by [RANK_TABLE_FILE] is used when running distributed tasks. You can use [hccl_tools](https://gitee.com/mindspore/mindspore/tree/master/model_zoo/utils/hccl_tools) to generate this file.

### Training Result

The training results will be saved in the example path, The folder name starts with "ICNet-".You can find the checkpoint file and similar results below in LOG(0-7)/log.txt.

```bash
# distributed training result(8p)
epoch: 1 step: 93, loss is 0.5659234
epoch time: 672111.671 ms, per step time: 7227.007 ms
epoch: 2 step: 93, loss is 1.0220546
epoch time: 66850.354 ms, per step time: 718.821 ms
epoch: 3 step: 93, loss is 0.49694514
epoch time: 70490.510 ms, per step time: 757.962 ms
epoch: 4 step: 93, loss is 0.74727297
epoch time: 73657.396 ms, per step time: 792.015 ms
epoch: 5 step: 93, loss is 0.45953503
epoch time: 97117.785 ms, per step time: 1044.277 ms
```

## Evaluation Process

### Evaluation

Check the checkpoint path used for evaluation before running the following command.

```shell
    bash run_eval.sh [DATASET_PATH] [CHECKPOINT_PATH] [PROJECT_PATH]
```

### Evaluation Result

The results at eval/log were as follows:

```bash
Found 500 images in the folder /data/cityscapes/leftImg8bit/val
pretrained....
2021-06-01 19:03:54,570 semantic_segmentation INFO: Start validation, Total sample: 500
avgmiou 0.69962835
avg_pixacc 0.94285786
avgtime 0.19648232793807982
````

# [Model Description](#Content)

## Performance

### Training Performance

|Parameter              | MaskRCNN                                                   |
| ------------------- | --------------------------------------------------------- |
|resources              | Ascend 910；CPU 2.60GHz, 192core；memory：755G |
|Upload date            |2021.6.1                    |
|mindspore version      |mindspore1.2.0     |
|training parameter     |epoch=160,batch_size=32   |
|optimizer              |SGD optimizer，momentum=0.9,weight_decay=0.0001    |
|loss function          |SoftmaxCrossEntropyLoss   |
|training speed         | epoch time：285693.557 ms per step time :42.961 ms |
|total time             |about 5 hours    |
|Script URL             |   |
|Random number seed     |set_seed = 1234     |

# [Description of Random Situation](#Content)

The seed in the `create_icnet_dataset` function is set in `cityscapes_mindrecord.py`, and the random seed in `train.py` is also used for weight initialization.

# [ModelZoo Homepage](#Content)

Please visit the official website [homepage](https://gitee.com/mindspore/mindspore/tree/master/model_zoo).
