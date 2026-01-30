//
// Created by staffanu on 1/24/26.
//

#ifndef AC3RF_DECODE_POPDETECTOR_H
#define AC3RF_DECODE_POPDETECTOR_H

#include <vector>
#include "TwoChannelSample.h"

/*
 * Detects single outlier samples to eliminate any incorrect corrections performed by CIRC.
 */

class PopDetector {
public:
    PopDetector();
    PopDetector(const PopDetector &) = delete;
    PopDetector &operator=(const PopDetector &) = delete;
    PopDetector(PopDetector &&) = delete;
    PopDetector &operator=(PopDetector &&) = delete;

    // Detect pops and erase them.  For each sample we look back 3 samples and forward 2 samples.
    std::vector<TwoChannelSampleWithErasureFlags> processSamples(const std::vector<TwoChannelSampleWithErasureFlags> &samples);

private:
    // The number of samples on each side of the processed sample that are considered.
    static constexpr int c_samples_per_side = 3;

    // Contains at least two samples that have already been processed.  These are the first samples of the vector.
    // Any other samples were not processed yet. Typically, this means we save 4 samples between calls, since the
    // two last samples can can be processed without knowing later samples.
    std::vector<TwoChannelSampleWithErasureFlags> m_saved_samples;

    unsigned long m_processed_samples;
};


#endif //AC3RF_DECODE_POPDETECTOR_H
