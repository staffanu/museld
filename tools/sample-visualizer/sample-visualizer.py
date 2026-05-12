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
from PyQt6 import QtWidgets, QtCore, QtGui


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
        self.rf32 = QtWidgets.QRadioButton("float32")
        self.r16.setChecked(True)
        self.bits_group.addButton(self.r8)
        self.bits_group.addButton(self.r16)
        self.bits_group.addButton(self.rf32)
        controls.addWidget(self.r8)
        controls.addWidget(self.r16)
        controls.addWidget(self.rf32)

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

        controls.addSpacing(20)
        self.select_region = QtWidgets.QCheckBox("select region")
        controls.addWidget(self.select_region)
        self.zoom_btn = QtWidgets.QPushButton("Zoom to selection")
        self.zoom_btn.setEnabled(False)
        controls.addWidget(self.zoom_btn)

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
        # Hidden until "select region" is checked. User drags edges/middle to set the
        # x-range, then clicks "Zoom to selection" to commit a single setXRange (one
        # redraw instead of many during wheel-scroll zoom).
        self.region = pg.LinearRegionItem(brush=(80, 120, 200, 40))
        self.region.setZValue(10)
        self.region.hide()
        self.plot.addItem(self.region)
        layout.addWidget(self.plot, 1)

        # Status bar
        self.status = self.statusBar()

        # --- Signals ----------------------------------------------------
        for w in (self.r8, self.r16, self.rf32, self.signed, self.big_endian):
            w.toggled.connect(self.refresh)
        self.bits_group.buttonToggled.connect(self.on_bits_changed)
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
        self.select_region.toggled.connect(self.on_select_region_toggled)
        self.zoom_btn.clicked.connect(self.on_zoom_clicked)
        self.plot.scene().sigMouseMoved.connect(self.on_mouse_moved)

        # Quit shortcuts (Q / Esc)
        for key in (QtCore.Qt.Key.Key_Q, QtCore.Qt.Key.Key_Escape):
            sc = QtGui.QShortcut(QtGui.QKeySequence(key), self)
            sc.activated.connect(self.close)

        # Last computed status (without cursor info) so we can append cursor readout.
        self._status_base = ""

        # Initial view shows whole file (downsampled)
        self.full_view = True
        self.refresh()

    # ------------------------------------------------------------------
    def current_dtype(self):
        """Return numpy dtype for current settings, plus the bytes-per-sample."""
        if self.rf32.isChecked():
            # Native-endian float32. signed/big-endian checkboxes do not apply.
            return np.dtype("f4"), 4
        signed = self.signed.isChecked()
        be = self.big_endian.isChecked()
        if self.r8.isChecked():
            return (np.dtype("i1") if signed else np.dtype("u1")), 1
        endian = ">" if be else "<"
        kind = "i" if signed else "u"
        return np.dtype(f"{endian}{kind}2"), 2

    def on_bits_changed(self, *_):
        is_float = self.rf32.isChecked()
        self.signed.setEnabled(not is_float)
        self.big_endian.setEnabled(not is_float)

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

    def on_select_region_toggled(self, checked: bool):
        if checked:
            (x0, x1), _ = self.plot.viewRange()
            mid = 0.5 * (x0 + x1)
            half = 0.25 * (x1 - x0)
            self.region.setRegion((mid - half, mid + half))
            self.region.show()
        else:
            self.region.hide()
        self.zoom_btn.setEnabled(checked)

    def on_zoom_clicked(self):
        x0, x1 = self.region.getRegion()
        if x1 > x0:
            self.plot.setXRange(x0, x1, padding=0)
        self.select_region.setChecked(False)

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
            # Span exceeds the read cap (very zoomed out). Reading every byte of the
            # span is hopeless for multi-GB files, so sample sparsely: at each of
            # `buckets` evenly-spaced bucket starts, read `inner` samples at fixed
            # sub-positions within the bucket and take min/max. Total bytes touched
            # ≈ buckets * inner * bps regardless of file size.
            dtype, bps = self.current_dtype()
            buckets = min(px_width, span)
            bucket_size = max(1, span // buckets)
            inner = min(bucket_size, 64)

            sub_pos = (np.arange(inner, dtype=np.int64) * bucket_size) // inner
            bucket_starts = x0 + np.arange(buckets, dtype=np.int64) * bucket_size
            sample_idx = bucket_starts[:, None] + sub_pos[None, :]
            np.clip(sample_idx, 0, n - 1, out=sample_idx)

            # Gather raw bytes via fancy indexing into the uint8 memmap, then view
            # as the target dtype (handles endianness).
            byte_idx = (sample_idx[:, :, None] * bps
                        + np.arange(bps, dtype=np.int64)[None, None, :])
            buf = np.ascontiguousarray(self.raw[byte_idx.ravel()])
            samples = buf.view(dtype).reshape(buckets, inner)

            mins = samples.min(axis=1)
            maxs = samples.max(axis=1)
            ys = np.empty(buckets * 2, dtype=np.float32)
            ys[0::2] = mins
            ys[1::2] = maxs
            centers = bucket_starts + bucket_size // 2
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
        self._status_base = (
            f"dtype={dtype.str}  bps={bps}  samples={n:,}  view=[{x0:,}..{x1:,}]  "
            f"span={span:,}  px={px_width}  drawn={len(xs):,}"
        )
        self.status.showMessage(self._status_base)

    def on_mouse_moved(self, scene_pos):
        vb = self.plot.getViewBox()
        if not self.plot.sceneBoundingRect().contains(scene_pos):
            self.status.showMessage(self._status_base)
            return
        pt = vb.mapSceneToView(scene_pos)
        idx = int(round(pt.x()))
        y_cursor = float(pt.y())
        n = self.total_samples()
        if idx < 0 or idx >= n:
            self.status.showMessage(f"{self._status_base}  |  y={y_cursor:.6g}")
            return
        samples, _ = self.get_samples(idx, 1)
        if len(samples) == 0:
            self.status.showMessage(f"{self._status_base}  |  y={y_cursor:.6g}")
            return
        v = samples[0]
        if np.issubdtype(samples.dtype, np.floating):
            vstr = f"{float(v):.6g}"
        else:
            vstr = f"{int(v)}"
        self.status.showMessage(
            f"{self._status_base}  |  @{idx:,} = {vstr}  |  y={y_cursor:.6g}"
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
