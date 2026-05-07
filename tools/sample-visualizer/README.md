# sample-visualizer

A quick interactive viewer for raw binary sample files of unknown format. Reinterpret
the same bytes on the fly as 8/16-bit, signed/unsigned, little/big-endian — pan/zoom
with the mouse to see which combination produces a sensible signal, then rename the
file accordingly.

Handles multi-GB files via `numpy.memmap` (no full load) and uses `np.minimum.reduceat`
/ `np.maximum.reduceat` for exact min/max-envelope decimation, with `connect="pairs"`
in pyqtgraph so each pixel column is a single clean vertical bar.

## Setup

```bash
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

## Run

```bash
./sample-visualizer.py path/to/file.bin
```

The script auto-detects the sibling `.venv` and re-execs through it, so a plain
`./sample-visualizer.py …` works after the one-time setup above. No activation needed.

## Controls

- **8-bit / 16-bit** — sample width. Toggling preserves the visible byte range.
- **signed / big-endian** — interpretation. Toggling does not change the view.
- **Y: auto** — auto-range vertically. Uncheck to lock the current y-range; the
  min/max fields snap to whatever was visible when you turned auto off.
- Mouse wheel — zoom (hold X/Y to constrain to one axis).
- Left-drag — pan. Right-drag — zoom box. Right-click → *View All* resets.
- Zoom is clamped to a minimum span of 100 samples and a maximum of the full file.

## Files

- `sample-visualizer.py` — the viewer (single file)
- `requirements.txt` — `numpy`, `pyqtgraph`, `PyQt6`
