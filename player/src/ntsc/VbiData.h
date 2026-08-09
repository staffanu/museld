// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

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

    std::vector<std::string> asStrings() const override;
    std::optional<double> playbackTimeSeconds() const override;
    [[nodiscard]] std::optional<bool> cxEnabled() const { return m_cx_enabled; }

    // The CX state the player applies when the user forces it (nullopt in
    // auto mode); set by the decoder so asStrings() can show detected vs used
    void setCxOverride(std::optional<bool> used) { m_cx_override = used; }

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
    std::optional<bool> m_cx_override;
};

#endif //MUSECPP_VBIDATA_H
