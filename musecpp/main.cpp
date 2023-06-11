#include <iostream>
#include <filesystem>
#include "Shaders.h"
#include "MuseDecoder.h"
#include "musevk/VulkanManager.h"
#include "MuseTypes.h"

using namespace std;

void process_file(string filename) {
    musevk::VulkanManager app;
    app.initWindow(MUSE_Y_BUF_WIDTH * 3, MUSE_BUF_HEIGHT * 2);
    app.initVulkan();
    {
        Shaders shaders(app);
        auto decoder = MuseDecoder(filename, shaders, app, false);
        decoder.Initialize();
        auto t0 = chrono::high_resolution_clock::now();
        int field_count = 0;
        do {
            if (!decoder.Next())
                break;
            field_count++;
        } while (app.drawNextFrame(*shaders.getResultImage()));
        auto t1 = chrono::high_resolution_clock::now();
        long time_us = chrono::duration_cast<chrono::microseconds>(t1 - t0).count();
        cout << "Avg " << setprecision(3) << (time_us / 1000.0 / field_count) << " ms/field"
            << " (" << setprecision(3) << 1000000.0 / time_us * field_count << " fields/s)" << endl;

    }
    app.cleanup();
}

int main(int argc, char *argv[]) {
    try {
        const vector<string> args(argv + 1, argv + argc);
        for (auto it = args.cbegin(), end = args.cend(); it != end; ++it) {
            if (!filesystem::exists(*it))
                throw runtime_error("File not found: " + string(*it));
            process_file(*it);
        }
    } catch (const exception &x) {
        cerr << "musecpp: " << x.what() << '\n';
        cerr << "usage: musecpp [-f] [-s] <input_file> ...\n";
        return EXIT_FAILURE;
    }

    return 0;
}
