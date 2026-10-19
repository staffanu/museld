// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <cstdint>
#include <bit>
#include <cmath>
#include <chrono>
#include <cstring>
#include <pthread.h>
#include <thread>
#include <utility>
#include <vector>
#include <iostream>
#include <filesystem>
#ifdef __APPLE__
#  include <TargetConditionals.h>
#endif
#include <array>
#include "MuseRfDemodulator.h"
#include "filter/FirFilterStage.h"
#include "filter/LeastSquaresFir.h"
#include "filter/RaisedCosine.h"
#include "filter/WindowedSinc.h"
#include "musevk/VulkanUtil.h"
#include "musevk/TimestampQueryPool.h"
#include "musevk/TimestampStatistics.h"

using namespace std;
using namespace musevk;

MuseRfDemodulator::MuseRfDemodulator(Logger &log, std::string executable_dir, PreparedInput input,
                                     float sample_frequency,
                                     musevk::VulkanManager &vulkan_manager, bool benchmark_shaders,
                                     bool efm_enabled, int efm_adaptive_filter_size)
: RfDemodulator<MuseDemodulatedBlock>(log, std::move(executable_dir), std::move(input), sample_frequency,
                                      vulkan_manager, MuseDemodulatedBlock::c_sample_block_size,
                                      benchmark_shaders),
  m_efm_demodulator(log, sample_frequency, MuseDemodulatedBlock::c_sample_block_size,
                    FirFilterStage::simdSupported(), true,
                    EfmDemodulator::defaultLog2Decimation(sample_frequency), efm_adaptive_filter_size, std::nullopt),
  m_efm_enabled(efm_enabled) {
}

