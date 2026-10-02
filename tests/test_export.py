"""The exported C++ bot (export/) does exactly what the integer actor does.

A small randomly initialised actor is exported and held to the export gate
natively: on every turn of recorded games, the bot built with the host
compiler must compute the same observation and reply as training's core and
the integer actor. RUN_SANDBOX_TESTS=1 also builds it with the judge's clang
and runs it in the judge's metered sandbox (slower: it compiles clang).
"""
import os
import pathlib
import shutil
import sys

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

torch = pytest.importorskip("torch")
bccore = pytest.importorskip("bccore", reason="build the C++ core first (see README)")

from export.export_bot import write_bot  # noqa: E402
from export.gate import run_gate  # noqa: E402
from export.quantize import quantize  # noqa: E402
from rl.policy import Actor  # noqa: E402


@pytest.fixture(scope="module")
def exported(tmp_path_factory):
    if shutil.which("c++") is None:
        pytest.skip("no host C++ compiler")
    torch.manual_seed(1)
    actor = Actor(bccore.OBS_SIZE, bccore.NUM_ACTIONS, bccore.feature_scales(), (64, 32))
    actor.qat = True
    actor.eval()
    integer = quantize(actor)
    out = tmp_path_factory.mktemp("bot")
    write_bot(integer, 1, out, "test")
    return out, actor, integer


def test_bot_matches_training_turn_for_turn_natively(exported):
    out, actor, integer = exported
    report = run_gate(out, actor, integer, games=6, sandbox=False)
    print(report.summary())
    assert report.passed
    assert report.native_turns > 500 and report.native_obs_mismatch == 0 and report.native_reply_mismatch == 0


@pytest.mark.skipif(os.environ.get("RUN_SANDBOX_TESTS") != "1", reason="set RUN_SANDBOX_TESTS=1")
def test_bot_matches_in_the_judges_sandbox(exported):
    pytest.importorskip("unswbc")
    out, actor, integer = exported
    report = run_gate(out, actor, integer, games=4, sandbox=True, sandbox_turns=400)
    print(report.summary())
    assert report.passed and report.sandbox_turns > 0
