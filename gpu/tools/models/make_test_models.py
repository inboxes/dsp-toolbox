"""Generates the tiny test networks for the GPU runtime tests (dev-only; run it in a venv that has torch, onnx and
onnxruntime, any environment with those three packages). The outputs are committed, a few KB each.

For each hop of 10, 20 and 40 ms at 48 kHz, with a left context of 960 frames, the window is 960 + hop frames and
each model maps [1, 1, window] to [1, 1, hop]:

  gain_h<hop>          y = 0.5 x on the last `hop` frames, as a 1x1 Conv1d (exact, no TF32)
  many_kernels_h<hop>  200 tiny layers (1x1 convolutions with weights 2 and 0.5, and elementwise +c and -c) whose net
                       effect is y = 0.5 x. Launch-bound on purpose, to show the gain from CUDA graphs.

Each exists as .onnx and as TorchScript (.ts, torch.jit.trace) with the same weights.

    python make_test_models.py <output dir>
"""
import sys

import numpy as np
import torch

CONTEXT = 960
HOPS = {"10": 480, "20": 960, "40": 1920}
LAYERS = 200

torch.backends.cuda.matmul.allow_tf32 = False
torch.backends.cudnn.allow_tf32 = False


def conv1x1(weight: float) -> torch.nn.Conv1d:
    c = torch.nn.Conv1d(1, 1, 1, bias=False)
    with torch.no_grad():
        c.weight.fill_(weight)
    return c


class Gain(torch.nn.Module):
    def __init__(self, hop: int):
        super().__init__()
        self.hop = hop
        self.conv = conv1x1(0.5)

    def forward(self, x):
        return self.conv(x[:, :, -self.hop:])


class ManyKernels(torch.nn.Module):
    """x -> (x * 2 + 0.25 - 0.25) * 0.5 ... repeated, ending at 0.5 x. Every layer is a separate GPU kernel."""

    def __init__(self, hop: int):
        super().__init__()
        self.hop = hop
        self.up = torch.nn.ModuleList([conv1x1(2.0) for _ in range(LAYERS // 4)])
        self.down = torch.nn.ModuleList([conv1x1(0.5) for _ in range(LAYERS // 4)])

    def forward(self, x):
        y = x[:, :, -self.hop:]
        for up, down in zip(self.up, self.down):
            y = up(y)           # x 2
            y = y + 0.25        # + c
            y = y - 0.25        # - c
            y = down(y)         # x 0.5
        return y * 0.5


def export(model: torch.nn.Module, name: str, window: int, out_dir: str) -> None:
    model.eval()
    x = torch.from_numpy(np.sin(np.arange(window, dtype=np.float32) * 0.03)).reshape(1, 1, window)
    with torch.no_grad():
        ref = model(x)
        torch.onnx.export(model, x, f"{out_dir}/{name}.onnx", input_names=["x"], output_names=["y"], opset_version=17,
                          dynamo=False)
        torch.jit.trace(model, x).save(f"{out_dir}/{name}.ts")
    expected = 0.5 * x[:, :, -ref.shape[-1]:]
    err = float((ref - expected).abs().max())
    print(f"{name}: window {window}, out {tuple(ref.shape)}, max |y - 0.5 x| = {err:.2e}")


def main() -> None:
    out_dir = sys.argv[1]
    for ms, hop in HOPS.items():
        window = CONTEXT + hop
        export(Gain(hop), f"gain_h{ms}", window, out_dir)
        export(ManyKernels(hop), f"many_kernels_h{ms}", window, out_dir)


if __name__ == "__main__":
    main()
