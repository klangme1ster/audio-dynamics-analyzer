"""Parallel batch scanner CLI `ada-scan` (plan 1.6)."""

from __future__ import annotations

import argparse
import csv
import json
import os
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

from rich.console import Console
from rich.progress import BarColumn, MofNCompleteColumn, Progress, TimeRemainingColumn
from rich.table import Table

from .decode import AUDIO_EXTENSIONS
from .engine import PSR_ZONES, analyze_file
from .visualizer import add_size_arguments

ZONE_STYLES = {"green": "green", "yellow": "yellow", "orange": "dark_orange", "red": "red"}


def find_audio(paths: list[Path]) -> list[Path]:
    found: set[Path] = set()
    for p in paths:
        if p.is_file():
            found.add(p.resolve())
        elif p.is_dir():
            found.update(f.resolve() for f in p.rglob("*")
                         if f.is_file() and f.suffix.lower() in AUDIO_EXTENSIONS)
    return sorted(found)


def scan_one(path: str, plot_dir: str | None, plot_format: str,
             plot_width: int = 2160, plot_aspect: float = 1.6) -> dict:
    """Worker: analyze one file, optionally plot it; returns only the summary (never audio)."""
    try:
        result = analyze_file(path)
        row = {"file": path, **result.summary()}
        if plot_dir:
            from .visualizer import plot_result
            out = Path(plot_dir) / f"{Path(path).stem}.{plot_format}"
            plot_result(result, Path(path).name, out, width_px=plot_width, aspect=plot_aspect)
            row["plot"] = str(out)
        return row
    except Exception as e:  # unreadable / unsupported files are reported, not fatal
        return {"file": path, "error": f"{type(e).__name__}: {e}"}


def _zone_style(psr: float | None) -> str:
    if psr is None:
        return "dim"
    return next(ZONE_STYLES[z] for z, lo, hi in PSR_ZONES if lo <= psr < hi)


def _num(v: float | None, style: str = "") -> str:
    if v is None:
        return "[dim]n/a[/]"
    return f"[{style}]{v:.1f}[/]" if style else f"{v:.1f}"


def render_table(rows: list[dict], root: Path | None) -> Table:
    table = Table(title="ADA dynamics scan", header_style="bold", show_lines=False)
    table.add_column("File", overflow="fold", min_width=16)
    for name in ("I (LUFS)", "LRA", "TP max", "PLR", "PSR min", "PSR mean", "PSR P10", "Ceil %"):
        table.add_column(name, justify="right")
    table.add_column("Zone (P10)")
    for r in rows:
        name = r["file"]
        if root is not None:
            try:
                name = str(Path(name).relative_to(root))
            except ValueError:
                pass
        if "error" in r:
            table.add_row(name, "[red]error[/]", *[""] * 8)
            continue
        tp = r["true_peak_max_dbtp"]
        tp_style = "red" if tp is not None and tp > 0.0 else ("yellow" if tp is not None and tp > -1.0 else "")
        p10 = r["psr_p10"]
        zone = next((z for z, lo, hi in PSR_ZONES if p10 is not None and lo <= p10 < hi), None)
        table.add_row(
            name, _num(r["integrated_lufs"]), _num(r["lra"]), _num(tp, tp_style), _num(r["plr"]),
            _num(r["psr_min"], _zone_style(r["psr_min"])),
            _num(r["psr_mean"], _zone_style(r["psr_mean"])),
            _num(p10, _zone_style(p10)),
            _num(r["peak_near_ceiling_pct"]),
            f"[{ZONE_STYLES[zone]}]{zone}[/]" if zone else "[dim]n/a[/]",
        )
    return table


def export_csv(rows: list[dict], path: Path) -> None:
    fields: list[str] = []
    for r in rows:
        fields += [k for k in r if k not in fields]
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for r in rows:
            w.writerow({k: ("" if v is None else v) for k, v in r.items()})


def export_json(rows: list[dict], path: Path) -> None:
    path.write_text(json.dumps(rows, indent=2, allow_nan=False), encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="ada-scan",
                                description="Batch loudness / true-peak / PSR / PLR scanner.")
    p.add_argument("paths", nargs="+", type=Path, help="audio files or directories (recursive)")
    p.add_argument("-j", "--jobs", type=int, default=os.cpu_count(), help="worker processes")
    p.add_argument("--export-csv", type=Path, metavar="FILE")
    p.add_argument("--export-json", type=Path, metavar="FILE")
    p.add_argument("--plot", type=Path, metavar="DIR", help="write a dynamics plot per file")
    p.add_argument("--plot-format", choices=("png", "pdf", "svg"), default="png")
    add_size_arguments(p, prefix="plot-")
    args = p.parse_args(argv)

    console = Console()
    files = find_audio(args.paths)
    if not files:
        console.print("[red]No audio files found.[/]")
        return 1
    if args.plot:
        args.plot.mkdir(parents=True, exist_ok=True)

    rows: list[dict] = []
    progress = Progress("[progress.description]{task.description}", BarColumn(),
                        MofNCompleteColumn(), TimeRemainingColumn(), console=console)
    with progress, ProcessPoolExecutor(max_workers=args.jobs) as pool:
        task = progress.add_task("Analyzing", total=len(files))
        futures = [pool.submit(scan_one, str(f), str(args.plot) if args.plot else None,
                               args.plot_format, args.plot_width, args.plot_aspect)
                   for f in files]
        for fut in as_completed(futures):
            rows.append(fut.result())
            progress.advance(task)
    rows.sort(key=lambda r: r["file"])

    root = args.paths[0].resolve() if len(args.paths) == 1 and args.paths[0].is_dir() else None
    console.print(render_table(rows, root))
    if args.export_csv:
        export_csv(rows, args.export_csv)
        console.print(f"CSV written to {args.export_csv}")
    if args.export_json:
        export_json(rows, args.export_json)
        console.print(f"JSON written to {args.export_json}")
    errors = [r for r in rows if "error" in r]
    for r in errors:
        console.print(f"[red]error[/] {r['file']}: {r['error']}", soft_wrap=True)
    if errors:
        console.print(f"[red]{len(errors)} file(s) could not be analyzed.[/]")
    return 1 if len(errors) == len(rows) else 0


if __name__ == "__main__":
    raise SystemExit(main())
