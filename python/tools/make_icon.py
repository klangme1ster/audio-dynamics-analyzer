"""Draw the application icon: a PSR meter in the app's zone colours.

A dark rounded square holds a vertical bar coloured by the PSR zones (red < 8 dB at the bottom,
orange 8-10, yellow 10-12, green >= 12 at the top, on the meter's 0-24 dB scale) with a white
marker at the "now" value. From 48 px up, a waveform beside it breathes on the left and is
pressed flat against its ceiling on the right: how squashed is this master? Small sizes show the
bar alone, so the taskbar icon stays readable. Colours are those of src/plugin/Theme.h.

    python python/tools/make_icon.py            # writes resources/icon/

Output: ada-icon.svg (vector, full variant), ada-icon-<size>.png and ada-icon.ico (16-256 px).
The build uses ada-icon-1024.png (ICON_BIG) and ada-icon-32.png (ICON_SMALL).
"""

from __future__ import annotations

from pathlib import Path

import matplotlib

matplotlib.use("Agg")
matplotlib.rcParams["svg.hashsalt"] = "ada-icon"  # stable ids: regenerating gives no spurious diff
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from matplotlib.patches import FancyBboxPatch, Rectangle  # noqa: E402
from PIL import Image  # noqa: E402  (Pillow comes with matplotlib)

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / "resources" / "icon"

BACKGROUND = "#222221"   # theme::panel
BORDER = "#383835"       # theme::grid
MARKER = "#f0efec"       # theme::textPrimary
WAVE = "#3987e5"         # theme::shortTerm
CEILING = "#d95926"      # theme::truePeak
ZONES = [(0, 8, "#d03b3b"), (8, 10, "#ec835a"), (10, 12, "#fab219"), (12, 24, "#0ca30c")]
PSR_NOW = 14.0           # marker position on the 0-24 dB scale

PNG_SIZES = [16, 24, 32, 48, 64, 128, 256, 512, 1024]
ICO_SIZES = [16, 24, 32, 48, 64, 128, 256]
SMALL = 32               # up to this size: bar only


def draw(size_px: int | None, full: bool) -> plt.Figure:
    inches = (size_px or 1024) / 100
    fig = plt.figure(figsize=(inches, inches), dpi=100)
    ax = fig.add_axes((0, 0, 1, 1))
    ax.set_xlim(0, 1)
    ax.set_ylim(0, 1)
    ax.set_axis_off()
    fig.patch.set_alpha(0)

    # rounded square; a thin border only where it is more than a pixel wide
    inset = 0.02
    border = 0.0 if size_px and size_px < 48 else (size_px or 1024) / 1024 * 6
    ax.add_patch(FancyBboxPatch((inset, inset), 1 - 2 * inset, 1 - 2 * inset,
                                boxstyle="round,pad=0,rounding_size=0.2",
                                facecolor=BACKGROUND, edgecolor=BORDER, linewidth=border))

    # the meter: zone colours on a 0-24 dB scale, clipped to a rounded bar
    x0, width = (0.17, 0.25) if full else (0.32, 0.36)
    y0, y1 = 0.14, 0.86
    to_y = lambda psr: y0 + (y1 - y0) * psr / 24.0  # noqa: E731
    clip = FancyBboxPatch((x0, y0), width, y1 - y0, boxstyle=f"round,pad=0,rounding_size={width * 0.22}",
                          facecolor="none", edgecolor="none")
    ax.add_patch(clip)
    for lo, hi, colour in ZONES:
        zone = Rectangle((x0, to_y(lo)), width, to_y(hi) - to_y(lo), facecolor=colour, edgecolor="none")
        ax.add_patch(zone)
        zone.set_clip_path(clip)

    # "now" marker: a white line across the bar, reaching a little beyond it
    marker_h = 0.045 if full else 0.07
    ax.add_patch(Rectangle((x0 - 0.05, to_y(PSR_NOW) - marker_h / 2), width + 0.10, marker_h,
                           facecolor=MARKER, edgecolor="none"))

    if full:
        # waveform: free transients on the left, limited flat against the ceiling on the right
        free = [0.13, 0.30, 0.20, 0.34, 0.16, 0.27]   # transients that breathe
        ceiling = 0.17
        amp = free + [ceiling] * 6                    # limited: every peak at the ceiling
        xs = np.linspace(0.53, 0.85, len(amp))
        bar_w = (xs[1] - xs[0]) * 0.62
        for x, a in zip(xs, amp):
            ax.add_patch(FancyBboxPatch((x - bar_w / 2, 0.5 - a), bar_w, 2 * a,
                                        boxstyle=f"round,pad=0,rounding_size={bar_w / 2}",
                                        facecolor=WAVE, edgecolor="none"))
        # the ceiling the right half is pressed against
        x_start = xs[len(free)] - bar_w
        for y in (0.5 + ceiling, 0.5 - ceiling):
            ax.plot([x_start, xs[-1] + bar_w], [y, y], color=CEILING, linewidth=(size_px or 1024) / 1024 * 14,
                    solid_capstyle="round")
    return fig


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    fig = draw(None, full=True)
    fig.savefig(OUT / "ada-icon.svg", transparent=True, metadata={"Date": None})
    plt.close(fig)
    for size in PNG_SIZES:
        fig = draw(size, full=size > SMALL)
        fig.savefig(OUT / f"ada-icon-{size}.png", dpi=100, transparent=True)
        plt.close(fig)
    images = [Image.open(OUT / f"ada-icon-{s}.png").convert("RGBA") for s in ICO_SIZES]
    images[-1].save(OUT / "ada-icon.ico", sizes=[(s, s) for s in ICO_SIZES], append_images=images[:-1])
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