void MuseRfDemodulator::demodulate() {
    CommandPool command_pool(m_vulkan_manager);
    std::unique_ptr<musevk::TimestampQueryPool> timestamp_query_pool = m_benchmark_shaders
            ? std::make_unique<musevk::TimestampQueryPool>(m_vulkan_manager.getPhysicalDevice(), m_vulkan_manager.getDevice(), 40)
            : nullptr;
    musevk::TimestampStatistics timestamp_statistics;
    auto command_buffer = command_pool.createCommandBuffer(timestamp_query_pool.get());

    vk::BufferUsageFlags buffer_usage_flags =
            vk::BufferUsageFlagBits::eStorageBuffer
            | vk::BufferUsageFlagBits::eTransferDst
            | vk::BufferUsageFlagBits::eTransferSrc;

    // Create buffers for all the filters

    // FIR band-pass filter to filter out noise over 22.5 MHz and also the pilot signal and EFM.
    // The pilot signal isn't completely suppressed but that doesn't seem to matter.
    // Match firdes' rectangular-window ntaps = (int)(21*Fs/(22*tw)) | 1
    // MUSELD_FIR_TRANSITION_SCALE widens the band-pass and low-pass transition
    // bands by that factor, which divides their tap counts by it: a measurement
    // hook for how much of the demodulator's GPU time the FIR taps are (the
    // picture degrades with it; not for playback)
    float transition_scale = 1.f;
    if (const char *env = getenv("MUSELD_FIR_TRANSITION_SCALE"); env != nullptr && atof(env) > 0) {
        transition_scale = (float)atof(env);
        m_log.info(ePerformance, std::format("FIR transition bands scaled by {} (measurement only)", transition_scale));
    }
    constexpr double c_band_low = 2.7e6, c_band_high = 22.3e6; // the MUSE FM band the band-pass keeps
    constexpr double c_band_center = (c_band_low + c_band_high) / 2, c_band_half_width = (c_band_high - c_band_low) / 2;

    // The band-pass output is analytic, with nothing left outside its pass band and
    // transitions, and a complex signal at fs / 2 holds any fs / 2 wide band, whatever
    // its position.  So when that band fits, the band-pass decimates by 2 itself (the
    // FIR shader's decimation constant): half its outputs, and the quadrature
    // demodulation, the envelope and the dropout passes run at the analytic rate,
    // with the low-pass no longer decimating.  On the Apple TV's A10X, where the
    // band-pass alone was half the MUSE chain's GPU time, this is what made MUSE
    // real time.  Below about 43 MHz the band does not fit and the chain runs as
    // before, the low-pass decimating; the carrier need not divide the sample rate
    // anywhere (fm_quadrature.comp takes the carrier out with a rotation computed
    // from the rates).
    const double bandpass_transition = 2e6 * transition_scale;
    const float bandpass_band_width = (float)(c_band_high - c_band_low + bandpass_transition);
    int analytic_decimation = m_sample_frequency / 2 >= bandpass_band_width ? 2 : 1;
    // Measurement hook: MUSELD_ANALYTIC_DECIMATION=1 keeps the band-pass at the input rate
    if (const char *env = getenv("MUSELD_ANALYTIC_DECIMATION"); env != nullptr && atoi(env) > 0) {
        analytic_decimation = atoi(env);
        m_log.info(ePerformance, std::format("analytic decimation forced to {} (measurement only)", analytic_decimation));
    }

    // Measurement hook: MUSELD_BANDPASS_DESIGN=rectangular|ls picks the band-pass design
    // regardless of the decimation (the rectangular design at decimation 2 shows the image)
    bool ls_bandpass = analytic_decimation == 2;
    if (const char *env = getenv("MUSELD_BANDPASS_DESIGN"); env != nullptr && *env) {
        ls_bandpass = std::string(env) == "ls";
        m_log.info(ePerformance, std::format("band-pass design forced to {} (measurement only)", ls_bandpass ? "least squares" : "rectangular"));
    }
    auto odd_taps_for = [transition_scale](int taps_at_62_5_mhz, double rate) {
        int n = (int)lround(taps_at_62_5_mhz * rate / 62.5e6 / transition_scale);
        return n | 1;
    };
    std::vector<std::complex<float>> bandpass_filter_def;
    if (!ls_bandpass) {
        bandpass_filter_def = WindowedSinc::complex_band_pass<float>(
                WindowedSinc::rectangular_ntaps(m_sample_frequency, bandpass_transition),
                m_sample_frequency, c_band_low, c_band_high);
    } else {
        // Decimating, the band-pass needs a real stop band where the rectangular
        // window's 30 dB were enough before: the real input's negative-frequency
        // image of the band (-22.3 to -2.7 MHz) folds onto 8.95 to 28.55 MHz at
        // 31.25 MHz, across the band itself, and its beat with the carrier, which
        // the low-pass removed at 25 MHz before, lands in the picture at 6.25 MHz
        // (measured: 11% rms difference in the demodulated baseband, all of it
        // above 5 MHz).  The band's lower edge has to stay where it was, too: the
        // EFM and the pilot below 2.7 MHz beat with the carrier at its low-level
        // end into the top of the video band (a symmetric Kaiser prototype that
        // let them through tripled the noise measured on the level-16 reference
        // line).  So the band-pass is a least-squares design with the stop bands
        // weighted by what they must reject: the image 50 dB and more, the EFM
        // region about as much as the rectangular design did (-12 dB at 2 MHz,
        // -31 at 1), the pass band flat within 0.1 dB from 3.5 to 20 MHz.
        // 43 taps at 62.5 MHz.
        const double tw = bandpass_transition / 2; // from the -6 dB edge to the flat pass band
        bandpass_filter_def = [&] {
            auto h = LeastSquaresFir::design(odd_taps_for(43, m_sample_frequency), m_sample_frequency, {
                    {-m_sample_frequency / 2, -c_band_low, LeastSquaresFir::constant(0), 10},  // the image
                    {-c_band_low, c_band_low - tw, LeastSquaresFir::constant(0), 3},          // EFM, pilot
                    {c_band_low + tw / 2, c_band_high - tw / 2, LeastSquaresFir::constant(1), 1},
                    {c_band_high + tw, m_sample_frequency / 2, LeastSquaresFir::constant(0), 3}});
            std::vector<std::complex<float>> hf(h.size());
            std::transform(h.begin(), h.end(), hf.begin(), [](auto c) { return std::complex<float>(c); });
            return hf;
        }();
    }
    std::reverse(bandpass_filter_def.begin(), bandpass_filter_def.end()); // shader correlates rather than convolves
    const int bandpass_filter_size = (int)bandpass_filter_def.size();
    static_assert(MuseDemodulatedBlock::c_video_decimation_rate == 2);
    const int lowpass_decimation = MuseDemodulatedBlock::c_video_decimation_rate / analytic_decimation;
    const float analytic_rate = m_sample_frequency / analytic_decimation;
    const int analytic_block_size = MuseDemodulatedBlock::c_sample_block_size / analytic_decimation;
    m_log.info(ePerformance, std::format("MUSE band-pass FIR: {} complex taps, decimating by {} to {:.3f} MHz",
                                         bandpass_filter_size, analytic_decimation, analytic_rate / 1e6));

    std::vector<float> bandpass_filter_re_coeffs;
    std::transform(bandpass_filter_def.cbegin(), bandpass_filter_def.cend(), back_inserter(bandpass_filter_re_coeffs),
                   [](std::complex<float> c) { return c.real(); });
    std::vector<float> bandpass_filter_im_coeffs;
    std::transform(bandpass_filter_def.cbegin(), bandpass_filter_def.cend(), back_inserter(bandpass_filter_im_coeffs),
                   [](std::complex<float> c) { return c.imag(); });

    shared_ptr<VulkanBuffer> bandpass_filter_re =
            VulkanUtil::createDeviceBuffer(m_vulkan_manager, command_pool, Size(bandpass_filter_size), bandpass_filter_re_coeffs);
    shared_ptr<VulkanBuffer> bandpass_filter_im =
            VulkanUtil::createDeviceBuffer(m_vulkan_manager, command_pool, Size(bandpass_filter_size), bandpass_filter_im_coeffs);

    // FIR lowpass filter for the demodulated signal, at the analytic rate.  With the
    // quadrature demodulation at the analytic rate its phase step per sample
    // averages the instantaneous frequency over that interval, a sinc response
    // 1 dB down at 8 MHz at 31.25 MHz (0.25 dB at 62.5, measured as a 0.9 dB loss at
    // 7-8 MHz in the demodulated baseband), so the low-pass's pass band is designed
    // to the inverse sinc: within 0.1 dB of it to 9 MHz, 26 dB down at 11.8 MHz and
    // 36 and more beyond in 23 taps at 31.25 MHz.
    constexpr double c_video_lowpass = 9.8e6;
    std::vector<float> lowpass_filter_def;
    if (analytic_decimation == 1) {
        lowpass_filter_def = WindowedSinc::low_pass<float>(WindowedSinc::rectangular_ntaps(analytic_rate, 2e6 * transition_scale),
                                                           analytic_rate, c_video_lowpass);
    } else {
        const double inverse_sinc_scale = analytic_rate;
        auto inverse_sinc = [inverse_sinc_scale](double f) {
            const double x = M_PI * std::abs(f) / inverse_sinc_scale;
            return x < 1e-9 ? 1.0 : x / std::sin(x);
        };
        const double stop = c_video_lowpass + 2e6;
        auto h = LeastSquaresFir::designReal(odd_taps_for(46, analytic_rate), analytic_rate, {
                {-c_video_lowpass, c_video_lowpass, inverse_sinc, 1},
                {-analytic_rate / 2, -stop, LeastSquaresFir::constant(0), 3},
                {stop, analytic_rate / 2, LeastSquaresFir::constant(0), 3}});
        lowpass_filter_def.assign(h.begin(), h.end());
    }
    std::reverse(lowpass_filter_def.begin(), lowpass_filter_def.end()); // symmetric, but the shader correlates
    const int lowpass_filter_size = (int)lowpass_filter_def.size();
    m_log.info(ePerformance, std::format("MUSE low-pass FIR: {} taps, decimating by {}", lowpass_filter_size, lowpass_decimation));

    shared_ptr<VulkanBuffer> lowpass_filter =
            VulkanUtil::createDeviceBuffer(m_vulkan_manager, command_pool, Size(lowpass_filter_size), lowpass_filter_def);

    // Root raised cosine filter for pulse shaping.
    // The RRC works on the video rate, half the input sample rate (e.g. 31.25 MHz at default Fs=62.5 MHz).
    auto rrc_d = RaisedCosine::rrcFilter(11, m_sample_frequency / MuseDemodulatedBlock::c_video_decimation_rate / 16.2e6, 0.1,
                                         RaisedCosine::Normalization::eUnityDcGain);
    std::vector<float> rrc_filter_def(rrc_d.cbegin(), rrc_d.cend());
    std::reverse(rrc_filter_def.begin(), rrc_filter_def.end()); // symmetric, but the shader correlates
    const int rrc_filter_size = (int)rrc_filter_def.size();

    shared_ptr<VulkanBuffer> rrc_filter =
            VulkanUtil::createDeviceBuffer(m_vulkan_manager, command_pool, Size(rrc_filter_size), rrc_filter_def);

    // Create buffers for data
    const int input_buffer_size = MuseDemodulatedBlock::c_sample_block_size + bandpass_filter_size - 1;
    const int analytic_buffer_size = analytic_block_size + 1;
    const int lowpass_in_buffer_size = analytic_block_size + lowpass_filter_size - 1;
    const int rrc_in_buffer_size = MuseDemodulatedBlock::c_video_block_size + rrc_filter_size - 1;

    // We need to delay the output of the detected dropouts as much as the rest of the filter chain delays the video signal
    const int c_dropout_delay = lowpass_filter_size / lowpass_decimation / 2 + rrc_filter_size / 2 - 1;
    const int c_dropout_buffer_size = MuseDemodulatedBlock::c_video_block_size + c_dropout_delay;

    // Two input buffers, alternating: the CPU reads and lays out block N + 1 in
    // one while the GPU works on block N in the other (see the loop), so the
    // read and the GPU chain overlap instead of taking turns.  The GPU only
    // reads them; the FIR history in front of each block is written by the CPU
    // along with the block, so the two never touch the same memory.  (As in
    // SdtvRfDemodulator.)
    const int input_history = bandpass_filter_size - 1;
    std::array<shared_ptr<VulkanBuffer>, 2> input_buffers;
    for (auto &buffer : input_buffers)
        buffer = make_shared<musevk::VulkanBuffer>(
                m_vulkan_manager, Size(input_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostWrite);
    shared_ptr<VulkanBuffer> input_buffer = input_buffers[0]; // the representative, where a fixed one is wanted

    // The CPU only writes to this mapping (the EFM path stages its input separately), but log the
    // memory properties since a non-host-cached (write-combined) mapping would make any future
    // CPU read of it very slow.
    m_log.info(ePerformance, std::format("input_buffer host mapping memory properties: {}{}",
                                         vk::to_string(input_buffer->hostMappedMemoryProperties()),
                                         (input_buffer->hostMappedMemoryProperties() & vk::MemoryPropertyFlagBits::eHostCached)
                                                 ? "" : " -- NOT host-cached: do not read from this mapping on the CPU"));

    shared_ptr<VulkanBuffer> analytic_buffer_re = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(analytic_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    shared_ptr<VulkanBuffer> analytic_buffer_im = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(analytic_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    shared_ptr<VulkanBuffer> lowpass_in_buffer = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(lowpass_in_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    shared_ptr<VulkanBuffer> rrc_in_buffer = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(rrc_in_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    shared_ptr<VulkanBuffer> dropout_buffer = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(c_dropout_buffer_size), sizeof(uint8_t), buffer_usage_flags, HostAccess::eHostNone);

    // Squared envelope of the analytic signal (written by fm_quadrature for the
    // new samples, entry 0 recycled from the previous block like the analytic
    // buffers) and its 9-sample box average, one entry per analytic sample.
    shared_ptr<VulkanBuffer> envelope_buffer = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(analytic_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    shared_ptr<VulkanBuffer> local_envelope_buffer = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(analytic_buffer_size), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    // The dropout detector's carrier reference: the envelope summed over blocks of
    // c_envelope_block samples, and the prefix sum of those, so that the mean over
    // any run of blocks is two loads.  envelope_prefix.comp scans in one workgroup
    // and is limited to c_envelope_max_blocks.
    constexpr uint32_t c_envelope_block = 256;      // BLOCK in the shaders
    constexpr uint32_t c_envelope_max_blocks = 4096; // MAX_BLOCKS in envelope_prefix.comp
    const uint32_t envelope_block_count = analytic_buffer_size / c_envelope_block;
    if (envelope_block_count > c_envelope_max_blocks)
        throw std::runtime_error("envelope_prefix.comp: too many envelope blocks for one workgroup");

    shared_ptr<VulkanBuffer> envelope_block_sums = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(envelope_block_count), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    shared_ptr<VulkanBuffer> envelope_prefix = make_unique<musevk::VulkanBuffer>(
            m_vulkan_manager, Size(envelope_block_count + 1), sizeof(float), buffer_usage_flags, HostAccess::eHostNone);

    // Create shaders.  Each FIR pipeline is specialized to its exact tap count and decimation
    // (fir_filter.comp), so the band-pass, the decimating low-pass and the root raised cosine
    // are three pipelines of the same shader.
    // As in SdtvRfDemodulator: 2 measured fastest on an Apple M2, 1 on the Apple
    // TV's A10X (measured on the NTSC chain; the same shader and similar taps
    // here), MUSELD_FIR_OUTPUTS overrides for measuring
#if defined(__APPLE__) && TARGET_OS_TV
    uint32_t c_fir_outputs_per_invocation = 1;
#else
    uint32_t c_fir_outputs_per_invocation = 2;
#endif
    if (const char *env = getenv("MUSELD_FIR_OUTPUTS"); env != nullptr && atoi(env) > 0)
        c_fir_outputs_per_invocation = atoi(env);
    auto fir_dispatch_size = [c_fir_outputs_per_invocation](uint32_t output_size) {
        return Size((output_size + c_fir_outputs_per_invocation - 1) / c_fir_outputs_per_invocation);
    };

    // The FIR shader: fir_filter.comp keeps the taps and the input window in
    // register arrays, fir_filter_direct.comp reads both straight from the
    // buffers.  The A10X cannot hold the 43-tap band-pass's arrays: the chain took
    // 19 ms per 8.4 ms block with the blocked shader and 6.4 with the direct one
    // (8.4 for the old, non-decimating chain with it), so tvOS uses the direct
    // shader; the blocked one is faster on an Apple M2.  MUSELD_FIR_SHADER names
    // either (a measurement hook) for the three FIR passes.
#if defined(__APPLE__) && TARGET_OS_TV
    std::string fir_shader = "fir_filter_direct.comp";
#else
    std::string fir_shader = "fir_filter.comp";
#endif
    if (const char *env = getenv("MUSELD_FIR_SHADER"); env != nullptr && *env) {
        fir_shader = env;
        m_log.info(ePerformance, std::format("FIR shader: {} (measurement)", fir_shader));
    }
    const bool direct_fir = fir_shader == "fir_filter_direct.comp"; // three push constants, the third an input offset
    shared_ptr<ComputeShader> bandpass_fir_shader = unique_ptr<ComputeShader>(
            new ComputeShader(m_vulkan_manager, "bandpass_fir",
                              {eBuffer, eBuffer, eBuffer}, (direct_fir ? 3 : 2) * sizeof(uint32_t),
                              VulkanUtil::loadSpirv(m_executable_dir, fir_shader),
                              fir_dispatch_size(analytic_block_size), 2,
                              {(uint32_t)bandpass_filter_size, (uint32_t)analytic_decimation, c_fir_outputs_per_invocation}));

    bandpass_fir_shader->updateBufferDescriptorsInSet(0, {bandpass_filter_re, input_buffer, analytic_buffer_re});
    bandpass_fir_shader->updateBufferDescriptorsInSet(1, {bandpass_filter_im, input_buffer, analytic_buffer_im});

    shared_ptr<ComputeShader> lowpass_fir_shader = unique_ptr<ComputeShader>(
            new ComputeShader(m_vulkan_manager, "lowpass_fir",
                              {lowpass_filter, lowpass_in_buffer, rrc_in_buffer}, (direct_fir ? 3 : 2) * sizeof(uint32_t),
                              VulkanUtil::loadSpirv(m_executable_dir, fir_shader),
                              fir_dispatch_size(MuseDemodulatedBlock::c_video_block_size), 1,
                              {(uint32_t)lowpass_filter_size, (uint32_t)lowpass_decimation, c_fir_outputs_per_invocation}));

    shared_ptr<ComputeShader> rrc_fir_shader = unique_ptr<ComputeShader>(
            new ComputeShader(m_vulkan_manager, "rrc_fir",
                              {eBuffer, eBuffer, eBuffer}, (direct_fir ? 3 : 2) * sizeof(uint32_t),
                              VulkanUtil::loadSpirv(m_executable_dir, fir_shader),
                              fir_dispatch_size(MuseDemodulatedBlock::c_video_block_size), 1,
                              {(uint32_t)rrc_filter_size, /* decimation */ 1, c_fir_outputs_per_invocation}));

    // The carrier's phase step per analytic sample, taken out of the product
    // before its phase is measured (see fm_quadrature.comp)
    const double carrier_phase_step = -2 * M_PI * c_center_frequency / analytic_rate;
    const float carrier_rotation_re = (float)cos(carrier_phase_step);
    const float carrier_rotation_im = (float)sin(carrier_phase_step);
    shared_ptr<ComputeShader> fm_quadrature_shader = unique_ptr<ComputeShader>(
            new ComputeShader(m_vulkan_manager, "fm_quadrature",
                              {analytic_buffer_re, analytic_buffer_im, lowpass_in_buffer, envelope_buffer}, 8 * sizeof(float),
                              VulkanUtil::loadSpirv(m_executable_dir, "fm_quadrature.comp"), Size(analytic_block_size)));

    constexpr uint32_t c_envelope_box_outputs = 4; // OUTPUTS in envelope_box.comp
    shared_ptr<ComputeShader> envelope_box_shader = unique_ptr<ComputeShader>(
            new ComputeShader(m_vulkan_manager, "envelope_box",
                              {envelope_buffer, local_envelope_buffer}, sizeof(uint32_t),
                              VulkanUtil::loadSpirv(m_executable_dir, "envelope_box.comp"),
                              Size((analytic_buffer_size + c_envelope_box_outputs - 1) / c_envelope_box_outputs)));

    shared_ptr<ComputeShader> envelope_block_sums_shader = unique_ptr<ComputeShader>(
            new ComputeShader(m_vulkan_manager, "envelope_block_sums",
                              {envelope_buffer, envelope_block_sums}, 2 * sizeof(uint32_t),
                              VulkanUtil::loadSpirv(m_executable_dir, "envelope_block_sums.comp"),
                              Size(envelope_block_count * c_envelope_block)));

    // exactly one workgroup: the scan runs in shared memory
    shared_ptr<ComputeShader> envelope_prefix_shader = unique_ptr<ComputeShader>(
            new ComputeShader(m_vulkan_manager, "envelope_prefix",
                              {envelope_block_sums, envelope_prefix}, sizeof(uint32_t),
                              VulkanUtil::loadSpirv(m_executable_dir, "envelope_prefix.comp"), Size(1)));

    shared_ptr<ComputeShader> detect_dropouts_shader = unique_ptr<ComputeShader>(
            new ComputeShader(m_vulkan_manager,
                              "detect_dropouts_envelope",
                              {local_envelope_buffer, envelope_prefix, dropout_buffer, lowpass_in_buffer}, 12 * sizeof(uint32_t),
                              VulkanUtil::loadSpirv(m_executable_dir, "detect_dropouts_envelope.comp"), Size(MuseDemodulatedBlock::c_video_block_size)));

    // Clear the buffers -- we start storing data a bit into the buffer, so the first filter pass
    // will have undefined output otherwise.
    command_buffer->begin();
    if (timestamp_query_pool != nullptr)
        timestamp_query_pool->reset(*command_buffer);
    float zero = 0.f;
    float one = 1.f;
    for (auto &buffer : input_buffers)
        command_buffer->enqueueFillBuffer(*buffer, 0);
    command_buffer->enqueueFillBuffer(*analytic_buffer_re, reinterpret_cast<uint32_t &>(one));
    command_buffer->enqueueFillBuffer(*analytic_buffer_im, reinterpret_cast<uint32_t &>(one));
    float two = 2.f; // the envelope of the (1, 1) analytic fill
    command_buffer->enqueueFillBuffer(*envelope_buffer, reinterpret_cast<uint32_t &>(two));
    command_buffer->enqueueFillBuffer(*lowpass_in_buffer, reinterpret_cast<uint32_t &>(zero));
    command_buffer->enqueueFillBuffer(*rrc_in_buffer, reinterpret_cast<uint32_t &>(zero));
    command_buffer->submit({}, {}, {});
    command_buffer->wait();

    // The EFM demodulation is CPU work independent of the Vulkan pipeline, so it runs on its own
    // worker thread and overlaps the next block's read/record/submit.  The demodulation loop stages
    // the raw input samples in block->efm_input and hands the block over after the GPU has finished
    // with it; the worker fills in efm_data and forwards the block to m_filled_blocks.  A single
    // worker with a FIFO queue keeps the blocks in order, and the queue is bounded by the block
    // pool: when all blocks are queued for EFM the loop waits on m_cv_vacant as before.
    // The worker's shared state and the thread live in one struct so a single
    // destructor stops and joins on every exit path: an exception below would
    // otherwise unwind past a joinable std::thread (std::terminate) while
    // destroying the queue the worker is still using.
    struct EfmWorker {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<unique_ptr<MuseDemodulatedBlock>> queue;
        bool stop = false;
        std::thread thread; // last member: joined before the state above goes away

        ~EfmWorker() {
            {
                std::unique_lock<std::mutex> lock(mutex);
                stop = true;
                cv.notify_one();
            }
            if (thread.joinable())
                thread.join();
        }
    } efm;
    efm.thread = std::thread([&]() {
#ifdef __APPLE__
        pthread_setname_np("museld-efm");
#elif defined(linux) || defined(__FreeBSD__)
        pthread_setname_np(pthread_self(), "museld-efm");
#endif
        while (true) {
            unique_ptr<MuseDemodulatedBlock> block;
            {
                std::unique_lock<std::mutex> lock(efm.mutex);
                efm.cv.wait(lock, [&] { return efm.stop || !efm.queue.empty(); });
                if (efm.queue.empty())
                    break; // stop requested and the queue is drained
                block = std::move(efm.queue.front());
                efm.queue.pop_front();
            }
            if (!block->efm_input.empty())
                m_efm_demodulator.demodulate(block->efm_input.data(), block->efm_data);
            else
                block->efm_data.clear();

            std::unique_lock<std::mutex> lock(m_demodulated_block_mutex);
            m_cv_filled.notify_one();
            m_filled_blocks.push_back(std::move(block));
        }
    });

    // Per-section CPU timing of the demodulation loop, reported every c_timing_report_blocks
    // blocks together with the real-time budget per block.  The read of the next block runs
    // while the GPU works on this one, so "gpu wait" is what remained of the GPU's time after
    // it; the GPU's own time per block is read + gpu wait when the read is the shorter.  On a
    // fifo the read time includes waiting for the capture device, so a large read share is
    // expected there.
    using timing_clock = std::chrono::steady_clock;
    constexpr int c_timing_report_blocks = 256;
    const double block_budget_ms = MuseDemodulatedBlock::c_sample_block_size / (double)m_sample_frequency * 1e3;
    double read_ms = 0, acquire_ms = 0, record_ms = 0, submit_ms = 0, efm_ms = 0, gpu_wait_ms = 0;
    int timed_blocks = 0;
    auto ms_between = [](timing_clock::time_point a, timing_clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };

    // The samples are always read into this heap buffer and copied into the mapping in one go,
    // never written to the mapping directly.  The mapping is not host-cached, and writing it is
    // only fast when whole cache lines arrive at once: a reader that interleaves its stores with
    // other work -- the FLAC readers decode a frame between every 4096 samples -- leaves the CPU's
    // few write-combining buffers flushing partial lines, which more than doubles the time a
    // compressed input spends here.  It also keeps the EFM path from reading back from the mapping.
    std::vector<float> input_staging;
    // The FIR history carried from block to block: the last input_history samples of the
    // previous block, laid out in front of the next block's samples
    std::vector<float> history_samples(input_history, 0.f);

    // Reads the next block into the staging buffer and lays it out, behind the history, in
    // the given input buffer.  False at the end of the input.
    // Measurement hook: with MUSELD_REPEAT_INPUT set, every block after the first
    // reuses the first block's samples -- the copies into the mapped buffer without
    // any input reading, which isolates the GPU chain's own time per block (with
    // --no-video --no-audio --no-sync: "gpu wait" is then all of it)
    static const bool repeat_input = getenv("MUSELD_REPEAT_INPUT") != nullptr;
    auto read_block = [&](int which, int64_t &offset, uint32_t &generation) -> bool {
        constexpr int block_size = MuseDemodulatedBlock::c_sample_block_size;
        if (repeat_input && input_staging.size() == (size_t)block_size) {
            offset += block_size;
        } else {
            input_staging.resize(block_size);
            if (!readInput(input_staging.data(), block_size, offset, generation))
                return false;
        }
        float *dst = input_buffers[which]->data<float>();
        memcpy(dst, history_samples.data(), input_history * sizeof(float));
        memcpy(dst + input_history, input_staging.data(), block_size * sizeof(float));
        memcpy(history_samples.data(), input_staging.data() + block_size - input_history,
               input_history * sizeof(float));
        return true;
    };

    // Block N is read before the GPU is given block N - 1 to work on, so the
    // read of the first block stands alone; every later one overlaps the GPU
    int current = 0;
    int64_t block_input_offset = 0;
    uint32_t block_seek_generation = 0;
    auto t_loop_start = timing_clock::now();
    bool have_block = read_block(current, block_input_offset, block_seek_generation);
    double pending_read_ms = ms_between(t_loop_start, timing_clock::now());

    while (!m_stop_request && have_block) {
        t_loop_start = timing_clock::now();
        const bool efm_enabled = m_efm_enabled;
        auto t_after_read = t_loop_start;

        // First get a free output block to write to
        unique_ptr<MuseDemodulatedBlock> block = nullptr;
        {
            std::unique_lock<std::mutex> lock(m_demodulated_block_mutex);
            if (m_input_is_fifo && m_vacant_blocks.empty() && !m_filled_blocks.empty()) {
                // discard a filled buffer -- this is better than waiting since it means the reader cannot cope anyway
                // (with all blocks in the EFM queue there is nothing to discard, so wait for the worker below)
                m_log.warn(eInput, "Discarding demodulated block due to overrun");
                m_vacant_blocks.push_back(std::move(m_filled_blocks.back()));
                m_filled_blocks.pop_back();
            }
            m_cv_vacant.wait(lock, [this] { return m_stop_request || !m_vacant_blocks.empty(); });
            if (m_stop_request) {
                m_log.info(eInput, "MuseRfDemodulator: stop requested");
                break;
            }
            block = std::move(m_vacant_blocks.front());
            m_vacant_blocks.pop_front();
        }
        auto t_after_acquire = timing_clock::now();
        // The first byte of the output lags the actual input due to three filters being applied
        // (the low-pass's taps are at the analytic rate)
        block->input_offset = block_input_offset
                              - (bandpass_filter_size + lowpass_filter_size * analytic_decimation + rrc_filter_size) / 2;
        block->seek_generation = block_seek_generation;

        // Begin Vulkan command buffer and reset the timestamp query pool if we use one
        command_buffer->begin();
        if (timestamp_query_pool != nullptr)
            timestamp_query_pool->reset(*command_buffer);

        // Make the data available on the GPU; this is necessary in case we didn't find a memory type good for both host writes and shader reads
        input_buffers[current]->synchronizeHostWrites(*command_buffer);
        // The band-pass reads this block's input buffer
        bandpass_fir_shader->updateBufferDescriptorsInSet(0, {bandpass_filter_re, input_buffers[current], analytic_buffer_re});
        bandpass_fir_shader->updateBufferDescriptorsInSet(1, {bandpass_filter_im, input_buffers[current], analytic_buffer_im});

        // Run the input signal through the bandpass filter that also converts the signal to an
        // analytic signal, decimated to the analytic rate
        if (!direct_fir) {
            command_buffer->enqueueComputeShader<uint32_t>(bandpass_fir_shader, {(uint32_t)analytic_block_size, /* out offset */ 1}, 0);
            command_buffer->enqueueComputeShader<uint32_t>(bandpass_fir_shader, {(uint32_t)analytic_block_size, /* out offset */ 1}, 1);
        } else {
            command_buffer->enqueueComputeShader<uint32_t>(bandpass_fir_shader, {(uint32_t)analytic_block_size, /* out offset */ 1, 0u}, 0);
            command_buffer->enqueueComputeShader<uint32_t>(bandpass_fir_shader, {(uint32_t)analytic_block_size, /* out offset */ 1, 0u}, 1);
        }

        // Demodulate the analytic signal, and scale to the standard MUSE range
        // +/- 1 corresponds to the white/black level, which is 128+/-112 (16, 240) in MUSE
        // With the current parameters (62.5MHz/12.5MHz/1.9MHz) the possible output swing is between -23.03 and +9.87.
        command_buffer->enqueueComputeShader<float>(fm_quadrature_shader,
                                                    {(float)analytic_block_size, (float)lowpass_filter_size - 1,
                                                     analytic_rate, c_frequency_deviation, carrier_rotation_re, carrier_rotation_im,
                                                     /* scale */ 112.f, /* add */ 128.f});

        // Lowpass filter the demodulated signal, and down-sample to the video rate (by 2, unless the band-pass did) before rrc filtering
        if (!direct_fir)
            command_buffer->enqueueComputeShader<uint32_t>(
                    lowpass_fir_shader, {MuseDemodulatedBlock::c_video_block_size, /* out offset */ (uint32_t)rrc_filter_size - 1});
        else
            command_buffer->enqueueComputeShader<uint32_t>(
                    lowpass_fir_shader, {MuseDemodulatedBlock::c_video_block_size, /* out offset */ (uint32_t)rrc_filter_size - 1, 0u});

        // Run the down-sampled signal through the root raised cosine pulse-shaping filter and store in the output block
        rrc_fir_shader->updateBufferDescriptorsInSet(0, {rrc_filter, rrc_in_buffer, block->video_data});
        if (!direct_fir)
            command_buffer->enqueueComputeShader<uint32_t>(rrc_fir_shader, {MuseDemodulatedBlock::c_video_block_size, /* out offset */ 0});
        else
            command_buffer->enqueueComputeShader<uint32_t>(rrc_fir_shader, {MuseDemodulatedBlock::c_video_block_size, /* out offset */ 0, 0u});

        // The local envelope average per raw sample (the squared envelope itself
        // comes from fm_quadrature), read by the dropout detector at five
        // positions per output sample
        command_buffer->enqueueComputeShader<uint32_t>(envelope_box_shader, {(uint32_t)analytic_buffer_size});
        command_buffer->enqueueComputeShader<uint32_t>(envelope_block_sums_shader, {(uint32_t)analytic_buffer_size, envelope_block_count});
        command_buffer->enqueueComputeShader<uint32_t>(envelope_prefix_shader, {envelope_block_count});

        // Detect dropouts from the RF envelope, against two references: the same
        // position on the neighbouring lines (brightness-matched, since the disc
        // MTF lowers the envelope at the bright end of the deviation range) and
        // the surrounding ~200 us average, strict enough for dropouts spanning
        // several lines.  A MUSE line is MUSE_TOTAL_WIDTH samples at 16.2 MHz.
        // The analytic signal is written with offset 1 into its buffers, and the
        // flags are delayed like the video to compensate the filter chain.
        //
        // The ratios are much stricter than the NTSC path's 0.49 / 0.25, and have
        // to be: MUSE's deviation is wide enough that picture content alone moves
        // the envelope a long way.  Measured on blue1-fx3-62.5MHz.ldf, the median
        // envelope power at peak white is about half that at black, and over the
        // whole disc the local envelope never fell below 0.156 of the surrounding
        // median -- all of it explained by content, since that capture has no
        // dropouts to speak of.  NTSC's 0.49 therefore sits in the middle of the
        // legal content range here, which is why the value-threshold detector this
        // replaced (and a straight port of the NTSC constants) concealed white
        // titles away as if they were dropouts.
        //
        // The gap to a real dropout is wide, so the thresholds do not have to be
        // a compromise: the dropouts around 55.5-56.1 s of makeup-muse-rf-62.5MHz
        // (a disc blemish struck once per revolution, 0.1-0.7 us each) take the
        // envelope down to 0.0036 of the surrounding median, more than an order of
        // magnitude below the 0.156 that content alone reaches.
        // The detector works at the analytic rate: the line period, the legal slew
        // and the carrier reference's span are derived from it, and its 9-sample
        // envelope box and +-16-sample slew window are what the NTSC path's are at
        // its demod rate.
        const uint32_t line_period = (uint32_t)lround(MUSE_TOTAL_WIDTH * analytic_rate / 16.2e6);
        constexpr float c_line_ratio_squared = 0.12f; // vs the neighbouring lines, with the noise corroboration
        constexpr float c_deep_ratio_squared = 0.10f; // vs the long average, confident on its own
        // The noise corroboration is in the demodulator's own units, where one
        // half-range (grey to white) is 112.  The legal sample-to-sample step
        // scales with the sample interval; MUSE carries roughly twice the video
        // bandwidth of NTSC, so it is allowed twice the step at the same rate.
        // The envelope conditions dominate: the flag count is unchanged between
        // 2 and 8 half-ranges here.  The exceedance counts (1 for the line
        // condition, 0 for the deep) keep the original single-spike /
        // envelope-only semantics, which these ratios were measured against;
        // the higher NTSC counts guard against white-on-black titles, which
        // MUSE's much stricter ratios already separate from content.
        const float slew_threshold = 4.0f * 112.f * 40e6f / analytic_rate;
        // carrier reference over ~205 us of whole envelope blocks, so a
        // maximum-length dropout cannot drag it down (50 blocks at 62.5 MHz)
        const uint32_t window_blocks = (uint32_t)lround(205e-6 * analytic_rate / c_envelope_block);
        command_buffer->enqueueComputeShader<uint32_t>(
                detect_dropouts_shader, {MuseDemodulatedBlock::c_video_block_size, 1u, (uint32_t)c_dropout_delay,
                                         (uint32_t)lowpass_decimation,
                                         line_period,
                                         std::bit_cast<uint32_t>(c_line_ratio_squared),
                                         std::bit_cast<uint32_t>(c_deep_ratio_squared),
                                         (uint32_t)lowpass_filter_size - 1, std::bit_cast<uint32_t>(slew_threshold),
                                         window_blocks, /* line_min_exceedances */ 1u, /* deep_min_exceedances */ 0u});

        // Barrier: ensure all compute shader writes are visible to the subsequent transfer operations
        command_buffer->enqueueBarrier(vk::AccessFlagBits::eShaderWrite,
                                       vk::AccessFlagBits::eTransferRead,
                                       vk::PipelineStageFlagBits::eComputeShader,
                                       vk::PipelineStageFlagBits::eTransfer);

        // Copy data from dropout detection to the output buffer before writing over the first part of the buffer below
        command_buffer->enqueueCopyBuffer(*dropout_buffer, *block->dropouts, 0, 0, MuseDemodulatedBlock::c_video_block_size * sizeof(uint8_t));

        // Ensure data can be read from the host later
        block->video_data->synchronizeForHostRead(*command_buffer);
        block->dropouts->synchronizeForHostRead(*command_buffer);

        // After filtering, we need to copy the last data from the input buffers to their start,
        // since the filter output is shorter than the input.  The same is true for the demodulation input,
        // but here only one byte needs to be copied.
        auto enqueue_float_copy = [command_buffer](VulkanBuffer &buffer, uint32_t buffer_size, uint32_t floats_to_copy) -> void {
            command_buffer->enqueueCopyBuffer(buffer, buffer, (buffer_size - floats_to_copy) * sizeof(float), 0, floats_to_copy * sizeof(float));
        };
        enqueue_float_copy(*analytic_buffer_re, analytic_buffer_size, 1);
        enqueue_float_copy(*analytic_buffer_im, analytic_buffer_size, 1);
        enqueue_float_copy(*envelope_buffer, analytic_buffer_size, 1);
        enqueue_float_copy(*lowpass_in_buffer, lowpass_in_buffer_size, lowpass_filter_size - 1);
        enqueue_float_copy(*rrc_in_buffer, rrc_in_buffer_size, rrc_filter_size - 1);
        // The tail recycle below writes the start of dropout_buffer, which the
        // copy into block->dropouts above reads; transfers on the same queue
        // have no implicit ordering, so the write needs an execution dependency
        // on the read.  (The float recycles are ordered against the shader
        // reads by the compute-to-transfer barrier already.)
        command_buffer->enqueueBarrier(vk::AccessFlagBits::eTransferRead,
                                       vk::AccessFlagBits::eTransferWrite,
                                       vk::PipelineStageFlagBits::eTransfer,
                                       vk::PipelineStageFlagBits::eTransfer);
        command_buffer->enqueueCopyBuffer(*dropout_buffer, *dropout_buffer, (c_dropout_buffer_size - c_dropout_delay) * sizeof(uint8_t), 0, c_dropout_delay * sizeof(uint8_t));

        auto t_after_record = timing_clock::now();
        command_buffer->submit({}, {}, {});
        auto t_after_submit = timing_clock::now();

        // Hand the staged input samples to the block for the EFM worker.  The swap gives the block
        // this iteration's samples and reclaims the block's old buffer for the next read, so no
        // data is copied and nothing is read back from the mapped input buffer.
        if (efm_enabled)
            std::swap(block->efm_input, input_staging);
        else
            block->efm_input.clear();
        auto t_after_efm = timing_clock::now();

        // The next block, read and laid out in the other input buffer while the GPU
        // works on this one.  The staging buffer was just reclaimed from the output
        // block by the swap above.
        const int next = 1 - current;
        int64_t next_input_offset = 0;
        uint32_t next_seek_generation = 0;
        have_block = read_block(next, next_input_offset, next_seek_generation);
        auto t_after_next_read = timing_clock::now();

        command_buffer->wait();
        auto t_after_gpu_wait = timing_clock::now();

        read_ms += pending_read_ms + ms_between(t_after_efm, t_after_next_read);
        pending_read_ms = 0;
        acquire_ms += ms_between(t_after_read, t_after_acquire);
        record_ms += ms_between(t_after_acquire, t_after_record);
        submit_ms += ms_between(t_after_record, t_after_submit);
        efm_ms += ms_between(t_after_submit, t_after_efm);
        gpu_wait_ms += ms_between(t_after_next_read, t_after_gpu_wait);
        if (++timed_blocks == c_timing_report_blocks) {
            m_log.info(ePerformance, std::format(
                    "demodulate avg/block (budget {:.2f} ms): read {:.2f} ms (overlapping the GPU), acquire {:.2f} ms, "
                    "record {:.2f} ms, submit {:.2f} ms, efm copy {:.2f} ms, gpu wait {:.2f} ms after the read, total {:.2f} ms",
                    block_budget_ms, read_ms / timed_blocks, acquire_ms / timed_blocks,
                    record_ms / timed_blocks, submit_ms / timed_blocks, efm_ms / timed_blocks,
                    gpu_wait_ms / timed_blocks,
                    (read_ms + acquire_ms + record_ms + submit_ms + efm_ms + gpu_wait_ms) / timed_blocks));
            read_ms = acquire_ms = record_ms = submit_ms = efm_ms = gpu_wait_ms = 0;
            timed_blocks = 0;
        }

        if (timestamp_query_pool != nullptr)
            timestamp_statistics.add_timestamps(timestamp_query_pool->getTimestamps());

        // Hand the block to the EFM worker, which forwards it to m_filled_blocks.  This must not
        // happen before command_buffer->wait() above, since the consumer reads video_data as soon
        // as the block appears in m_filled_blocks.
        {
            std::unique_lock<std::mutex> lock(efm.mutex);
            efm.cv.notify_one();
            efm.queue.push_back(std::move(block));
        }

        current = next;
        block_input_offset = next_input_offset;
        block_seek_generation = next_seek_generation;
    }

    // Let the EFM worker drain its queue before signalling end of stream
    {
        std::unique_lock<std::mutex> lock(efm.mutex);
        efm.stop = true;
        efm.cv.notify_one();
    }
    efm.thread.join();

    m_reader_thread_finished = true;
    m_cv_filled.notify_one();

    timestamp_statistics.print_stats(0);
}
