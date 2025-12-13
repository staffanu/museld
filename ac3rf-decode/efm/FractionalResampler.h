//
// Created by staffanu on 11/15/25.
//

#ifndef AC3RF_DECODE_FRACTIONALRESAMPLER_H
#define AC3RF_DECODE_FRACTIONALRESAMPLER_H

class FractionalResampler {
public:
    explicit FractionalResampler(int input_block_size);

    FractionalResampler(const FractionalResampler &) = delete;
    FractionalResampler &operator=(const FractionalResampler &) = delete;
    FractionalResampler(FractionalResampler &&) = delete;
    FractionalResampler &operator=(FractionalResampler &&) = delete;

    void updateInput(const float *input);
    bool advanceTimeAndResample(double dt, float &output);
    [[nodiscard]] size_t get_index() const;

private:
    static constexpr int c_max_look_back = 3;

    int m_input_block_size;
    const float *m_buffer;
    float m_prev_data[c_max_look_back];
    double m_t;
    int m_block_count;
};

#endif //AC3RF_DECODE_FRACTIONALRESAMPLER_H
