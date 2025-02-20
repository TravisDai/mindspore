# Copyright 2020 Huawei Technologies Co., Ltd
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
import mindspore.context as context
from mindspore import Tensor, ms_function
from mindspore.common import dtype as mstype
import pytest

ZERO = Tensor([0], mstype.int32)
ONE = Tensor([1], mstype.int32)


@ms_function
def f(x):
    y = ZERO
    if x < 0:
        y = f(x - 3)
    elif x < 3:
        y = x * f(x - 1)
    elif x < 5:
        y = x * f(x - 2)
    else:
        y = f(x - 4)
    z = y + 1
    return z


@ms_function
def fr(x):
    y = ZERO
    if x < 0:
        y = ONE
    elif x < 3:
        y = x * fr(x - 1)
    elif x < 5:
        y = x * fr(x - 2)
    else:
        y = fr(x - 4)
    z = y + 1
    return z


def test_endless():
    context.set_context(mode=context.GRAPH_MODE)
    x = Tensor([5], mstype.int32)
    try:
        f(x)
    except RuntimeError as e:
        assert 'endless loop' in str(e)


@pytest.mark.skip(reason="backend is not supported yet")
def test_recrusive_fun():
    context.set_context(mode=context.GRAPH_MODE)
    x = Tensor([5], mstype.int32)
    ret = fr(x)
    expect = Tensor([3], mstype.int32)
    assert ret == expect


if __name__ == "__main__":
    test_endless()
