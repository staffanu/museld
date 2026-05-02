//
// Created by staffanu on 6/22/24.
//

#ifndef MUSECPP_VBIDATA_H
#define MUSECPP_VBIDATA_H

#include <optional>
#include <vector>
#include <string>
#include "DiscInfo.h"

class VbiData : public DiscInfo {
public:
    VbiData(bool is_lead_in, bool is_lead_out, bool is_clv, bool is_stop_code,
    std::optional<int> chapter, std::optional<int> clv_time_seconds,
    std::optional<int> clv_picture_number,
    std::optional<int> cav_picture_number,
    std::optional<bool> cx_enabled);

    std::vector<std::string> asStrings() const;

private:
    bool m_is_lead_in;
    bool m_is_lead_out;
    bool m_is_clv;
    bool m_is_stop_code;
    std::optional<int> m_chapter;
    std::optional<int> m_clv_time_seconds;
    std::optional<int> m_clv_picture_number;
    std::optional<int> m_cav_picture_number;
    std::optional<bool> m_cx_enabled;
};

#endif //MUSECPP_VBIDATA_H
