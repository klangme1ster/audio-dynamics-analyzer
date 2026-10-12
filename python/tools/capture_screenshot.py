"""Capture the standalone app's window for the README (Windows only).

Starts the built standalone app with an audio file, sizes the window, waits for the analysis
and saves the window as PNG. The app is then terminated, so its saved settings (window size,
view, delivery target) are left as they were.

    python python/tools/capture_screenshot.py <audio file> docs/screenshots/file-view.png
        [--size 1600x1000] [--wait 8] [--exe <path to Audio Dynamics Analyzer.exe>]

--size is in physical pixels (the capture's size); the app's own scaling follows Windows'.
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[2]
DEFAULT_EXE = (REPO / "build/src/plugin/AudioDynamicsAnalyzer_artefacts/Release/Standalone"
               / "Audio Dynamics Analyzer.exe")
PW_RENDERFULLCONTENT = 2


class BitmapInfoHeader(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
                ("biPlanes", wt.WORD), ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG),
                ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD)]


def find_window(pid: int, timeout: float = 15.0) -> int:
    user32 = ctypes.windll.user32
    found: list[int] = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def visit(hwnd, _):
        owner = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        user32.EnumWindows(visit, 0)
        if found:
            return found[0]
        time.sleep(0.2)
    raise RuntimeError("the app's window did not appear")


def capture(hwnd: int) -> np.ndarray:
    user32, gdi32 = ctypes.windll.user32, ctypes.windll.gdi32
    rect = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    w, h = rect.right - rect.left, rect.bottom - rect.top
    window_dc = user32.GetDC(hwnd)
    memory_dc = gdi32.CreateCompatibleDC(window_dc)
    bitmap = gdi32.CreateCompatibleBitmap(window_dc, w, h)
    gdi32.SelectObject(memory_dc, bitmap)
    try:
        user32.PrintWindow(hwnd, memory_dc, PW_RENDERFULLCONTENT)
        header = BitmapInfoHeader(ctypes.sizeof(BitmapInfoHeader), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
        pixels = (ctypes.c_ubyte * (w * h * 4))()
        gdi32.GetDIBits(memory_dc, bitmap, 0, h, pixels, ctypes.byref(header), 0)
    finally:
        gdi32.DeleteObject(bitmap)
        gdi32.DeleteDC(memory_dc)
        user32.ReleaseDC(hwnd, window_dc)
    return np.frombuffer(pixels, np.uint8).reshape(h, w, 4)[:, :, [2, 1, 0]].copy()  # BGRA -> RGB


def main(argv: list[str] | None = None) -> int:
    if sys.platform != "win32":
        print("capture_screenshot.py needs Windows", file=sys.stderr)
        return 2
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("audio", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--size", default="1600x1000", help="window size in physical pixels, WxH")
    ap.add_argument("--wait", type=float, default=8.0, help="seconds to wait for the analysis")
    ap.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    args = ap.parse_args(argv)
    width, height = (int(v) for v in args.size.lower().split("x"))

    ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))  # per-monitor v2: physical pixels
    app = subprocess.Popen([str(args.exe), str(args.audio)])
    try:
        hwnd = find_window(app.pid)
        time.sleep(1.0)
        SWP_NOMOVE, SWP_NOZORDER = 0x0002, 0x0004
        ctypes.windll.user32.SetWindowPos(hwnd, 0, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER)
        time.sleep(args.wait)
        image = capture(hwnd)
    finally:
        app.kill()  # no clean shutdown: the app's saved settings stay untouched

    import matplotlib.image

    args.out.parent.mkdir(parents=True, exist_ok=True)
    matplotlib.image.imsave(args.out, image)
    print(f"wrote {args.out} ({image.shape[1]}x{image.shape[0]})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
