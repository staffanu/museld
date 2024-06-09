//
// Created by staffanu on 6/9/24.
//

#include <cmath>
#include <stdexcept>
#include <format>
#include "MuseSignalGenerator.h"

MuseSignalGenerator::MuseSignalGenerator(std::function<std::array<std::array<float, 480>, 1125> const *()> const &get_next_frame_callback) :
        m_dev(nullptr),
        m_get_next_frame_callback(get_next_frame_callback),
        m_next_input_to_send(c_input_buffer_size),
        m_input_buffer(nullptr),
        m_muse_tx_buffer(nullptr) {

    m_input_buffer = (uint8_t *)malloc(c_input_buffer_size);
    if (!m_input_buffer)
        throw std::runtime_error("malloc error!");

    m_muse_tx_buffer = (uint8_t *)malloc(FL2K_BUF_LEN);
    if (!m_muse_tx_buffer)
        throw std::runtime_error("malloc error!");

    m_sync_tx_buffer = (uint8_t *)malloc(FL2K_BUF_LEN);
    if (!m_sync_tx_buffer)
        throw std::runtime_error("malloc error!");
}

void MuseSignalGenerator::start() {
    uint32_t dev_index = 0;
    fl2k_open(&m_dev, dev_index);
    if (!m_dev)
        throw std::runtime_error(std::format("Failed to open fl2k device #%d.", dev_index));

    auto fl2k_callback = [](fl2k_data_info_t *data_info) {
        ((MuseSignalGenerator *)data_info->ctx)->instance_fl2k_callback(data_info);
    };
    int r = fl2k_start_tx(m_dev, fl2k_callback, this, 0);
    if (r < 0)
        throw std::runtime_error("Couldn't start the transmission.");

    r = fl2k_set_sample_rate(m_dev, c_muse_samp_rate * c_oversample_factor);
    if (r < 0)
        throw std::runtime_error("Failed to set sample rate.");

    uint32_t actual_sample_rate = fl2k_get_sample_rate(m_dev);
    printf("Actual sample rate is %d\n", actual_sample_rate);
}

MuseSignalGenerator::~MuseSignalGenerator() {
    fl2k_close(m_dev);
    free(m_muse_tx_buffer);
    free(m_input_buffer);
}

void MuseSignalGenerator::instance_fl2k_callback(fl2k_data_info_t *data_info) {
    if (data_info->device_error)
        throw std::runtime_error("Device error, exiting.");

    // input is floating point 0-255 muse range.
    // output is 0-255 uint8 meaning 0-0.7 volts from the FL2K.
    auto muse2byte = [](float v) {
        float muse_voltage_range = 0.436;
        return (uint8_t)((v - 128.0f) * muse_voltage_range / 0.7 + 128);
    };

    data_info->sampletype_signed = 0;

    int bytes_written = 0;
    int sync_samples = 0;
    while (bytes_written < FL2K_BUF_LEN) {
        if (m_next_input_to_send == c_input_buffer_size) {
            int count = 0;
            auto data = m_get_next_frame_callback();
            for (auto &line : *data)
                for (auto pixel : line)
                    m_input_buffer[count++] = muse2byte(pixel);
            m_next_input_to_send = 0;
            sync_samples = c_oversample_factor * 480; // keep high for one line
        }
        int input_samples_to_copy = std::min(c_input_buffer_size - m_next_input_to_send, (FL2K_BUF_LEN - bytes_written) / c_oversample_factor);
        for (int i = 0; i < input_samples_to_copy; i++) {
            for (int j = 0; j < c_oversample_factor; j++) {
                m_muse_tx_buffer[bytes_written] = m_input_buffer[m_next_input_to_send];
                m_sync_tx_buffer[bytes_written] = sync_samples ? 255 : 0;
                if (sync_samples)
                    sync_samples--;
                bytes_written++;
            }
            m_next_input_to_send++;
        }
    }

    data_info->g_buf = (char *)m_muse_tx_buffer;
    data_info->b_buf = (char *)m_sync_tx_buffer;
}
