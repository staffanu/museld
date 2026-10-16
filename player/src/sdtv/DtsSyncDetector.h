// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_DTSSYNCDETECTOR_H
#define MUSELD_DTSSYNCDETECTOR_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>
#include "efm/TwoChannelSample.h"

/**
 * Tells a DTS bitstream on the EFM track from PCM.
 *
 * A DTS laserdisc's EFM track is a DTS core stream in 16-bit words, one sync
 * word per DTS frame: a frame holds at most 4096 samples (93 ms), the usual
 * 512 (11.6 ms), so a stream has at least ten sync words per second and
 * normally about ninety.  PCM contains the sync patterns too, now and then:
 * a 32-bit match in audio samples, and the WOW demo disc showed about one a
 * minute.  Counting them without regard to density flipped that disc into DTS
 * decoding (silence) for seconds at a time whenever two such hits fell within
 * the hold-off, so the detector asks for the density a real stream cannot
 * fall below -- c_enter_syncs sync words within the last c_window_reads reads
 * (a second of video frames) -- before it switches, and once switched it
 * stays until no sync word has been seen for c_release_reads reads, which
 * rides out dropouts.  Samples the CIRC decoder flagged as erased are not
 * looked at: they hold whatever the concealment will replace.
 */
class DtsSyncDetector {
public:
    static constexpr int c_window_reads = 30;  // about one second of frames
    static constexpr int c_enter_syncs = 8;    // below the floor of ~10/s for the largest DTS frames
    static constexpr int c_release_reads = 120; // four seconds without a sync word

    // Sync words of the DTS core stream in the four 16-bit framings it can
    // take on a disc (big/little endian, 16-bit and 14-bit packing), counted
    // over the words of both channels in disc order (L then R)
    [[nodiscard]] static int countSyncWords(const std::vector<TwoChannelSampleWithErasureFlags> &samples) {
        int syncs = 0;
        uint16_t prev = 0;
        bool prev_erased = true;
        for (const auto &s : samples)
            for (int ch = 0; ch < 2; ch++) {
                const auto w = (uint16_t)s.samples[ch];
                if (!prev_erased && !s.erased[ch]
                    && ((prev == 0x7ffe && w == 0x8001) || (prev == 0x1fff && w == 0xe800) ||
                        (prev == 0xfe7f && w == 0x0180) || (prev == 0xff1f && w == 0x00e8)))
                    syncs++;
                prev = w;
                prev_erased = s.erased[ch];
            }
        return syncs;
    }

    // One read's worth of decoded EFM samples; returns whether the track is
    // taken to carry a DTS bitstream from this read on
    bool update(const std::vector<TwoChannelSampleWithErasureFlags> &samples) {
        return update(countSyncWords(samples));
    }

    // The same, given the sync word count of the read
    bool update(int syncs) {
        m_window_sum += syncs - m_window[m_window_index];
        m_window[m_window_index] = syncs;
        m_window_index = (m_window_index + 1) % c_window_reads;
        if (syncs > 0)
            m_reads_since_sync = 0;
        else if (m_reads_since_sync < c_release_reads)
            m_reads_since_sync++;
        if (!m_active && m_window_sum >= c_enter_syncs)
            m_active = true;
        else if (m_active && m_reads_since_sync >= c_release_reads)
            m_active = false;
        return m_active;
    }

    [[nodiscard]] bool active() const { return m_active; }

private:
    std::array<int, c_window_reads> m_window{};
    int m_window_index = 0;
    int m_window_sum = 0;
    int m_reads_since_sync = c_release_reads;
    bool m_active = false;
};

#endif //MUSELD_DTSSYNCDETECTOR_H
