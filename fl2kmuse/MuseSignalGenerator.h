//
// Created by staffanu on 6/9/24.
//

#ifndef FL2KMUSE_MUSESIGNALGENERATOR_H
#define FL2KMUSE_MUSESIGNALGENERATOR_H

#include <osmo-fl2k.h>
#include <functional>

class MuseSignalGenerator {
public:
    explicit MuseSignalGenerator(std::function<std::array<std::array<float, 480>, 1125> const *()> const &get_next_frame_callback);
    ~MuseSignalGenerator();

    void start();

private:
    static constexpr int c_input_buffer_size = 480 * 1125;
    static constexpr int c_muse_samp_rate = 16200000;
    static constexpr int c_oversample_factor = 1;

    void instance_fl2k_callback(fl2k_data_info_t *data_info);

    fl2k_dev_t *m_dev;
    std::function<std::array<std::array<float, 480>, 1125> const *()> const &m_get_next_frame_callback;
    int m_next_input_to_send;
    uint8_t *m_input_buffer;
    uint8_t *m_muse_tx_buffer; // transmitted in G
    uint8_t *m_sync_tx_buffer; // for oscilloscope sync, on B

    std::function<int const *(int)> x;
};

#endif //FL2KMUSE_MUSESIGNALGENERATOR_H
