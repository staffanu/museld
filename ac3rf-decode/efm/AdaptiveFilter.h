//
// Created by staffanu on 11/18/25.
//

#ifndef AC3RF_DECODE_ADAPTIVEFILTER_H
#define AC3RF_DECODE_ADAPTIVEFILTER_H

class AdaptiveFilter {
public:
    virtual ~AdaptiveFilter() = default;

    [[nodiscard]] virtual int size() const = 0;
    virtual void addSample(float sample) = 0;
    virtual void adaptError(float desired, float actual) = 0;
    virtual void getLast3(float &f1, float&f2, float &f3) = 0;
    virtual std::string filterString() = 0;
};

class NonAdaptiveFilter : public AdaptiveFilter {
public:
    NonAdaptiveFilter() : m_window{} {}
    [[nodiscard]] int size() const override {
        return 0;
    }
    void addSample(float sample) override {
        for (int i = 0; i < 2; i++)
            m_window[i] = m_window[i + 1];
        m_window[2] = sample;
    }
    void adaptError(float desired, float actual) override {}
    void getLast3(float &f1, float&f2, float &f3) override {
        f1 = m_window[0];
        f2 = m_window[1];
        f3 = m_window[2];
    }
    std::string filterString() override {
        return "[non-adaptive]";
    }

private:
    float m_window[3];
};

#endif //AC3RF_DECODE_ADAPTIVEFILTER_H