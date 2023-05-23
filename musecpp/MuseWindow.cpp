//
// Created by staffanu on 5/21/23.
//

#include "MuseWindow.h"

MuseWindow::MuseWindow(int width, int height, MuseDecoder &museDecoder)
: m_probablyInFullScreen(false),
  m_box(Gtk::ORIENTATION_VERTICAL),
  m_drawing_area(museDecoder) {
    this->set_default_size(width, height);
    m_drawing_area.show();

    m_box.pack_start(m_drawing_area, Gtk::PACK_EXPAND_WIDGET);
    this->add(m_box);
    m_box.show();
    add_events(Gdk::KEY_PRESS_MASK);
}

bool MuseWindow::on_key_press_event(GdkEventKey* event) {
    switch (event->keyval) {
        case GDK_KEY_C:
        case GDK_KEY_c:
            if ((event->state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK) {
                get_application()->quit();
            }
            return true;

        case GDK_KEY_F:
        case GDK_KEY_f:
            if (m_probablyInFullScreen) {
                unfullscreen();
                m_probablyInFullScreen = false;
            } else {
                fullscreen();
                m_probablyInFullScreen = true;
            }
            return true;

        case GDK_KEY_Escape:
            unfullscreen();
            m_probablyInFullScreen = false;
            return true;
    }

    return false;
}
