//
// Created by staffanu on 6/22/24.
//

#include "DiscCode.h"
#include <format>

DiscCode::DiscCode(int mode, int cadr, int fadr1, int fadr2) :
        DiscInfo(),
        m_mode(mode),
        m_cadr(cadr),
        m_fadr1(fadr1),
        m_fadr2(fadr2) {
}

std::vector<std::string> DiscCode::asStrings() const {
    std::string disc_code_string1 =
        std::format("{}{} {}", pf() ? "TOC " : "", sz() ? "20 cm" : "30 cm", df() ? "CLV" : "CAV");
    std::string disc_code_string2 = std::format("Chapter {} Frame {}", chapter(), frame());

    return {disc_code_string1, disc_code_string2};
}
