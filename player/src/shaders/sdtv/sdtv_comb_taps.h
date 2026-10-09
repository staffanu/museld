// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

// Chroma comb taps shared by sdtv_chroma_taps.comp (which evaluates them once
// per column) and sdtv_decode_single_field.comp (which still needs them on
// the rescue and donor paths).  The including shader declares input_frame,
// prev_frame and next_frame before including this.

// 3-line comb Y/C separation: the same-field adjacent lines are 180 degrees
// apart in subcarrier phase.  It beat 1D notch/bandpass separation clearly in
// an A/B comparison (both halves of the picture, three captures): both its Y
// and C paths carry about 4 dB less noise, at the price of some vertical
// resolution.  For the luma that price is only paid in the chroma band: the
// field decoder subtracts the band around fsc of this estimate from the
// composite instead of using the comb's own luma sum.
//
// An adaptive variant was tried and measured against the temporal separation
// on still content (2026-09): choosing between the line above, the line
// below and a horizontal bandpass by how well the neighbours two lines out
// (same subcarrier phase) match this one.  It did not help.  Real pictures
// change gradually from line to line, where the symmetric comb's error is
// the second vertical difference and a one-sided comb's the first; and at
// the noise levels of this medium a per-pixel choice is wrong often enough
// to cost more than the true steps gain.  The chroma error of the fixed comb
// was already within the noise of the temporal reference.
#ifdef SDTV_PAL
// PAL: the field neighbours' chroma sits at -90 and +90 degrees and cancels
// in their sum (sdtv_decode_single_field.comp); on the field's last line,
// where next_line is the line itself, the line two above (181 degrees, the
// same V switch) stands in as a two-line comb
float16_t chroma_sample(uint frame_line, uint next_line, uint fcol) {
    return next_line > frame_line
        ? input_frame[frame_line][fcol] - 0.5hf * (input_frame[frame_line - 1][fcol] + input_frame[next_line][fcol])
        : 0.5hf * (input_frame[frame_line][fcol] - input_frame[frame_line - 2][fcol]);
}
#else
float16_t chroma_sample(uint frame_line, uint next_line, uint fcol) {
    return -0.25hf * input_frame[frame_line - 1][fcol]
           + 0.5hf * input_frame[frame_line][fcol]
           - 0.25hf * input_frame[next_line][fcol];
}
#endif

// Comb and chroma taps on the neighbour frames, for temporal dropout
// concealment: a still neighbour's same pixel is a far better donor than
// the surrounding lines.  The donor's chroma is demodulated against its own
// frame's burst reference, which absorbs the frame-to-frame subcarrier
// inversion and the sampling phase jitter in one step.
float16_t chroma_sample_prev(uint fl, uint nl, uint fc) {
    return -0.25hf * prev_frame[fl - 1][fc] + 0.5hf * prev_frame[fl][fc] - 0.25hf * prev_frame[nl][fc];
}

float16_t chroma_sample_next(uint fl, uint nl, uint fc) {
    return -0.25hf * next_frame[fl - 1][fc] + 0.5hf * next_frame[fl][fc] - 0.25hf * next_frame[nl][fc];
}

// Sampling phase correction for the temporal pairs: the sampling grid follows
// the sync pulses, whose timing against the subcarrier wanders by some tens
// of nanoseconds over a field and differently in each frame, so a line and
// the same line of the neighbouring frame are sampled some degrees of
// subcarrier apart, and the pair difference (f0 - fneighbour)/2 comes out as
// the chroma rotated by half the lines' phase offset.  The offset is measured
// exactly by the two lines' own colour bursts: e^(-j delta) = -b_n conj(b_0)
// / |...|, and the estimate is de-rotated by delta/2 using the
// one-sample-shift quadrature, which is exact at fsc on the 4 fsc grid.
// Returns (cos, sin) of delta/2.
f16vec2 pair_rotation(f16vec2 bn, f16vec2 b0) {
    float rx = float(bn.x) * float(b0.x) + float(bn.y) * float(b0.y);
    float ry = float(bn.y) * float(b0.x) - float(bn.x) * float(b0.y);
    float m = sqrt(rx * rx + ry * ry);
    if (m < 1e-4)
        return f16vec2(1.hf, 0.hf); // no usable burst on one of the lines
    float cd = -rx / m;
    float sd = ry / m;
    float ch = sqrt(max(0.5 * (1.0 + cd), 0.0));
    float sh = (sd >= 0.0 ? 1.0 : -1.0) * sqrt(max(0.5 * (1.0 - cd), 0.0));
    return f16vec2(float16_t(ch), float16_t(sh));
}

// Interpolate two rotations: the grid phase is continuous in scan time, so
// between this line's burst and the next line's burst the phase offset
// drifts linearly, and the correction at a column is the interpolation of
// the two measurements.  Without it the correction is exact at the burst and
// decays across the line (measured: half the artifact left at the far edge).
f16vec2 lerp_rotation(f16vec2 a, f16vec2 b, float t) {
    vec2 v = mix(vec2(a), vec2(b), t);
    float l = max(length(v), 1e-4);
    return f16vec2(v / l);
}

float16_t rotated_prev_pair(uint fl, uint fc, f16vec2 r) {
    float16_t c0 = 0.5hf * (input_frame[fl][fc] - prev_frame[fl][fc]);
    float16_t cm = 0.5hf * (input_frame[fl][fc - 1] - prev_frame[fl][fc - 1]);
    float16_t cp = 0.5hf * (input_frame[fl][fc + 1] - prev_frame[fl][fc + 1]);
    return c0 * r.x + 0.5hf * (cm - cp) * r.y;
}

