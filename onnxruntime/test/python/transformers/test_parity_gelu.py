# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation.  All rights reserved.
# Licensed under the MIT License.  See License.txt in the project root for
# license information.
# --------------------------------------------------------------------------
"""
Below are test results for Gelu or FastGelu FP32 kernels using CUDA:

Formula             Input(BeforeCast)  MaxDiff     MaxDiff(Optimized)
0(gelu_python)      FP32               2.38E-07    4.77E-07
0(gelu_python)      FP16               0           6.10E-05
1(gelu)             FP32               4.77E-07    0
1(gelu)             FP16               6.10E-05    0
2(erf_gelu)         FP32               2.38E-07    9.54E-07
2(erf_gelu)         FP16               1.22E-04    1.95E-03
3(gelu_new)         FP32               2.38E-07    2.38E-07
3(gelu_new)         FP16               0           0
4(gelu_fast)        FP32               0           2.38E-07
4(gelu_fast)        FP16               0           3.05E-05
5(openai_gelu)      FP32               0           2.38E-07
5(openai_gelu)      FP16               0           3.05E-05

For comparison, CPU has MaxDiff=4.77E-07 for each formula.
"""

import unittest
import torch
from torch import nn
import math
import os
from parity_utilities import *


class Gelu(nn.Module):
    def __init__(self, formula=4):
        super().__init__()
        self.formula = formula

    def gelu(self, x):
        if self.formula == 0:
            return x * 0.5 * (1.0 + torch.erf(x / math.sqrt(2.0)))
        elif self.formula == 1:
            return nn.functional.gelu(x)
        elif self.formula == 2:
            # erf_gelu in Megatron: x * 0.5 * (torch.erf(x / 1.41421).to(dtype=x.dtype)+torch.ones_like(x).to(dtype=x.dtype))
            return x * 0.5 * (torch.erf(x / 1.41421).to(dtype=x.dtype) + 1.0)
        elif self.formula == 3:
            # gelu_new in huggingface transformers
            return 0.5 * x * (1.0 + torch.tanh(math.sqrt(2.0 / math.pi) * (x + 0.044715 * torch.pow(x, 3.0))))
        elif self.formula == 4:
            # gelu_fast in huggingface transformers with lower precision in a constant (0.7978845608)
            return 0.5 * x * (1.0 + torch.tanh(x * 0.7978845608 * (1.0 + 0.044715 * x * x)))
        else:
            # openai_gelu in Megatron
            return 0.5 * x * (1.0 + torch.tanh(0.7978845608028654 * x * (1.0 + 0.044715 * x * x)))

    @staticmethod
    def get_fused_op(formula):
        return "Gelu" if formula in [0, 1, 2] else "FastGelu"

    def forward(self, x):
        if x.dtype == torch.float16:
            # This test only evaluates FP32 kernels so add data type cast for input and output.
            fp16_gelu = self.gelu(x.to(torch.float32)).to(torch.float16)
            return (fp16_gelu, )
        else:
            fp32_gelu = self.gelu(x)
            return (fp32_gelu, )


def get_output_names():
    outputs = ["output"]
    return outputs


def run(batch_size, float16, optimized, hidden_size, device, test_cases, formula=0, sequence_length=2):
    test_name = f"device={device}, float16={float16}, optimized={optimized}, batch_size={batch_size}, sequence_length={sequence_length}, hidden_size={hidden_size}, formula={formula}"
    print(f"\nTesting: {test_name}")

    model = Gelu(formula=formula)
    model.eval()
    model.to(device)
    if float16:
        model.half()

    # Do not re-use onnx file from previous test since weights of model are random.
    onnx_model_path = './temp/gelu_{}_{}.onnx'.format(formula, "fp16" if float16 else "fp32")
    export_onnx(model, onnx_model_path, float16, hidden_size, device)

    if optimized:
        optimized_onnx_path = './temp/gelu_{}_opt_{}.onnx'.format(formula, "fp16" if float16 else "fp32")
        optimize_onnx(onnx_model_path, optimized_onnx_path, Gelu.get_fused_op(formula))
        onnx_path = optimized_onnx_path
    else:
        onnx_path = onnx_model_path

    num_failure = run_parity(model, onnx_path, batch_size, hidden_size, sequence_length, float16, device, optimized,
                             test_cases)

    # clean up onnx file
    os.remove(onnx_model_path)
    if optimized:
        os.remove(onnx_path)

    return num_failure, test_name


class TestGeluParity(unittest.TestCase):
    def setUp(self):
        self.optimized = True  # Change it to False if you want to test parity of non optimized ONNX
        self.test_cases = 100  # Number of test cases per test run
        self.sequence_length = 2
        self.hidden_size = 768
        self.formula_to_test = [0, 1, 2, 3, 4, 5]
        self.formula_must_pass = [0, 1, 3, 4, 5]  # formula 2 cannot pass precision test.

    def run_test(self, batch_size, float16, optimized, hidden_size, device, formula, enable_assert=True):
        if float16 and device.type == 'cpu':  # CPU does not support FP16
            return
        num_failure, test_name = run(batch_size, float16, optimized, hidden_size, device, self.test_cases, formula,
                                     self.sequence_length)
        if enable_assert:
            self.assertTrue(num_failure == 0, "Failed: " + test_name)

    def run_one(self, optimized, device, hidden_size=768, formula=0):
        for batch_size in [4]:
            self.run_test(batch_size,
                          float16=False,
                          optimized=optimized,
                          hidden_size=hidden_size,
                          device=device,
                          formula=formula,
                          enable_assert=formula in self.formula_must_pass)

            self.run_test(batch_size,
                          float16=True,
                          optimized=optimized,
                          hidden_size=hidden_size,
                          device=device,
                          formula=formula,
                          enable_assert=formula in self.formula_must_pass)

    def test_cpu(self):
        cpu = torch.device('cpu')
        for i in self.formula_to_test:
            self.run_one(self.optimized, cpu, hidden_size=self.hidden_size, formula=i)

    def test_cuda(self):
        if not torch.cuda.is_available():
            import pytest
            pytest.skip('test requires GPU and torch+cuda')
        else:
            gpu = torch.device('cuda')
            for i in self.formula_to_test:
                self.run_one(self.optimized, gpu, hidden_size=self.hidden_size, formula=i)


if __name__ == '__main__':
    unittest.main()
