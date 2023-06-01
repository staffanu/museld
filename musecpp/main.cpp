#include <iostream>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <deque>
#include "Shaders.h"
#include "MuseDecoder.h"
#include "VulkanManager.h"

using namespace std;

void process_file(string filename, bool read_floats) {
    VulkanManager app;
    app.InitWindow();
    app.InitVulkan();
    {
        auto resources = app.resources();
        Shaders shaders(resources);
        auto decoder = MuseDecoder(filename, read_floats, shaders);
        decoder.Initialize();

        do {
            if (!decoder.Next())
                break;
        } while (app.DrawNextFrame(*shaders.result_tensor()));
    }
    app.Cleanup();
}

int main(int argc, char *argv[]) {
    try {
        bool read_floats = false;
        const vector<string> args(argv + 1, argv + argc);
        for (auto it = args.cbegin(), end = args.cend(); it != end; ++it) {
            if (*it == "-f")
                read_floats = true;
            else if (*it == "-s")
                read_floats = false;
            else {
                if (!filesystem::exists(*it))
                    throw runtime_error("File not found: " + string(*it));
                process_file(*it, read_floats);
            }
        }
    } catch (const exception &x) {
        cerr << "musecpp: " << x.what() << '\n';
        cerr << "usage: musecpp [-f] [-s] <input_file> ...\n";
        return EXIT_FAILURE;
    }

    return 0;
}
