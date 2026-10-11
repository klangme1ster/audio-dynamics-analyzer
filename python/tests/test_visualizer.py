"""Plot output: size, aspect and format (plan 2.1-2.4)."""

from __future__ import annotations

import matplotlib.image as mpimg
import pytest

from ada_core.visualizer import parse_aspect, plot_result
from conftest import analyze_array
import signals

FS = 48000


@pytest.fixture(scope="module")
def result():
    x = signals.tone_sequence(FS, [(10, -20.0), (10, -30.0)])
    return analyze_array(x, FS)


@pytest.mark.parametrize("width,aspect,expected", [
    (2160, "16:10", (1350, 2160)),
    (1200, "4:3", (900, 1200)),
    (1000, "2", (500, 1000)),
])
def test_png_pixel_size(tmp_path, result, width, aspect, expected):
    out = tmp_path / "p.png"
    plot_result(result, "test", out, width_px=width, aspect=parse_aspect(aspect))
    assert mpimg.imread(out).shape[:2] == expected


@pytest.mark.parametrize("fmt", ["pdf", "svg"])
def test_vector_formats(tmp_path, result, fmt):
    out = tmp_path / f"p.{fmt}"
    plot_result(result, "test", out)
    assert out.stat().st_size > 1000


def test_parse_aspect_rejects_garbage():
    with pytest.raises(ValueError):
        parse_aspect("wide")
