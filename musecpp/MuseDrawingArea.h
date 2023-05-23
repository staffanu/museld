//
// Created by staffanu on 5/21/23.
//

#ifndef MUSECPP_MUSEDRAWINGAREA_H
#define MUSECPP_MUSEDRAWINGAREA_H

#include <opencv2/highgui.hpp>
#include <gtkmm.h>
#include "MuseDecoder.h"

class MuseDrawingArea : public Gtk::DrawingArea {
public:
    MuseDrawingArea(MuseDecoder &museDecoder);
    ~MuseDrawingArea() override;

protected:
    bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr) override;
    void on_size_allocate (Gtk::Allocation& allocation) override;

    bool everyNowAndThen();

private:
    MuseDecoder &m_museDecoder;
    sigc::connection m_every_now_and_then_connection;
    int m_width;
    int m_height;
};

#endif //MUSECPP_MUSEDRAWINGAREA_H
