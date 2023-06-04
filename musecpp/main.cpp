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
        auto decoder = MuseDecoder(filename, shaders, app);
        decoder.Initialize();

        do {
            if (!decoder.Next())
                break;
        } while (app.drawNextFrame(*shaders.getResultBuffer(), app));
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
