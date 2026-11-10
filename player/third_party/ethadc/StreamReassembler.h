// SPDX-License-Identifier: GPL-3.0-or-later
//
// Turns the datagrams of an ethadc stream back into the continuous sample
// stream the sender saw: packets are put back in order, duplicates dropped,
// and a packet that never arrives becomes exactly as many filler bytes as it
// carried, because nothing downstream can resynchronise.
//
// This is the logic alone.  It owns no socket, no thread and no clock: the
// caller hands it datagrams and the time they arrived, calls tick() when time
// passes without one, and gets the continuous bytes through a sink callback.
// That keeps it testable with a fake clock and lets a player embed it instead
// of running the ethadc-rx process next to it.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "Protocol.h"

namespace ethadc {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

// What goes in place of a packet that never arrived.  Hold repeats the last
// sample group, which leaves no DC step whatever container convention the
// stream uses; this matters because 10-bit data in a 16-bit container centres
// on 512 rather than 32768, so there is no single right "zero" to write.  The
// alternatives are there for when a deliberate, recognisable value is wanted.
enum class FillMode { Hold, Midscale, Zero };

inline const char *fill_mode_name(FillMode m) {
    switch (m) {
        case FillMode::Hold: return "hold";
        case FillMode::Midscale: return "mid";
        case FillMode::Zero: return "zero";
    }
    return "?";
}

struct ReassemblerOptions {
    std::size_t window = 256;  // reorder window, in packets
    double timeout_ms = 5.0;   // how long to wait for a missing packet
    double prime_ms = 5.0;     // how long to buffer before output starts
    FillMode fill = FillMode::Hold;
};

// Fixed for the life of a stream; taken from its first packet.
struct StreamInfo {
    Format format = Format::U8;
    std::uint32_t sample_rate = 0;
    std::size_t payload_len = 0;
};

struct ReassemblerStats {
    std::uint64_t packets = 0;      // well-formed datagrams received
    std::uint64_t bytes_out = 0;
    std::uint64_t lost = 0;         // packets never seen, concealed
    std::uint64_t fill_bytes = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t recovered = 0;    // arrived out of order, put back in place
    std::uint64_t too_late = 0;     // arrived after their slot was concealed
    std::uint64_t malformed = 0;
    std::uint64_t timeouts = 0;     // gaps given up on by the timer
    std::uint64_t overruns = 0;     // gaps given up on because the window filled
    std::uint64_t longest_gap = 0;  // in packets

    // Packets the sender must have produced, as far as we can tell.
    std::uint64_t expected() const { return packets - duplicates - too_late + lost; }
};

class StreamReassembler {
public:
    using Sink = std::function<void(const std::uint8_t *, std::size_t)>;
    using StreamCallback = std::function<void(const StreamInfo &)>;

    // `sink` receives the continuous byte stream; `on_stream` is called when a
    // stream starts, or restarts, with what its packets say about it.
    StreamReassembler(const ReassemblerOptions &options, Sink sink, StreamCallback on_stream = {})
        : m_o(options),
          m_timeout(to_duration(options.timeout_ms)),
          m_prime(to_duration(options.prime_ms)),
          m_sink(std::move(sink)),
          m_on_stream(std::move(on_stream)),
          m_slot(options.window),
          m_present(options.window, false) {
        if (options.window < 2)
            throw std::invalid_argument("the reorder window must hold at least 2 packets");
        if (!m_sink)
            throw std::invalid_argument("a sink is required");
    }

    // One datagram, exactly as it came off the socket, and when it arrived.
    void packet(const std::uint8_t *datagram, std::size_t len, TimePoint now) {
        Header h;
        if (!decode_header(datagram, len, h)) {
            ++m_st.malformed;
        } else {
            ++m_st.packets;
            m_last_packet = now;
            handle(h, datagram + kHeaderSize, now);
        }
        tick(now);
    }

