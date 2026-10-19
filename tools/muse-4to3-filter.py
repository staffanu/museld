#!/usr/bin/env python3
# The weighted least-squares low-pass for the MUSE decoder's 4:3 horizontal
# sample-rate conversion of the inter-frame luma (muse/Shaders.cpp,
# m_filter_4_to_3_video_buffer): pass band to 0.125 of the 3x-upsampled rate,
# stop band from 0.167 with weight 30, 37 taps.  Prints the taps and the response.
import numpy as np

def design(ntaps, pass_edge, stop_edge, wstop):
    grid = np.linspace(0, 0.5, 8192)
    n = np.arange(ntaps) - (ntaps - 1) // 2
    desired = (grid <= pass_edge) * 1.0
    weight = np.where(grid <= pass_edge, 1.0, np.where(grid >= stop_edge, wstop, 0.0))
    A = np.cos(2 * np.pi * grid[:, None] * n[None, :])
    W = np.sqrt(weight)[:, None]
    h, *_ = np.linalg.lstsq(A * W, desired * W[:, 0], rcond=None)
    return h / np.sum(h)

def response(h, f):
    n = np.arange(len(h)) - (len(h) - 1) / 2
    return np.abs(np.sum(np.array(h)[None, :] * np.exp(-2j * np.pi * f[:, None] * n[None, :]), axis=1))

if __name__ == "__main__":
    h = design(37, 0.125, 0.167, 30)
    for i in range(0, len(h), 4):
        print("                  " + ", ".join(f"{v:.18f}" for v in h[i:i + 4]) + ",")
    f = np.linspace(0, 0.5, 4001)
    r = 20 * np.log10(response(h, f))
    print(f"pass band ripple {r[f <= 0.125].max():+.2f}/{r[f <= 0.125].min():+.2f} dB, stop band max {r[f >= 0.167].max():.1f} dB")
