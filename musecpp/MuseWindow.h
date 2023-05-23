//
// Created by staffanu on 5/21/23.
//

#ifndef MUSECPP_MUSEWINDOW_H
#define MUSECPP_MUSEWINDOW_H

#include <gtkmm.h>
#include "MuseDrawingArea.h"
#include "MuseDecoder.h"

class MuseWindow : public Gtk::Window {
public:
    MuseWindow(int width, int height, MuseDecoder &museDecoder);

protected:
    bool on_key_press_event(GdkEventKey* event) override;

private:
    bool m_probablyInFullScreen;
    Gtk::Box m_box;
    MuseDrawingArea m_drawing_area;
};

#endif //MUSECPP_MUSEWINDOW_H
