"""Dynamics profile plots (plan Phase 2).

Panels: loudness and peak timeline, PSR timeline with zones, PSR distribution,
loudness-vs-PSR density, and peak levels relative to the file's maximum true peak.
"""

from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from matplotlib.colors import LinearSegmentedColormap  # noqa: E402
from matplotlib.ticker import FuncFormatter  # noqa: E402

from .engine import CEILING_WINDOW_DB, PSR_ZONES, Result, analyze_file  # noqa: E402

DEFAULT_WIDTH_PX = 2160
DEFAULT_ASPECT = 16 / 10
FIGURE_WIDTH_IN = 14.4  # fixed layout size; pixel width only changes resolution

SURFACE = "#fcfcfb"
TEXT_PRIMARY = "#0b0b0b"
TEXT_SECONDARY = "#52514e"
GRID = "#e4e3df"
SERIES_SHORT_TERM = "#2a78d6"   # categorical slot 1
SERIES_TRUE_PEAK = "#eb6834"    # categorical slot 2
PSR_LINE = "#0b0b0b"
ZONE_COLORS = {"green": "#0ca30c", "yellow": "#fab219", "orange": "#ec835a", "red": "#d03b3b"}
ZONE_LABELS = {"green": "Open ≥ 12", "yellow": "Controlled 10–12",
               "orange": "Heavy 8–10", "red": "Over-limited < 8"}
DENSITY_CMAP = LinearSegmentedColormap.from_list(
    "ada_blue", ["#cde2fb", "#86b6ef", "#3987e5", "#256abf", "#104281", "#0d366b"])
PSR_RANGE = (0.0, 24.0)
PEAK_RANGE_DB = 12.0
LINE_WIDTH = 1.0
PSR_LINE_WIDTH = 0.8
LABEL_SIZE = 9
TITLE_SIZE = 10


def parse_aspect(text: str) -> float:
    """'16:10' -> 1.6, '1.5' -> 1.5 (width / height)."""
    try:
        if ":" in text:
            w, h = text.split(":", 1)
            value = float(w) / float(h)
        else:
            value = float(text)
    except (ValueError, ZeroDivisionError):
        raise ValueError(f"invalid aspect ratio {text!r}; use W:H such as 16:10") from None
    if not 0.2 <= value <= 5:
        raise ValueError(f"aspect ratio {text!r} out of range")
    return value


def _style(ax) -> None:
    ax.set_facecolor(SURFACE)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(GRID)
    ax.tick_params(colors=TEXT_SECONDARY, labelsize=8, length=0)
    ax.grid(True, color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)


def _labels(ax, title: str, xlabel: str | None, ylabel: str | None) -> None:
    ax.set_title(title, loc="left", fontsize=TITLE_SIZE, color=TEXT_PRIMARY)
    if xlabel:
        ax.set_xlabel(xlabel, color=TEXT_SECONDARY, fontsize=LABEL_SIZE)
    if ylabel:
        ax.set_ylabel(ylabel, color=TEXT_SECONDARY, fontsize=LABEL_SIZE)


def _empty(ax, text: str = "No measurable material (silence or < 3 s)") -> None:
    ax.text(0.5, 0.5, text, ha="center", va="center", fontsize=LABEL_SIZE,
            color=TEXT_SECONDARY, transform=ax.transAxes)


def _fmt(v, unit: str) -> str:
    return "n/a" if v is None or not np.isfinite(v) else f"{v:.1f} {unit}".rstrip()


def _mmss(seconds, _pos=None) -> str:
    seconds = max(0.0, seconds)
    return f"{int(seconds // 60)}:{int(seconds % 60):02d}"


def _zone_of(psr_value: float) -> str:
    return next(z for z, lo, hi in PSR_ZONES if lo <= psr_value < hi)


