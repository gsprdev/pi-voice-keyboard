# /// script
# requires-python = ">=3.12,<3.13"
# dependencies = ["openai-whisper", "torch"]  # mise run model adds openvino, pinned to the build's
#
# [tool.uv.sources]
# torch = { index = "pytorch-cpu" }
#
# [[tool.uv.index]]
# name = "pytorch-cpu"
# url = "https://download.pytorch.org/whl/cpu"
# explicit = true
# ///
"""Convert a Whisper model's encoder to OpenVINO IR, named as whisper.cpp looks for it.

Replaces whisper.cpp's models/convert-whisper-to-openvino.py, which imports openvino.runtime,
gone since OpenVINO 2026. Usage: openvino-encoder.py <model> <dir>
"""

import os
import sys
import tempfile

import openvino as ov
import torch
import whisper

name, out = sys.argv[1:]
# The PyTorch checkpoint is only needed here, so it goes to a temporary directory, not ~/.cache/whisper
with tempfile.TemporaryDirectory() as checkpoints:
    model = whisper.load_model(name, device="cpu", download_root=checkpoints)
mel = torch.zeros((1, model.dims.n_mels, 3000))  # Whisper's fixed 30s window
encoder = ov.convert_model(model.encoder.eval(), example_input=mel)
# Full precision, like upstream's; each device plugin picks its own inference precision
# Written under temporary names, the .xml last, since mise run model skips the conversion once it exists
final = f"{out}/ggml-{name}-encoder-openvino"
tmp = f"{out}/.ggml-{name}-encoder-openvino.tmp"
ov.save_model(encoder, tmp + ".xml", compress_to_fp16=False)
os.replace(tmp + ".bin", final + ".bin")
os.replace(tmp + ".xml", final + ".xml")