    // Runs the timers.  packet() calls it; the caller does too whenever time
    // passes without a datagram, since a gap is only concealed once its timeout
    // has run out.
    void tick(TimePoint now) {
        if (m_priming && now >= m_prime_deadline)
            end_priming();

        // A packet is only declared lost once the timer runs out on it, and
        // only while later packets are actually waiting behind it -- a sender
        // that has simply stopped must not make us fill forever.
        const bool gap_now = m_have_stream && !m_priming && m_present_count > 0 &&
                             !m_present[m_next_seq % m_o.window];
        if (gap_now && !m_gap_open) {
            m_gap_open = true;
            m_gap_since = now;
        } else if (!gap_now) {
            m_gap_open = false;
        }
        if (m_gap_open && now - m_gap_since >= m_timeout) {
            ++m_st.timeouts;
            while (m_present_count > 0 && !m_present[m_next_seq % m_o.window])
                conceal_one();
            m_gap_open = false;
        }
    }

    // The stream is over: anything still in the window belongs before its end.
    void finish() {
        if (m_priming)
            end_priming();
        while (m_have_stream && m_present_count > 0)
            conceal_one();
    }

    bool have_stream() const { return m_have_stream; }
    // The packet flagged as the last of the stream has been handed out (or
    // concealed), so nothing more is coming from this stream.  A restart
    // clears it.
    bool ended() const { return m_ended; }
    const StreamInfo &stream() const { return m_info; }
    const ReassemblerStats &stats() const { return m_st; }
    // Arrival time of the latest well-formed packet; meaningful once have_stream().
    TimePoint last_packet() const { return m_last_packet; }

private:
    static Clock::duration to_duration(double ms) {
        return std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double, std::milli>(ms));
    }

    static std::int32_t diff(std::uint32_t a, std::uint32_t b) {
        return static_cast<std::int32_t>(a - b);
    }

    void store(std::uint32_t seq, const std::uint8_t *payload, std::size_t len) {
        m_slot[seq % m_o.window].assign(payload, payload + len);
        m_present[seq % m_o.window] = true;
        ++m_present_count;
    }

    void handle(const Header &h, const std::uint8_t *payload, TimePoint now) {
        const std::int32_t seq_diff = diff(h.seq, m_next_seq);
        const bool out_of_window = seq_diff < 0
                                       ? static_cast<std::size_t>(-seq_diff) > m_o.window
                                       : static_cast<std::size_t>(seq_diff) >= m_o.window;
        // A duplicate or a late copy of the first packet still carries
        // kFlagRestart; only treat it as a real restart when the sequence
        // cannot belong to the stream in hand.
        const bool restart = (h.flags & kFlagRestart) != 0 && out_of_window;
        if (!m_have_stream || restart) {
            if (m_have_stream) {
                // Drop whatever the old stream left behind rather than
                // silently splicing two streams together.
                std::fill(m_present.begin(), m_present.end(), false);
                m_present_count = 0;
            }
            m_have_stream = true;
            m_next_seq = m_lowest_seq = m_highest_seq = h.seq;
            m_priming = true;
            m_prime_deadline = now + m_prime;
            m_info = {h.format, h.sample_rate, h.payload_len};
            m_last_group.clear();
            m_have_end = false;
            m_ended = false;
            if (m_on_stream)
                m_on_stream(m_info);
        }

        // A short payload is only legitimate on the packet that ends the
        // stream; any other length change would break the mapping from a
        // sequence gap to a byte count, as would a change of format.
        const bool short_end = h.payload_len < m_info.payload_len && (h.flags & kFlagEnd) != 0;
        if (h.format != m_info.format || (h.payload_len != m_info.payload_len && !short_end)) {
            ++m_st.malformed;
            return;
        }
        if ((h.flags & kFlagEnd) != 0) {
            m_have_end = true;
            m_end_seq = h.seq;
        }

        // Priming: for the first few milliseconds of a stream packets are
        // buffered but nothing is handed downstream, so the origin stays open.
        // Without it the very first packet to arrive fixes the origin, and a
        // stream whose opening packets were reordered loses its head -- and
        // on a live stream every gap would start one packet late.
        if (m_priming) {
            const std::int32_t from_low = diff(h.seq, m_lowest_seq);
            const bool fits = from_low >= 0
                                  ? static_cast<std::size_t>(from_low) < m_o.window
                                  : static_cast<std::size_t>(m_highest_seq - h.seq) < m_o.window;
            if (fits) {
                if (m_present[h.seq % m_o.window]) {
                    ++m_st.duplicates;
                } else {
                    store(h.seq, payload, h.payload_len);
                    if (from_low < 0) {
                        m_lowest_seq = h.seq;
                        ++m_st.recovered;
                    }
                    if (diff(h.seq, m_highest_seq) > 0)
                        m_highest_seq = h.seq;
                }
                return;
            }
            // Too far from the origin to buffer: start the stream with what we
            // have and treat this packet as a normal arrival.
            end_priming();
        }

        const std::int32_t d = diff(h.seq, m_next_seq);
        if (d < 0) {
            ++m_st.too_late;
        } else if (static_cast<std::size_t>(d) >= m_o.window) {
            // Too far ahead to hold: conceal everything that can no longer fit
            // in the window.
            const std::uint32_t target = h.seq - static_cast<std::uint32_t>(m_o.window) + 1;
            ++m_st.overruns;
            while (diff(target, m_next_seq) > 0)
                conceal_one();
            store(h.seq, payload, h.payload_len);
            drain();
        } else if (m_present[h.seq % m_o.window]) {
            ++m_st.duplicates;
        } else {
            if (diff(h.seq, m_highest_seq) < 0)
                ++m_st.recovered;
            store(h.seq, payload, h.payload_len);
            if (d == 0)
                drain();
        }
        if (diff(h.seq, m_highest_seq) > 0)
            m_highest_seq = h.seq;
    }