def plot_result(result: Result, title: str, out_path: str | Path | None = None,
                width_px: int = DEFAULT_WIDTH_PX, aspect: float = DEFAULT_ASPECT):
    """Render the dynamics profile; save to out_path (format from its suffix) if given.

    Raster output is exactly width_px × width_px / aspect pixels.
    """
    t = result.times
    measured = np.isfinite(result.psr)  # bins that pass the PSR statistics gate
    fig = plt.figure(figsize=(FIGURE_WIDTH_IN, FIGURE_WIDTH_IN / aspect),
                     facecolor=SURFACE, layout="constrained")
    gs = fig.add_gridspec(3, 2, width_ratios=(4.2, 1), height_ratios=(1, 1, 1.25))
    ax_lvl = fig.add_subplot(gs[0, 0])
    ax_info = fig.add_subplot(gs[0, 1])
    ax_psr = fig.add_subplot(gs[1, 0], sharex=ax_lvl)
    ax_hist = fig.add_subplot(gs[1, 1], sharey=ax_psr)
    bottom = gs[2, :].subgridspec(1, 2, width_ratios=(1.5, 1))
    ax_dens = fig.add_subplot(bottom[0, 0])
    ax_ceil = fig.add_subplot(bottom[0, 1])
    for ax in (ax_lvl, ax_psr, ax_hist, ax_dens, ax_ceil):
        _style(ax)

    x_end = t[-1] if len(t) else 1.0
    time_fmt = FuncFormatter(_mmss)

    # 1. Loudness and peak timeline: both in dB re full scale, one shared axis.
    with np.errstate(invalid="ignore"):
        st = np.where(np.isfinite(result.short_term), result.short_term, np.nan)
        tp = np.where(np.isfinite(result.true_peak_3s), result.true_peak_3s, np.nan)
    ax_lvl.plot(t, tp, color=SERIES_TRUE_PEAK, linewidth=LINE_WIDTH,
                label="True peak, max over 3 s (dBTP)")
    ax_lvl.plot(t, st, color=SERIES_SHORT_TERM, linewidth=LINE_WIDTH,
                label="Short-term loudness, 3 s (LUFS)")
    finite = np.concatenate([st[np.isfinite(st)], tp[np.isfinite(tp)]])
    if finite.size:
        ax_lvl.set_ylim(max(finite.min() - 3, -70), min(finite.max() + 3, 6))
    ax_lvl.legend(loc="lower left", fontsize=8, frameon=False, labelcolor=TEXT_PRIMARY)
    ax_lvl.xaxis.set_major_formatter(time_fmt)
    _labels(ax_lvl, "Loudness and peak over time", None, "Level (LUFS / dBTP)")
    plt.setp(ax_lvl.get_xticklabels(), visible=False)

    # 2. PSR timeline over labelled zone bands; unmeasured bins are gaps.
    for zone, lo, hi in PSR_ZONES:
        lo_c, hi_c = max(lo, PSR_RANGE[0]), min(hi, PSR_RANGE[1])
        ax_psr.axhspan(lo_c, hi_c, color=ZONE_COLORS[zone], alpha=0.12, linewidth=0)
        ax_psr.text(x_end, (lo_c + hi_c) / 2, ZONE_LABELS[zone], ha="right", va="center",
                    fontsize=7, color=TEXT_SECONDARY, zorder=5,
                    bbox=dict(facecolor=SURFACE, edgecolor="none", alpha=0.85, pad=1.5))
    ax_psr.plot(t, result.psr, color=PSR_LINE, linewidth=PSR_LINE_WIDTH)
    if result.psr_p10 is not None:
        p10 = float(np.clip(result.psr_p10, *PSR_RANGE))
        for ax in (ax_psr, ax_hist):
            ax.axhline(p10, color=TEXT_PRIMARY, linewidth=0.8, linestyle=(0, (5, 3)))
        ax_psr.text(0.005, p10, f" PSR P10 = {result.psr_p10:.1f} dB", ha="left", va="bottom",
                    fontsize=8, color=TEXT_PRIMARY, transform=ax_psr.get_yaxis_transform())
    ax_psr.set_ylim(*PSR_RANGE)
    ax_psr.set_xlim(0, x_end)
    ax_psr.xaxis.set_major_formatter(time_fmt)
    _labels(ax_psr, "Peak-to-short-term ratio (PSR) over time", "Time (min:s)", "PSR (dB)")

    # 3. PSR distribution, sharing the PSR axis.
    psr = result.psr[measured]
    edges = np.arange(PSR_RANGE[0], PSR_RANGE[1] + 1, 1.0)
    if psr.size:
        counts, _ = np.histogram(np.clip(psr, PSR_RANGE[0], PSR_RANGE[1] - 1e-9), edges)
        pct = counts / psr.size * 100
        for lo_edge, p in zip(edges[:-1], pct):
            ax_hist.barh(lo_edge + 0.5, p, height=0.85, color=ZONE_COLORS[_zone_of(lo_edge)],
                         linewidth=0)
    plt.setp(ax_hist.get_yticklabels(), visible=False)
    _labels(ax_hist, "PSR distribution", "Share of duration (%)", None)

    # 4. Loudness vs PSR density: where in the loudness range the material is squashed.
    if psr.size:
        loud = result.short_term[measured]
        x_lo, x_hi = np.floor(loud.min()) - 1, np.ceil(loud.max()) + 1
        x_edges = np.arange(x_lo, x_hi + 0.5, 0.5)
        y_edges = np.arange(PSR_RANGE[0], PSR_RANGE[1] + 0.5, 0.5)
        h, _, _ = np.histogram2d(loud, np.clip(psr, PSR_RANGE[0], PSR_RANGE[1] - 1e-9),
                                 bins=(x_edges, y_edges))
        h = np.ma.masked_equal(h.T / psr.size * 100, 0)
        mesh = ax_dens.pcolormesh(x_edges, y_edges, h, cmap=DENSITY_CMAP, shading="flat")
        cbar = fig.colorbar(mesh, ax=ax_dens, pad=0.01, fraction=0.04)
        cbar.set_label("Share of duration (%)", color=TEXT_SECONDARY, fontsize=LABEL_SIZE)
        cbar.ax.tick_params(colors=TEXT_SECONDARY, labelsize=8, length=0)
        cbar.outline.set_visible(False)
        for boundary in (8.0, 10.0, 12.0):
            ax_dens.axhline(boundary, color=TEXT_SECONDARY, linewidth=0.6, linestyle=(0, (4, 3)))
        for zone, lo, hi in PSR_ZONES:
            lo_c, hi_c = max(lo, PSR_RANGE[0]), min(hi, PSR_RANGE[1])
            ax_dens.text(x_hi, (lo_c + hi_c) / 2, ZONE_LABELS[zone] + " ", ha="right",
                         va="center", fontsize=7, color=TEXT_SECONDARY)
        ax_dens.set_xlim(x_lo, x_hi)
    else:
        _empty(ax_dens)
    ax_dens.set_ylim(*PSR_RANGE)
    _labels(ax_dens, "Loudness vs. PSR — time spent at each combination",
            "Short-term loudness (LUFS)", "PSR (dB)")

    # 5. Peak levels relative to the file's max true peak: limiting stacks peaks at the ceiling.
    tp_max_lin = 10.0 ** (result.true_peak_max / 20.0) if np.isfinite(result.true_peak_max) else 0.0
    if psr.size and tp_max_lin > 0:
        with np.errstate(divide="ignore"):
            rel = 20.0 * np.log10(result.bin_peak[measured] / tp_max_lin)
        rel = np.clip(rel, -PEAK_RANGE_DB, 0.0)
        step = 0.25
        edges = np.arange(-PEAK_RANGE_DB, step / 2, step)
        counts, _ = np.histogram(rel, np.append(edges[:-1], 1e-9))
        pct = counts / rel.size * 100
        ax_ceil.axvspan(-CEILING_WINDOW_DB, 0.0, color=GRID, alpha=0.8, linewidth=0)
        ax_ceil.bar(edges[:-1] + step / 2, pct, width=step * 0.85, color=SERIES_SHORT_TERM,
                    linewidth=0)
        ax_ceil.set_xlim(-PEAK_RANGE_DB, 0.0)
        ax_ceil.xaxis.set_major_formatter(
            FuncFormatter(lambda v, _p: ("≤ " if v <= -PEAK_RANGE_DB else "")
                          + f"{v:g}".replace("-", "−")))
    else:
        _empty(ax_ceil)
    near_text = ("" if result.peak_near_ceiling_pct is None else
                 f": {result.peak_near_ceiling_pct:.1f} % within {CEILING_WINDOW_DB:g} dB (shaded)")
    _labels(ax_ceil, "Peaks relative to the ceiling" + near_text,
            "Peak level below max true peak (dB)", "Share of duration (%)")

    # Summary block.
    ax_info.axis("off")
    rows = [
        ("Integrated loudness", _fmt(result.integrated, "LUFS")),
        ("Loudness range (LRA)", _fmt(result.lra, "LU")),
        ("Max true peak", _fmt(result.true_peak_max, "dBTP")),
        ("PLR", _fmt(result.plr, "dB")),
        ("PSR min / P10 / mean",
         f"{_fmt(result.psr_min, '')} / {_fmt(result.psr_p10, '')} / {_fmt(result.psr_mean, 'dB')}"),
        (f"Peaks within {CEILING_WINDOW_DB:g} dB of max", _fmt(result.peak_near_ceiling_pct, "%")),
    ]
    for i, (label, value) in enumerate(rows):
        y = 0.97 - i * 0.165
        ax_info.text(0.06, y, label, fontsize=8, color=TEXT_SECONDARY, transform=ax_info.transAxes)
        ax_info.text(0.06, y - 0.075, value, fontsize=11, color=TEXT_PRIMARY,
                     transform=ax_info.transAxes)

    fig.suptitle(title, x=0.01, ha="left", fontsize=12, color=TEXT_PRIMARY)
    if out_path is not None:
        fig.savefig(out_path, facecolor=SURFACE, dpi=width_px / FIGURE_WIDTH_IN)
        plt.close(fig)
    return fig


def add_size_arguments(p: argparse.ArgumentParser, prefix: str = "") -> None:
    p.add_argument(f"--{prefix}width", type=int, default=DEFAULT_WIDTH_PX, metavar="PX",
                   help=f"plot width in pixels (default {DEFAULT_WIDTH_PX})")
    p.add_argument(f"--{prefix}aspect", type=parse_aspect, default=DEFAULT_ASPECT, metavar="W:H",
                   help="plot aspect ratio (default 16:10)")


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="ada-plot", description="Plot dynamics profiles.")
    p.add_argument("files", nargs="+", type=Path)
    p.add_argument("-o", "--out-dir", type=Path, default=Path("."))
    p.add_argument("--format", choices=("png", "pdf", "svg"), default="png")
    add_size_arguments(p)
    args = p.parse_args(argv)
    args.out_dir.mkdir(parents=True, exist_ok=True)
    for f in args.files:
        out = args.out_dir / f"{f.stem}.{args.format}"
        plot_result(analyze_file(f), f.name, out, width_px=args.width, aspect=args.aspect)
        print(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
