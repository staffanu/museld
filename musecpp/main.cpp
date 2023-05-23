#include <iostream>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <deque>
#include <gtkmm.h>
#include "MuseWindow.h"
#include "Shaders.h"
#include "MuseDecoder.h"

using namespace std;

void process_file(string_view filename, bool read_floats, bool use_gtk) {

    Shaders::CreateInstance();
    auto decoder = MuseDecoder(filename, read_floats);
    decoder.Initialize();

    if (use_gtk) {
        auto gtkApp = Gtk::Application::create("se.musecpp");
        MuseWindow museWindow(3 * MUSE_Y_BUF_WIDTH, 2 * MUSE_BUF_HEIGHT, decoder);
        gtkApp->run(museWindow);
    } else {
        optional<cv::Mat> optMat;
        while ((optMat = decoder.Next(false)).has_value()) {
            cv::imshow("MUSE", optMat.value());
            cv::waitKey(1);
        }
    }
}

int main(int argc, char *argv[]) {
    try {
        bool read_floats = false;
        bool use_gtk = false;
        const vector<string_view> args(argv + 1, argv + argc);
        for (auto it = args.cbegin(), end = args.cend(); it != end; ++it) {
            if (*it == "-f")
                read_floats = true;
            else if (*it == "-s")
                read_floats = false;
            else if (*it == "-g")
                use_gtk = true;
            else {
                if (!filesystem::exists(*it))
                    throw runtime_error("File not found: " + string(*it));
                process_file(*it, read_floats, use_gtk);
            }
        }
    } catch (const exception &x) {
        cerr << "musecpp: " << x.what() << '\n';
        cerr << "usage: musecpp [-f] [-s] <input_file> ...\n";
        return EXIT_FAILURE;
    }

    return 0;
}
