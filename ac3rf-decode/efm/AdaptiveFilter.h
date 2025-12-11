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
    [[nodiscard]] virtual float getOutput() const = 0;
    [[nodiscard]] virtual float calcCenter() const = 0;
    [[nodiscard]] virtual std::string filterString() const = 0;
};

class NonAdaptiveFilter : public AdaptiveFilter {
public:
    NonAdaptiveFilter() : m_sample{} {}
    [[nodiscard]] int size() const override {
        return 0;
    }
    void addSample(float sample) override {
        m_sample = sample;
    }
    void adaptError(float desired, float actual) override {}
    [[nodiscard]] float getOutput() const override {
        return m_sample;
    }
    [[nodiscard]] float calcCenter() const override {
        return 0.f;
    }
    [[nodiscard]] std::string filterString() const override {
        return "[non-adaptive]";
    }

private:
    float m_sample;
};

#endif //AC3RF_DECODE_ADAPTIVEFILTER_H