"""The SPRT in export/evaluate.py decides the obvious cases the right way."""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from export.evaluate import expected_score, sprt_bounds, sprt_llr  # noqa: E402


def test_expected_score():
    assert expected_score(0) == 0.5
    assert abs(expected_score(400) - 10 / 11) < 1e-12


def test_sprt_accepts_a_clearly_stronger_bot():
    lower, upper = sprt_bounds(0.05, 0.05)
    scores = [1.0] * 70 + [0.0] * 30          # 70% score: about +147 Elo
    assert sprt_llr(scores, 0, 30) > upper


def test_sprt_rejects_an_equal_bot():
    lower, upper = sprt_bounds(0.05, 0.05)
    scores = ([1.0, 0.0] * 300) + [0.5] * 100  # dead even
    assert sprt_llr(scores, 0, 30) < lower


def test_sprt_waits_on_little_evidence():
    lower, upper = sprt_bounds(0.05, 0.05)
    assert lower < sprt_llr([1.0, 0.0, 1.0, 1.0], 0, 30) < upper
