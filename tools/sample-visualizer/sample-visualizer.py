#!/usr/bin/env python3
"""Quick viewer for raw binary sample files of unknown format.

Reinterpret the same bytes as 8/16-bit, signed/unsigned, little/big-endian
on the fly and pan/zoom with the mouse to see which one looks right.

Requires: pip install pyqtgraph PyQt6 numpy
(or run scripts/setup-venv.sh once to create a sibling .venv)
"""

import os
import sys

# If a sibling .venv exists and we're not already inside it, re-exec through it.
# (Venv pythons symlink to the base interpreter, so compare sys.prefix, not the binary.)
_here = os.path.dirname(os.path.abspath(__file__))
_venv = os.path.join(_here, ".venv")
_venv_py = os.path.join(_venv, "bin", "python")
if os.path.exists(_venv_py) and os.path.realpath(sys.prefix) != os.path.realpath(_venv):
    os.execv(_venv_py, [_venv_py, __file__, *sys.argv[1:]])

import argparse

import numpy as np
import pyqtgraph as pg
from PyQt6 import QtWidgets, QtCore


class Viewer(QtWidgets.QMainWindow):
    def __init__(self, path: str):
        super().__init__()
        self.path = path
        self.filesize = os.path.getsize(path)
        # memmap raw bytes — we'll reinterpret as needed
        self.raw = np.memmap(path, dtype=np.uint8, mode="r")

        self.setWindowTitle(f"sample-visualizer — {os.path.basename(path)} ({self.filesize:,} bytes)")
        self.resize(1200, 600)

        central = QtWidgets.QWidget()
        self.setCentralWidget(central)
        layout = QtWidgets.QVBoxLayout(central)

        # --- Controls ---------------------------------------------------
        controls = QtWidgets.QHBoxLayout()
        layout.addLayout(controls)

        self.bits_group = QtWidgets.QButtonGroup(self)
        self.r8 = QtWidgets.QRadioButton("8-bit")
        self.r16 = QtWidgets.QRadioButton("16-bit")
        self.r16.setChecked(True)
        self.bits_group.addButton(self.r8)
        self.bits_group.addButton(self.r16)
        controls.addWidget(self.r8)
        controls.addWidget(self.r16)

        controls.addSpacing(12)
        self.signed = QtWidgets.QCheckBox("signed")
        self.signed.setChecked(True)
        controls.addWidget(self.signed)

        self.big_endian = QtWidgets.QCheckBox("big-endian")
        controls.addWidget(self.big_endian)

        controls.addSpacing(20)
        controls.addWidget(QtWidgets.QLabel("Y:"))
        self.auto_y = QtWidgets.QCheckBox("auto")
        self.auto_y.setChecked(True)
        controls.addWidget(self.auto_y)

        controls.addWidget(QtWidgets.QLabel("min"))
        self.ymin = QtWidgets.QLineEdit("-32768")
        self.ymin.setMaximumWidth(80)
        controls.addWidget(self.ymin)
        controls.addWidget(QtWidgets.QLabel("max"))
        self.ymax = QtWidgets.QLineEdit("32767")
        self.ymax.setMaximumWidth(80)
        controls.addWidget(self.ymax)

        controls.addStretch(1)

        # --- Plot -------------------------------------------------------
        self.plot = pg.PlotWidget()
        self.plot.setBackground("w")
        self.plot.showGrid(x=True, y=True, alpha=0.3)
        self.plot.setLabel("bottom", "sample index")
        # connect='pairs' draws each consecutive (i, i+1) pair as an isolated segment,
        # so for min/max envelope we emit (x, min), (x, max) per bucket and get a clean
        # vertical bar with no inter-bucket drop lines (which otherwise alias visually
        # at extreme zoom-out and produce a fake every-other-column pattern).
        self.curve = self.plot.plot([], [], pen=pg.mkPen("b", width=1), connect="pairs")
        layout.addWidget(self.plot, 1)

        # Status bar
        self.status = self.statusBar()

        # --- Signals ----------------------------------------------------
        for w in (self.r8, self.r16, self.signed, self.big_endian):
            w.toggled.connect(self.refresh)
        self.auto_y.toggled.connect(self.on_auto_y_toggled)
        self.ymin.editingFinished.connect(self.refresh)
        self.ymax.editingFinished.connect(self.refresh)
        self.plot.sigRangeChanged.connect(self.on_range_changed)
        # Coalesced redraw on resize
        self._resize_timer = QtCore.QTimer(self)
        self._resize_timer.setSingleShot(True)
        self._resize_timer.setInterval(60)
        self._resize_timer.timeout.connect(self.redraw)
        self.plot.getViewBox().sigResized.connect(lambda *_: self._resize_timer.start())

        # Initial view shows whole file (downsampled)
        self.full_view = True
        self.refresh()

    # ------------------------------------------------------------------
    def current_dtype(self):
        """Return numpy dtype for current settings, plus the bytes-per-sample."""
        bits = 8 if self.r8.isChecked() else 16
        signed = self.signed.isChecked()
        be = self.big_endian.isChecked()
        if bits == 8:
            return (np.dtype("i1") if signed else np.dtype("u1")), 1
        endian = ">" if be else "<"
        kind = "i" if signed else "u"
        return np.dtype(f"{endian}{kind}2"), 2

    def total_samples(self):
        _, bps = self.current_dtype()
        return self.filesize // bps

    def get_samples(self, start: int, count: int):
        """Read `count` samples starting at sample-index `start`, with current interpretation."""
        dtype, bps = self.current_dtype()
        n = self.total_samples()
        start = max(0, min(start, n))
        end = max(start, min(start + count, n))
        if end <= start:
            return np.empty(0, dtype=np.float32), start
        offset = start * bps
        nbytes = (end - start) * bps
        # Slice memmap bytes and view as target dtype (handles endianness)
        buf = self.raw[offset:offset + nbytes]
        arr = np.frombuffer(buf, dtype=dtype)
        return arr, start

    # ------------------------------------------------------------------
    def refresh(self):
        """Format/y-range changed: keep the same byte window visible, update limits."""
        # Capture the currently-visible byte range using the *previous* bytes-per-sample.
        prev_bps = getattr(self, "_last_bps", None)
        if prev_bps is not None:
            (vx0, vx1), _ = self.plot.viewRange()
            byte0 = max(0.0, vx0) * prev_bps
            byte1 = max(byte0 + prev_bps, vx1 * prev_bps)
        else:
            byte0, byte1 = 0.0, float(self.filesize)

        dtype, bps = self.current_dtype()
        n = self.total_samples()

        # Y-range fields are only meaningful when auto-Y is off.
        self.ymin.setEnabled(not self.auto_y.isChecked())
        self.ymax.setEnabled(not self.auto_y.isChecked())

        # Zoom limits: min span 100 samples, max span = full file.
        vb = self.plot.getViewBox()
        vb.setLimits(xMin=0, xMax=max(1, n), minXRange=100, maxXRange=max(100, n))

        # Re-apply the byte window in the new dtype's sample units.
        new_x0 = byte0 / bps
        new_x1 = byte1 / bps
        self.plot.blockSignals(True)
        self.plot.setXRange(new_x0, new_x1, padding=0)
        self.plot.blockSignals(False)

        self._last_bps = bps
        self.full_view = (new_x0 <= 0 and new_x1 >= n - 1)
        self.redraw()

    def on_range_changed(self, *_):
        self.full_view = False
        self.redraw()

    def on_auto_y_toggled(self, checked: bool):
        """When the user turns auto-Y off, lock the current visible y-range into the fields."""
        if not checked:
            (_, (y0, y1)) = self.plot.viewRange()
            self.ymin.setText(f"{y0:g}")
            self.ymax.setText(f"{y1:g}")
        self.refresh()

    def redraw(self):
        n = self.total_samples()
        if n == 0:
            self.curve.setData([], [])
            return

        if self.full_view:
            x0, x1 = 0, n
        else:
            (x0f, x1f), _ = self.plot.viewRange()
            x0 = int(max(0, np.floor(x0f)))
            x1 = int(min(n, np.ceil(x1f)))
            if x1 <= x0:
                x1 = min(n, x0 + 1)

        span = x1 - x0

        # One min/max pair per pixel column — no point producing more than the screen shows.
        px_width = max(64, int(self.plot.getViewBox().width()))
        max_read = 2_000_000  # hard cap on samples touched per redraw

        connect_mode = "all"  # raw branch keeps a connected polyline; reduce paths override.
        if span <= 2 * px_width:
            # Few enough samples to draw raw.
            samples, base = self.get_samples(x0, span)
            xs = np.arange(base, base + len(samples), dtype=np.int64)
            ys = samples.astype(np.float32, copy=False)
        elif span <= max_read:
            # Float-precise bucket boundaries via np.*.reduceat — every sample lands in
            # exactly one bucket, with widths alternating between floor(N/B) and ceil(N/B)
            # to cover N samples in B buckets exactly. Avoids the integer-rounding loss
            # of the reshape approach (which becomes very visible at small bucket sizes).
            samples, base = self.get_samples(x0, span)
            n_samples = len(samples)
            if n_samples == 0:
                self.curve.setData([], [])
                return
            buckets = min(px_width, n_samples)
            # bounds[i] = floor(i * n_samples / buckets); guaranteed monotonic and
            # strictly increasing when buckets <= n_samples.
            idx = np.arange(buckets + 1, dtype=np.int64)
            bounds = (idx * n_samples) // buckets
            starts = bounds[:-1]
            centers = base + (bounds[:-1] + bounds[1:]) // 2

            mins = np.minimum.reduceat(samples, starts)
            maxs = np.maximum.reduceat(samples, starts)
            ys = np.empty(buckets * 2, dtype=np.float32)
            ys[0::2] = mins
            ys[1::2] = maxs
            xs = np.empty(buckets * 2, dtype=np.int64)
            xs[0::2] = centers
            xs[1::2] = centers
            connect_mode = "pairs"
        else:
            # Span exceeds the read cap (very zoomed out). Bucket size is huge here, so
            # the integer-rounding error is negligible. Use the reshape + random in-bucket
            # subsample path with file-aligned bucket starts (so panning by < 1 pixel
            # doesn't shimmer the moiré).
            bucket_size = max(1, span // px_width)
            read_start = (x0 // bucket_size) * bucket_size
            read_end = ((x1 + bucket_size - 1) // bucket_size) * bucket_size
            read_end = min(read_end, n)
            count = read_end - read_start
            buckets_full = count // bucket_size
            inner = max(2, max_read // max(1, buckets_full))

            samples, base = self.get_samples(read_start, count)
            usable = (len(samples) // bucket_size) * bucket_size
            buckets = usable // bucket_size if bucket_size else 0
            samples = samples[:usable]
            if buckets == 0:
                self.curve.setData([], [])
                return

            view = samples.reshape(buckets, bucket_size)
            if inner < bucket_size:
                # Random subset of in-bucket positions, shared across all buckets.
                # Fixed seed → stable picture as you pan (no flicker).
                rng = np.random.default_rng(0xC0FFEE)
                offsets = np.sort(rng.choice(bucket_size, size=inner, replace=False))
                view = view[:, offsets]

            xs_base = base + np.arange(buckets) * bucket_size
            mins = view.min(axis=1)
            maxs = view.max(axis=1)
            ys = np.empty(buckets * 2, dtype=np.float32)
            ys[0::2] = mins
            ys[1::2] = maxs
            centers = xs_base + bucket_size // 2
            xs = np.empty(buckets * 2, dtype=np.int64)
            xs[0::2] = centers
            xs[1::2] = centers
            connect_mode = "pairs"

        self.curve.setData(xs, ys, connect=connect_mode)

        # Y-range
        if self.auto_y.isChecked():
            self.plot.enableAutoRange(axis="y", enable=True)
        else:
            try:
                lo = float(self.ymin.text())
                hi = float(self.ymax.text())
                if hi > lo:
                    self.plot.setYRange(lo, hi, padding=0)
            except ValueError:
                pass

        dtype, bps = self.current_dtype()
        self.status.showMessage(
            f"dtype={dtype.str}  bps={bps}  samples={n:,}  view=[{x0:,}..{x1:,}]  "
            f"span={span:,}  px={px_width}  drawn={len(xs):,}"
        )


def main():
    ap = argparse.ArgumentParser(description="Interactive viewer for raw binary sample files.")
    ap.add_argument("file", help="path to raw binary file")
    args = ap.parse_args()

    app = QtWidgets.QApplication(sys.argv)
    pg.setConfigOptions(useOpenGL=False, antialias=False)
    w = Viewer(args.file)
    w.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
