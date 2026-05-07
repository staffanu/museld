#ifndef MUSECPP_INPUTCONTROLLER_H
#define MUSECPP_INPUTCONTROLLER_H

#include <functional>
#include <set>

#include "DropoutMode.h"

struct GLFWwindow;
struct PlayerState;
class Logger;

struct ReaderControls {
    std::function<void(double)> seek;
    std::function<void(bool)> setEfmEnabled;
};

class InputController {
public:
    explicit InputController(Logger &log) : m_log(log) {}

    // Returns false if the user requested quit.
    bool poll(GLFWwindow *window,
              PlayerState &state,
              ReaderControls &reader,
              DropoutMode &dropout_mode,
              bool &efm_audio,
              bool &full_screen,
              int window_width,
              int window_height);

private:
    bool checkKey(GLFWwindow *window, int key);

    Logger &m_log;
    std::set<int> m_keys_down;
};

#endif //MUSECPP_INPUTCONTROLLER_H
