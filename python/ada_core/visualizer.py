"""Dynamics profile plots: S / TP(3 s) timeline, PSR with zones, PSR histogram (plan Phase 2)."""

from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

from .engine import PSR_ZONES, Result, analyze_file  # noqa: E402

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
PSR_RANGE = (0.0, 24.0)


def _style(ax) -> None:
    ax.set_facecolor(SURFACE)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(GRID)
    ax.tick_params(colors=TEXT_SECONDARY, labelsize=8, length=0)
    ax.grid(True, color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)


def _fmt(v, unit: str) -> str:
    return "n/a" if v is None or not np.isfinite(v) else f"{v:.1f} {unit}"


def plot_result(result: Result, title: str, out_path: str | Path | None = None):
    """Render the dynamics profile; save to out_path (format from suffix) if given."""
    t = result.times
    fig = plt.figure(figsize=(12, 6.5), facecolor=SURFACE)
    gs = fig.add_gridspec(2, 2, width_ratios=(5, 1.2), height_ratios=(1, 1),
                          hspace=0.3, wspace=0.05)
    ax_lvl = fig.add_subplot(gs[0, 0])
    ax_psr = fig.add_subplot(gs[1, 0], sharex=ax_lvl)
    ax_hist = fig.add_subplot(gs[1, 1], sharey=ax_psr)
    ax_info = fig.add_subplot(gs[0, 1])
    for ax in (ax_lvl, ax_psr, ax_hist):
        _style(ax)

    # Loudness and peak share one dB axis (both are decibels re full scale).
    with np.errstate(invalid="ignore"):
        st = np.where(np.isfinite(result.short_term), result.short_term, np.nan)
        tp = np.where(np.isfinite(result.true_peak_3s), result.true_peak_3s, np.nan)
    ax_lvl.plot(t, tp, color=SERIES_TRUE_PEAK, linewidth=2, label="True peak, 3 s window (dBTP)")
    ax_lvl.plot(t, st, color=SERIES_SHORT_TERM, linewidth=2, label="Short-term loudness (LUFS)")
    finite = np.concatenate([st[np.isfinite(st)], tp[np.isfinite(tp)]])
    if finite.size:
        ax_lvl.set_ylim(max(finite.min() - 3, -70), min(finite.max() + 3, 6))
    ax_lvl.set_ylabel("dB", color=TEXT_SECONDARY, fontsize=9)
    ax_lvl.legend(loc="lower left", fontsize=8, frameon=False, labelcolor=TEXT_PRIMARY)
    ax_lvl.set_title("Loudness and peak", loc="left", fontsize=10, color=TEXT_PRIMARY)
    plt.setp(ax_lvl.get_xticklabels(), visible=False)

    # PSR over zone bands; gated bins are NaN and show as gaps.
    x_end = t[-1] if len(t) else 1.0
    for zone, lo, hi in PSR_ZONES:
        lo_c, hi_c = max(lo, PSR_RANGE[0]), min(hi, PSR_RANGE[1])
        ax_psr.axhspan(lo_c, hi_c, color=ZONE_COLORS[zone], alpha=0.12, linewidth=0)
        ax_psr.text(x_end, (lo_c + hi_c) / 2, ZONE_LABELS[zone] + " ", ha="right",
                    va="center", fontsize=7, color=TEXT_SECONDARY)
    ax_psr.plot(t, result.psr, color=PSR_LINE, linewidth=1.5)
    ax_psr.set_ylim(*PSR_RANGE)
    ax_psr.set_xlim(0, x_end)
    ax_psr.set_ylabel("PSR (dB)", color=TEXT_SECONDARY, fontsize=9)
    ax_psr.set_xlabel("Time (s)", color=TEXT_SECONDARY, fontsize=9)
    ax_psr.set_title("Peak-to-short-term ratio", loc="left", fontsize=10, color=TEXT_PRIMARY)

    # Histogram: share of gated duration per 1 dB PSR bin.
    psr = result.psr[np.isfinite(result.psr)]
    edges = np.arange(PSR_RANGE[0], PSR_RANGE[1] + 1, 1.0)
    if psr.size:
        counts, _ = np.histogram(np.clip(psr, PSR_RANGE[0], PSR_RANGE[1] - 1e-9), edges)
        pct = counts / psr.size * 100
        for lo_edge, p in zip(edges[:-1], pct):
            zone = next(z for z, lo, hi in PSR_ZONES if lo <= lo_edge < hi)
            ax_hist.barh(lo_edge + 0.5, p, height=0.9, color=ZONE_COLORS[zone], linewidth=0)
    ax_hist.set_xlabel("% of duration", color=TEXT_SECONDARY, fontsize=9)
    plt.setp(ax_hist.get_yticklabels(), visible=False)
    ax_hist.set_title("Distribution", loc="left", fontsize=10, color=TEXT_PRIMARY)

    # Summary block.
    ax_info.axis("off")
    rows = [
        ("Integrated", _fmt(result.integrated, "LUFS")),
        ("LRA", _fmt(result.lra, "LU")),
        ("True peak max", _fmt(result.true_peak_max, "dBTP")),
        ("PLR", _fmt(result.plr, "dB")),
        ("PSR min / P10", f"{_fmt(result.psr_min, '')}/ {_fmt(result.psr_p10, 'dB')}"),
        ("PSR mean", _fmt(result.psr_mean, "dB")),
    ]
    for i, (label, value) in enumerate(rows):
        y = 0.95 - i * 0.16
        ax_info.text(0.08, y, label, fontsize=8, color=TEXT_SECONDARY, transform=ax_info.transAxes)
        ax_info.text(0.08, y - 0.07, value, fontsize=11, color=TEXT_PRIMARY,
                     transform=ax_info.transAxes)

    fig.suptitle(title, x=0.06, ha="left", fontsize=12, color=TEXT_PRIMARY)
    if out_path is not None:
        fig.savefig(out_path, facecolor=SURFACE, dpi=150, bbox_inches="tight")
        plt.close(fig)
    return fig


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="ada-plot", description="Plot dynamics profiles.")
    p.add_argument("files", nargs="+", type=Path)
    p.add_argument("-o", "--out-dir", type=Path, default=Path("."))
    p.add_argument("--format", choices=("png", "pdf", "svg"), default="png")
    args = p.parse_args(argv)
    args.out_dir.mkdir(parents=True, exist_ok=True)
    for f in args.files:
        out = args.out_dir / f"{f.stem}.{args.format}"
        plot_result(analyze_file(f), f.name, out)
        print(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