    void emit(const std::uint8_t *data, std::size_t len) {
        m_sink(data, len);
        m_st.bytes_out += len;
    }

    // Rebuilds the bytes handed downstream in place of a packet that never
    // arrived, then emits them.
    void emit_filler() {
        const auto &info = format_info(m_info.format);
        std::vector<std::uint8_t> group;
        if (m_o.fill == FillMode::Zero)
            group.assign(info.group_bytes, 0);
        else if (m_o.fill == FillMode::Midscale || m_last_group.empty())
            group = midscale_group(m_info.format);
        else
            group = m_last_group;
        m_filler.resize(m_info.payload_len);
        for (std::size_t i = 0; i < m_filler.size(); i += group.size())
            std::memcpy(m_filler.data() + i, group.data(),
                        std::min(group.size(), m_filler.size() - i));

        emit(m_filler.data(), m_filler.size());
        m_st.fill_bytes += m_filler.size();
        ++m_st.lost;
        ++m_run_of_losses;
        m_st.longest_gap = std::max(m_st.longest_gap, m_run_of_losses);
    }

    // Hands out everything at the front of the window that has arrived.
    void drain() {
        while (m_present[m_next_seq % m_o.window]) {
            const auto &p = m_slot[m_next_seq % m_o.window];
            const std::size_t g = format_info(m_info.format).group_bytes;
            if (m_o.fill == FillMode::Hold && p.size() >= g)
                m_last_group.assign(p.end() - static_cast<std::ptrdiff_t>(g), p.end());
            emit(p.data(), p.size());
            m_present[m_next_seq % m_o.window] = false;
            --m_present_count;
            ++m_next_seq;
            m_run_of_losses = 0;
            note_passed_end();
        }
    }

    void note_passed_end() {
        if (m_have_end && diff(m_next_seq, m_end_seq) > 0)
            m_ended = true;
    }

    // Closes the priming window: the lowest sequence buffered becomes the
    // origin, and output starts flowing.
    void end_priming() {
        m_priming = false;
        m_next_seq = m_lowest_seq;
        drain();
    }

    // Gives up on the packet at the front of the window and moves on.
    void conceal_one() {
        emit_filler();
        ++m_next_seq;
        note_passed_end();
        drain();
    }

    ReassemblerOptions m_o;
    Clock::duration m_timeout, m_prime;
    Sink m_sink;
    StreamCallback m_on_stream;

    // Reorder window: one slot per outstanding sequence number.
    std::vector<std::vector<std::uint8_t>> m_slot;
    std::vector<bool> m_present;
    std::size_t m_present_count = 0;
    std::vector<std::uint8_t> m_filler;
    std::vector<std::uint8_t> m_last_group;  // for FillMode::Hold

    bool m_have_stream = false;
    bool m_priming = false;
    std::uint32_t m_next_seq = 0, m_lowest_seq = 0, m_highest_seq = 0;
    StreamInfo m_info;
    ReassemblerStats m_st;
    TimePoint m_last_packet{}, m_gap_since{}, m_prime_deadline{};
    bool m_gap_open = false;
    std::uint64_t m_run_of_losses = 0;
    bool m_have_end = false, m_ended = false;
    std::uint32_t m_end_seq = 0;
};

}  // namespace ethadc
