// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include "InputController.h"

#include <algorithm>
#include <format>

#include <GLFW/glfw3.h>

#include "PlayerState.h"
#include "logging/Logger.h"

bool InputController::checkKey(GLFWwindow *window, int key) {
    if (glfwGetKey(window, key) == GLFW_PRESS) {
        if (m_keys_down.find(key) == m_keys_down.end()) {
            m_keys_down.insert(key);
            return true;
        }
        return false;
    }
    m_keys_down.erase(key);
    return false;
}

bool InputController::poll(GLFWwindow *window,
                           PlayerState &state,
                           ReaderControls &reader,
                           DropoutMode &dropout_mode,
                           bool &efm_audio,
                           bool &full_screen,
                           int window_width,
                           int window_height) {
    glfwPollEvents();

    if (checkKey(window, GLFW_KEY_ESCAPE) || checkKey(window, GLFW_KEY_Q))
        return false;
    if (checkKey(window, GLFW_KEY_TAB)) {
        if (full_screen) {
            glfwSetWindowMonitor(window, nullptr, 0, 0, window_width, window_height, 60);
            full_screen = false;
        } else {
            glfwSetWindowMonitor(window, glfwGetPrimaryMonitor(), 0, 0, window_width, window_height, 60);
            full_screen = true;
        }
    }
    if (checkKey(window, GLFW_KEY_SPACE)) {
        state.paused = !state.paused;
        state.osd_text = state.paused ? "PAUSE" : "PLAY";
    }
    if (checkKey(window, GLFW_KEY_N)) {
        state.paused = false;
        state.redo_last_field = false;
        state.paused_countdown = 1;
    }

    const double zoom_step = 0.2;
    if (checkKey(window, GLFW_KEY_LEFT)) {
        if (state.zoom_factor != 1) {
            state.zoom_center.first = std::max(0.5 / state.zoom_factor,
                                               state.zoom_center.first - zoom_step / state.zoom_factor);
        } else {
            reader.seek(-10);
            if (state.paused) {
                state.paused = false;
                state.paused_countdown = 5;
            }
        }
    }
    if (checkKey(window, GLFW_KEY_RIGHT)) {
        if (state.zoom_factor != 1) {
            state.zoom_center.first = std::min(1.0 - 0.5 / state.zoom_factor,
                                               state.zoom_center.first + zoom_step / state.zoom_factor);
        } else {
            reader.seek(10);
            if (state.paused) {
                state.paused = false;
                state.paused_countdown = 5;
            }
        }
    }
    if (checkKey(window, GLFW_KEY_UP)) {
        state.zoom_center.second = std::max(0.5 / state.zoom_factor,
                                            state.zoom_center.second - zoom_step / state.zoom_factor);
    }
    if (checkKey(window, GLFW_KEY_DOWN)) {
        state.zoom_center.second = std::min(1.0 - 0.5 / state.zoom_factor,
                                            state.zoom_center.second + zoom_step / state.zoom_factor);
    }

    if (checkKey(window, GLFW_KEY_1)) {
        state.field_interpolation_mode = Decoder::FieldInterpolationMode::eNormal;
        if (state.paused) state.redo_last_field = true;
        m_log.info(eApplication | eVideo, "Field interpolation determined by motion detection");
        state.osd_text = "MOTION NORMAL";
    }
    if (checkKey(window, GLFW_KEY_2)) {
        state.field_interpolation_mode = Decoder::FieldInterpolationMode::eForceIntraField;
        if (state.paused) state.redo_last_field = true;
        m_log.info(eApplication | eVideo, "Field interpolation forced to intra field only");
        state.osd_text = "MOTION ALL";
    }
    if (checkKey(window, GLFW_KEY_3)) {
        state.field_interpolation_mode = Decoder::FieldInterpolationMode::eForceInterFrame;
        if (state.paused) state.redo_last_field = true;
        m_log.info(eApplication | eVideo, "Inter-frame interpolation forced");
        state.osd_text = "MOTION NONE";
    }
    if (checkKey(window, GLFW_KEY_4)) {
        state.use_3d_comb = !state.use_3d_comb;
        if (state.paused) state.redo_last_field = true;
        m_log.info(eApplication | eVideo, state.use_3d_comb ? "3D comb enabled" : "3D comb disabled");
        state.osd_text = state.use_3d_comb ? "3D COMB ON" : "3D COMB OFF";
    }
    if (checkKey(window, GLFW_KEY_5)) {
        state.film_mode = !state.film_mode;
        if (state.paused) state.redo_last_field = true;
        m_log.info(eApplication | eVideo, state.film_mode ? "Film mode auto" : "Film mode off");
        state.osd_text = state.film_mode ? "FILM MODE AUTO" : "FILM MODE OFF";
    }
    if (checkKey(window, GLFW_KEY_A)) {
        efm_audio = !efm_audio;
        reader.setEfmEnabled(efm_audio);
        state.osd_text = efm_audio ? "EFM AUDIO" : "MUSE AUDIO";
    }
    if (checkKey(window, GLFW_KEY_D)) {
        switch (dropout_mode) {
            case DropoutMode::eNormal:
                dropout_mode = DropoutMode::eDisabled;
                state.osd_text = "DROPOUT DISABLED";
                break;
            case DropoutMode::eDisabled:
                dropout_mode = DropoutMode::eHighlight;
                state.osd_text = "DROPOUT HIGHLIGHT";
                break;
            case DropoutMode::eHighlight:
                dropout_mode = DropoutMode::eNormal;
                state.osd_text = "DROPOUT ENABLED";
                break;
        }
    }
    if (checkKey(window, GLFW_KEY_L)) {
        state.enable_non_linear = !state.enable_non_linear;
        state.osd_text = state.enable_non_linear ? "NON-LINEAR DE-EMPH ON" : "NON-LINEAR DE-EMPH OFF";
    }
    if (checkKey(window, GLFW_KEY_C)) {
        state.enable_cursor = !state.enable_cursor;
        glfwSetInputMode(window, GLFW_CURSOR, state.enable_cursor ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_HIDDEN);
    }
    if (checkKey(window, GLFW_KEY_V)) {
        state.show_disc_code = !state.show_disc_code;
    }
    if (checkKey(window, GLFW_KEY_PRINT_SCREEN)) {
        glfwSetClipboardString(window, state.last_cursor_string.c_str());
    }
    if (checkKey(window, GLFW_KEY_S)) {
        state.export_frame = true;
        if (state.paused)
            state.redo_last_field = true; // re-decode so baked-in OSD text is not exported
    }
    if (checkKey(window, GLFW_KEY_7) && reader.cycleEqMode) {
        std::string mode = reader.cycleEqMode();
        state.osd_text = std::format("EQ {}", mode);
        m_log.info(eApplication | eVideo, std::format("Adaptive equaliser mode: {}", mode));
    }
    if (checkKey(window, GLFW_KEY_8) && reader.resetEqTaps) {
        reader.resetEqTaps();
        state.osd_text = "EQ RESET";
        m_log.info(eApplication | eVideo, "Adaptive equaliser taps reset to identity");
    }
    if (checkKey(window, GLFW_KEY_Z)) {
        state.zoom_factor = (state.zoom_factor * 2) % 7;
        state.zoom_center.first = std::max(0.5 / state.zoom_factor,
                                           std::min(1.0 - 0.5 / state.zoom_factor, state.zoom_center.first));
        state.zoom_center.second = std::max(0.5 / state.zoom_factor,
                                            std::min(1.0 - 0.5 / state.zoom_factor, state.zoom_center.second));
        state.osd_text = std::format("ZOOM {}", state.zoom_factor);
    }
    return true;
}
