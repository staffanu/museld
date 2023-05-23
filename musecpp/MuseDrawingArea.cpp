//
// Created by staffanu on 5/21/23.
//

#include "opencv2/imgproc.hpp"
#include "opencv2/core.hpp"
#include "MuseDrawingArea.h"

MuseDrawingArea::MuseDrawingArea(MuseDecoder &museDecoder)
: m_museDecoder(museDecoder) {
    m_every_now_and_then_connection =
            Glib::signal_timeout().connect(sigc::mem_fun(*this, &MuseDrawingArea::everyNowAndThen), 1);
}

MuseDrawingArea::~MuseDrawingArea() {
    m_every_now_and_then_connection.disconnect();
}

bool MuseDrawingArea::everyNowAndThen() {
    auto win = get_window();
    if (win) {
        Gdk::Rectangle r(0, 0, m_width, m_height);
        win->invalidate_rect(r, false);
    }
    return true;
}

void MuseDrawingArea::on_size_allocate (Gtk::Allocation& allocation) {
    DrawingArea::on_size_allocate(allocation);
    m_width = allocation.get_width();
    m_height = allocation.get_height();
}

bool MuseDrawingArea::on_draw(const Cairo::RefPtr<Cairo::Context>& cr) {
    if (m_width == 0 || m_height == 0)
        return true;

    auto decoded_opt = m_museDecoder.Next(true);
    if (!decoded_opt.has_value()) {
        return true; // might want to communicate this to the window
    }
    cv::Mat resized;
    resize(decoded_opt.value(), resized, cv::Size(m_width, m_height), 0, 0, cv::INTER_LINEAR);

    Glib::RefPtr<Gdk::Pixbuf> pixbuf =
            Gdk::Pixbuf::create_from_data((guint8*)resized.data,
                                          Gdk::Colorspace::COLORSPACE_RGB,
                                          true,
                                          8,
                                          resized.cols,
                                          resized.rows,
                                          (int)resized.step);
    Gdk::Cairo::set_source_pixbuf(cr, pixbuf);
    cr->paint();
    return true;
}
