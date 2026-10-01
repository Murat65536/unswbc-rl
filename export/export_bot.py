"""Turns a PPO checkpoint (rl/train.py's .pt files) into a submittable
`unswbc` bot project: a plain-NumPy main.py (bot_template/main.py.tmpl) plus
its weights, using the real helper.py so it actually speaks the wire
protocol.

Usage:
    python -m export.export_bot --checkpoint checkpoints/latest.pt --out bots/rl_bot_v1
    unswbc run maps/arena.map bots/rl_bot_v1 bots/rl_bot_v1   # try it locally
    unswbc submit bots/rl_bot_v1                              # upload it
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
from pathlib import Path

import numpy as np
import torch

_TEMPLATE_DIR = Path(__file__).resolve().parent / "bot_template"


_WEIGHT_FILES = {
    "trunk.0.weight": "w0.bin", "trunk.0.bias": "b0.bin",
    "trunk.2.weight": "w2.bin", "trunk.2.bias": "b2.bin",
    "actor.weight": "wa.bin", "actor.bias": "ba.bin",
}


def export(checkpoint_path: str, bot_dir: str):
    bot_dir = Path(bot_dir)
    ckpt = torch.load(checkpoint_path, map_location="cpu")
    state_dict = ckpt["model"] if isinstance(ckpt, dict) and "model" in ckpt else ckpt
    weights = {k: v.detach().cpu().numpy().astype("<f4") for k, v in state_dict.items()}
    hidden = weights["trunk.0.weight"].shape[0]

    if not (bot_dir / "helper.py").exists():
        bot_dir.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["unswbc", "init", "python", str(bot_dir)], check=True)

    total_bytes = 0
    for key, filename in _WEIGHT_FILES.items():
        arr = np.ascontiguousarray(weights[key])
        arr.tofile(bot_dir / filename)
        total_bytes += arr.nbytes

    template = (_TEMPLATE_DIR / "main.py.tmpl").read_text()
    template = template.replace("__HIDDEN__", str(hidden))
    (bot_dir / "main.py").write_text(template)
    shutil.copy(_TEMPLATE_DIR / "bot_encoding.py", bot_dir / "bot_encoding.py")

    toml_path = bot_dir / "bot.toml"
    text = toml_path.read_text()
    if "*.bin" not in text:
        text = text.replace('include = [ "*.py" ]', 'include = [ "*.py", "*.bin" ]')
        toml_path.write_text(text)

    size_kb = total_bytes / 1024
    print(f"exported bot to {bot_dir} (hidden={hidden}, weights: {size_kb:.0f} KiB raw)")
    print(f"try it locally:  unswbc run maps/arena.map {bot_dir} {bot_dir}")
    print(f"sandboxed timing: unswbc run maps/arena.map {bot_dir} {bot_dir} --sandbox -v")
    print(f"submit it:        unswbc submit {bot_dir}")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--checkpoint", required=True)
    p.add_argument("--out", required=True)
    args = p.parse_args()
    export(args.checkpoint, args.out)


if __name__ == "__main__":
    main()
