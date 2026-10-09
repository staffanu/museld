// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_FRAMEREADER_H
#define MUSECPP_FRAMEREADER_H

#include <thread>
#include <condition_variable>
#include <exception>
#include <optional>
#include <vector>
#include <deque>
#include <atomic>
#include "AudioDefs.h"

namespace musevk {
    class VulkanBuffer;
}
class Logger;

enum class InputStatus {
    eNormal = 0,
    eBuffersEmpty = 1,
    eBuffersFilled = 2, // buffers all filled, real-time input
    eTimeout = 3, // returned value is null
    eEof = 4, // returned value is null
};

template<class InputBlock>
class FrameReader {
public:
    void operator=(const FrameReader&) = delete;
    FrameReader(const FrameReader&) = delete;
    // Derived classes call cleanup() from their own destructors, so the reader
    // thread is joined while the threadFunc() override it is running still
    // exists.  The call here is the idempotent backstop (and resolves to the
    // base cleanup(), which is why it cannot replace the derived calls).
    virtual ~FrameReader();

    [[nodiscard]] virtual bool initialize(std::vector<std::unique_ptr<InputBlock>> &buffers);
    virtual void cleanup();

    std::pair<std::unique_ptr<InputBlock>, InputStatus> getNextInputBuffer();
    void returnBuffer(std::unique_ptr<InputBlock> &buffer);
    // Relative seek; false when the input refused it and nothing moved
    // (live input ignores seeks and reports true)
    virtual bool seek(double seconds) = 0;
    // Positions the input at an absolute sample offset and returns the new
    // seek generation (see InputBlockBase::seek_generation), or nullopt when
    // the reader cannot: live input, the legacy readers without input
    // offset bookkeeping, or a position the input refuses (then nothing
    // moved).  The chapter search is built on this.
    virtual std::optional<uint32_t> seekToInputSample(int64_t) { return std::nullopt; }
    // The input's sample rate, and its length in samples (-1 when unknown);
    // what the search needs to turn disc time into input offsets and to
    // keep its probes inside the file
    [[nodiscard]] virtual double inputSampleRate() const { return 0; }
    [[nodiscard]] virtual int64_t inputSampleCount() const { return -1; }
    virtual void setEfmEnabled(bool) {}
    // Which audio source to demodulate.  MUSE readers know two sources, so the
    // default maps the track onto the EFM on/off switch; the NTSC reader
    // overrides this with its three-way selection (analog/EFM/AC3-RF).
    virtual void setAudioTrack(AudioTrack track) { setEfmEnabled(track == AudioTrack::eEfm); }
    // CX noise reduction for the NTSC analog audio; a no-op elsewhere
    virtual void setAnalogCx(bool) {}
    // Taps in the EFM timing recovery's adaptive filter (0 disables it).
    // efmAdaptiveFilterSize() is negative when the reader has no EFM demodulator.
    virtual void setEfmAdaptiveFilterSize(int) {}
    [[nodiscard]] virtual int efmAdaptiveFilterSize() const { return -1; }

protected:
    // Hands the frames already read but not yet collected back to the reader
    // thread, for a seek: they are from the abandoned position, and every
    // one of them would cost the consumer a decode before the first frame
    // from the new position comes through
    void discardFilledBuffers() {
        std::unique_lock<std::mutex> lock(m_mutex);
        for (auto &b : m_filled_input_buffers)
            m_vacant_input_buffers.push_back(std::move(b));
        m_filled_input_buffers.clear();
        m_cv_vacant.notify_one();
    }

    // input_is_realtime is separate from input_is_fifo since we could have non-real-time input from a pipe
    // that is written to by someone that reads from a file (the example so far is reading from the FmDemodulator)
    FrameReader(Logger &log, const std::string &filename,
                bool input_is_realtime,
                double initial_seek_seconds, const std::optional<std::string> &output_filename);

    virtual void threadFunc() = 0;

    Logger &m_log;
    const std::string m_filename;
    const bool m_input_is_realtime; // if real-time, we tell the player to speed up if all buffers are full
    const double m_initial_seek_seconds;
    const std::optional<std::string> m_output_filename;
    int m_output_file_fd;
    std::deque<std::unique_ptr<InputBlock>> m_vacant_input_buffers;
    std::deque<std::unique_ptr<InputBlock>> m_filled_input_buffers;
    std::atomic<bool> m_stop_request;
    std::atomic<bool> m_reader_thread_finished;
    std::mutex m_mutex;
    std::condition_variable m_cv_filled;
    std::condition_variable m_cv_vacant;
    int m_get_input_buffers_count;

private:
    void *m_file_write_buffer;
    std::exception_ptr m_thread_exception; // set by the reader thread; guarded by m_mutex
    std::thread m_reader_thread; // last member: joined before the state above goes away
};

#include "FrameReader.impl.h"

#endif //MUSECPP_FRAMEREADER_H
