//
// Created by staffanu on 11/15/25.
//

#ifndef AC3RF_DECODE_ADAPTIVEFILTER_H
#define AC3RF_DECODE_ADAPTIVEFILTER_H

#include <cassert>
#include <cstdio>

class AdaptiveFilter {
public:
    explicit AdaptiveFilter(int filter_size, float mu)
    : m_filter_size(filter_size),
      m_mu(mu),
      window{},
      filter{},
      filtered{}
    {
        assert(filter_size >= 3);
        window = new float[filter_size];
        filter = new float[filter_size];
        filtered = new float[filter_size];
        
        for (int i = 0; i < filter_size; i++)
            filter[i] = 0.f;
        filter[filter_size / 2] = 1.0;
    }

    AdaptiveFilter(const AdaptiveFilter &) = delete;
    AdaptiveFilter &operator=(const AdaptiveFilter &) = delete;
    AdaptiveFilter(AdaptiveFilter &&) = delete;
    AdaptiveFilter &operator=(AdaptiveFilter &&) = delete;

    ~AdaptiveFilter() {
        delete[] window;
        delete[] filter;
        delete[] filtered;
    }

    void add_sample(float sample) {
        for (int i = 0; i < m_filter_size - 1; i++)
            window[i] = window[i + 1];
        window[m_filter_size - 1] = sample;

        for (int i = 0; i < m_filter_size - 1; i++)
            filtered[i] = filtered[i + 1];
        float s = 0.f;
        for (int i = 0; i < m_filter_size; i++)
            s += filter[i] * window[i];
        filtered[m_filter_size - 1] = s;

        // printf("Added %f, last filtered now %f\n", sample, s);
    }

    void adaptError(float e) {
        for (int i = 0; i < m_filter_size; i++)
            filter[i] += window[i] * e * m_mu;
    }

    void getLast3(float &f1, float&f2, float &f3) {
        f1 = filtered[m_filter_size - 3];
        f2 = filtered[m_filter_size - 2];
        f3 = filtered[m_filter_size - 1];
    }

    void printFilter() {
        for (int i = 0; i < m_filter_size; i++)
            printf("%f ", filter[i]);
        printf("\n");
    }

private:
    const int m_filter_size;
    const float m_mu;
    float *window; // raw samples -- newest last
    float *filter;
    float *filtered; // filtered samples -- newest last
};

#endif //AC3RF_DECODE_ADAPTIVEFILTER_H