float16_t rotated_next_pair(uint fl, uint fc, f16vec2 r) {
    float16_t c0 = 0.5hf * (input_frame[fl][fc] - next_frame[fl][fc]);
    float16_t cm = 0.5hf * (input_frame[fl][fc - 1] - next_frame[fl][fc - 1]);
    float16_t cp = 0.5hf * (input_frame[fl][fc + 1] - next_frame[fl][fc + 1]);
    return c0 * r.x + 0.5hf * (cm - cp) * r.y;
}


#ifdef SDTV_PAL
// --- PAL: the frame sources, the spatial comb, the line-pair -U axis ---
// The PAL decode reads one of three frames: this one (src 0), or the
// previous / next for the temporal dropout donor (-1 / +1).  GLSL has no
// buffer references, so the source is selected per access.
float16_t pal_f(int src, uint fl, uint fc) {
    return src == 0 ? input_frame[fl][fc] : src < 0 ? prev_frame[fl][fc] : next_frame[fl][fc];
}

f16vec2 pal_burst(int src, uint fl) {
    return src == 0 ? phase_data[fl] : src < 0 ? prev_phase_data[fl] : next_phase_data[fl];
}

// The field neighbour a line pairs with: the one below, except for the
// field's last line
uint pal_pair_line(uint fl, uint field_last) {
    return fl < field_last ? fl + 1 : fl - 1;
}

// The line's chroma estimate at a column: the composite less the mean of
// its two field neighbours, whose chroma is at -90 and +90 degrees and
// cancels (the field's first line borrows the blank line above it)
float16_t pal_chroma(int src, uint fl, uint field_last, uint fc) {
    // on the field's last line the line two above (181 degrees, the same V
    // switch) stands in as a two-line comb
    return fl < field_last
        ? pal_f(src, fl, fc) - 0.5hf * (pal_f(src, fl - 1, fc) + pal_f(src, fl + 1, fc))
        : 0.5hf * (pal_f(src, fl, fc) - pal_f(src, fl - 2, fc));
}

// The line's luma at a column: the composite less the chroma band of the
// comb estimate
float16_t pal_luma(int src, uint fl, uint field_last, uint fc) {
    return pal_f(src, fl, fc) - (0.5hf * pal_chroma(src, fl, field_last, fc)
                                 - 0.25hf * (pal_chroma(src, fl, field_last, fc - 2) + pal_chroma(src, fl, field_last, fc + 2)));
}

// One line's -U axis estimate from its pair with the field neighbour, in
// the line's own grid frame: the neighbour's burst de-rotated by the
// structural 270.576 degrees added to the line's own (the +-45 degree
// swings cancel).  Zero where either burst is missing.
vec2 pal_pair_axis(int src, uint fl, uint field_last) {
    const uint nxt = pal_pair_line(fl, field_last);
    vec2 m0 = vec2(pal_burst(src, fl));
    vec2 m1 = vec2(pal_burst(src, nxt));
    if (dot(m0, m0) <= 0.04 || dot(m1, m1) <= 0.04)
        return vec2(0.0);
    // The measured phasor is the conjugate of the subcarrier phase (see the
    // sine LUT), so the physical advance of +270.576 degrees per line is
    // undone by rotating the next line's phasor by +270.576 (the previous
    // line's by -270.576).
    const float delta = radians(270.576) * (nxt > fl ? 1.0 : -1.0);
    return m0 + vec2(m1.x * cos(delta) - m1.y * sin(delta), m1.x * sin(delta) + m1.y * cos(delta));
}


// The -U axis of a line averaged over its two neighbours on either side,
// each pair estimate brought into this line's grid frame by the structural
// rotation and weighted (1 2 3 2 1): the sampling phase is stable from line
// to line once the pilot has refined the timebase, so what the average
// removes is the burst's measurement noise, about 2 degrees per line pair
// on a 27 dB capture.  The sum is 9 pair estimates' worth; zero where none
// is usable.
vec2 pal_smoothed_axis(int src, uint fl, uint field_last) {
    const uint field_first = field_last + 1 - SDTV_FIELD_HEIGHT;
    const float delta = radians(270.576);
    vec2 r = vec2(0.0);
    for (int k = -2; k <= 2; k++) {
        int l = int(fl) + k;
        if (l < int(field_first) || l > int(field_last))
            continue;
        vec2 rk = pal_pair_axis(src, uint(l), field_last);
        if (dot(rk, rk) <= 0.25)
            continue;
        const float a = delta * float(k); // line fl + k's frame is k advances ahead
        const float wk = 3.0 - abs(float(k));
        r += wk * vec2(rk.x * cos(a) - rk.y * sin(a), rk.x * sin(a) + rk.y * cos(a));
    }
    return r;
}

// The frame-pair rotation for PAL's temporal comb (frames N and N +- 2, see
// SdtvDecoder), from the smoothed -U axes of the same line in both frames
// rather than from two single bursts: the axes carry the structural 180
// degrees between the frames like NTSC's bursts do, so pair_rotation applies
// as it is
f16vec2 pal_pair_rotation(int src_n, uint fl, uint field_last) {
    vec2 rn = pal_smoothed_axis(src_n, fl, field_last);
    vec2 r0 = pal_smoothed_axis(0, fl, field_last);
    return pair_rotation(f16vec2(rn), f16vec2(r0));
}
#endif
