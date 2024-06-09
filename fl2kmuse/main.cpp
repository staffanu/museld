#include <csignal>
#include <cmath>
#include <cassert>
#include <fcntl.h>
#include "MuseSignalGenerator.h"

#if false
float low_vits_value = 16;
float high_vits_value = 239;
float low_ctrl_value = 16;
float high_ctrl_value = 239;
#else
float low_vits_value = 0;
float high_vits_value = 255;
float low_ctrl_value = 0;
float high_ctrl_value = 255;
#endif
float low_hd_value = 64;
float high_hd_value = 192;

std::vector<std::array<std::array<float, 480>, 1125>> frame_data;

static int next_frame_no = 0;

std::array<std::array<float, 480>, 1125> *get_next_frame_callback() {
    auto v = &frame_data[next_frame_no++];
    if (next_frame_no == frame_data.size())
        next_frame_no = 0;
    return v;
}

#define MUSE_BUF_HEIGHT 516
#define MUSE_Y_BUF_WIDTH 374
#define MUSE_C_BUF_WIDTH 94

// These are all gamma corrected values range [0, 1]
struct VideoPicture {
    std::array<std::array<double, MUSE_Y_BUF_WIDTH * 3>, MUSE_BUF_HEIGHT * 2> red;
    std::array<std::array<double, MUSE_Y_BUF_WIDTH * 3>, MUSE_BUF_HEIGHT * 2> green;
    std::array<std::array<double, MUSE_Y_BUF_WIDTH * 3>, MUSE_BUF_HEIGHT * 2> blue;
};

void fill_y_area(std::array<std::array<float, 480>, 1125> &frame, VideoPicture const &picture, int field_index,
                 int field_subsampling_phase, int frame_subsampling_phase) {
    int first_line = field_index ? 609 : 47;
    for (int line = first_line; line < first_line + MUSE_BUF_HEIGHT; line++) {
        auto &source_r_line = picture.red[(line - first_line) * 2 + 1 - field_index];
        auto &source_g_line = picture.green[(line - first_line) * 2 + 1 - field_index];
        auto &source_b_line = picture.blue[(line - first_line) * 2 + 1 - field_index];

        for (int pixel = 107; pixel <= 480; pixel++) {
            int phase = (frame_subsampling_phase + line + 1) % 2;
            double r = source_r_line[(pixel - 107) * 3 + phase]; // TODO: over simplification: we should down convert the sample rate first, use field subsampling etc.
            double g = source_g_line[(pixel - 107) * 3 + phase];
            double b = source_b_line[(pixel - 107) * 3 + phase];

            const double A = 0.541579;
            const double B = -0.354155;
            const double C = -0.055193;

            double r_pseudo_linear = A * pow(r - B, 2.2) + C;
            double g_pseudo_linear = A * pow(g - B, 2.2) + C;
            double b_pseudo_linear = A * pow(b - B, 2.2) + C;

            double YM = 0.588 * g_pseudo_linear + 0.118 * b_pseudo_linear + 0.294 * r_pseudo_linear;
            double YM_after_transmission_gamma = sqrt((15 * YM + 1) / 9) - 1.0/3;
            assert(YM_after_transmission_gamma >= 0);
            assert(YM_after_transmission_gamma <= 1.001);

            frame[line - 1][pixel - 1] = (float)(YM_after_transmission_gamma * (239 - 16) + 16);
        }
    }

    // TODO: equalization and non-linear processing
}

void fill_c_area(std::array<std::array<float, 480>, 1125> &frame, VideoPicture const &picture, int field_index, int subsampling_phase) {
    int first_line = field_index ? 605 : 43;
    for (int line = first_line; line < first_line + MUSE_BUF_HEIGHT; line++)
        for (int pixel = 12; pixel <= 105; pixel++)
            frame[line - 1][pixel - 1] = 128; // All b/w for now
}

void fill_control_signal(std::array<std::array<float, 480>, 1125> &frame, int field_index,
                         int field_subsampling_phase_y, int frame_subsampling_phase_y, int subsampling_phase_c) {
    std::vector<int> bits = {field_subsampling_phase_y,
                    0, 0, 0, 0, 0, 0, 0, // motion vectors
                    frame_subsampling_phase_y,
                    subsampling_phase_c,
                    0, 0, // Control of noise reducer
                    0, // equalization in progress
                    0, // motion sensitivity
                    0, // inhibit two-frame motion detection
                    0, 0, 0, // motion information,
                    0, // receiving mode control
                    0, // modulation mode
                    0, 0, 0, // extension bits
                    0, // flag for still picture
                    0, 0, 0, 0, 0, 0, 0, 0 // extension bits
                    };

    std::vector<std::vector<int>> groups;
    for (int i = 0; i < 8; i++) {
        std::vector<int> group;
        std::copy(bits.cbegin() + 4 * i, bits.cbegin() + 4 * i + 4, std::back_inserter(group));
        group.push_back(group[0] ^ group[1] ^ group[2]); // ecc bits
        group.push_back(group[1] ^ group[2] ^ group[3]);
        group.push_back(group[0] ^ group[1] ^ group[3]);
        group.push_back(group[0] ^ group[2] ^ group[3]);
        groups.push_back(group);
    }
    for (int i = 0; i < 16; i++)
        groups.push_back(groups[i % 8]);

    // Is this right? Probably doesn't matter since it shouldn't be used in a real decoder
    std::vector<int> groupE = {bits[20 - 1], bits[20 - 1], bits[20 - 1], bits[9 - 1], bits[9 - 1], bits[9 - 1], bits[10 - 1], bits[10 - 1]};
    groups.push_back(groupE);

    for (int i = 0; i < 5; i++) {
        int line = field_index ? 1121 + i : 559 + i;
        // guard area
        for (int pixel = 13; pixel <= 19; pixel++)
            frame[line - 1][pixel - 1] = 128;
        for (int pixel = 100; pixel <= 106; pixel++)
            frame[line - 1][pixel - 1] = 128;

        for (int pixel = 20; pixel <= 99; pixel++) {
            int groupIx = i * 5 + (pixel - 20) / 16;
            int bitIx = (pixel - 20) / 2 % 8;
            frame[line - 1][pixel - 1] = groups[groupIx][bitIx] ? high_ctrl_value : low_ctrl_value;
        }
    }
}

// frame_index decides the sub sampling phases.  picture1 and picture2 are the pictures used for the first and second field.
std::array<std::array<float, 480>, 1125> make_frame(int frame_index, VideoPicture &picture1, VideoPicture &picture2) {
    std::array<std::array<float, 480>, 1125> frame{};

    for (int line = 1; line <= 2; line++)
        for (int pixel = 12; pixel <= 480; pixel++) {
            float v = pixel <= 316 ? high_vits_value : pixel >= 472 ? low_vits_value : pixel >= 456 ? high_vits_value : (pixel - 317) / 4 % 2 ? high_vits_value : low_vits_value;
            if (line == 2)
                v = 255 - v;
            frame[line - 1][pixel - 1] = v;
        }

    // First field audio
    for (int line = 3; line <= 46; line++)
        for (int pixel = (line <= 42 ? 12 : 106); pixel <= 480; pixel++)
            frame[line - 1][pixel - 1] = 128; // no audio signal

    // Second field audio
    for (int line = 565; line <= 608; line++)
        for (int pixel = (line <= 604 ? 12 : 106); pixel <= 480; pixel++)
            frame[line - 1][pixel - 1] = 128;

    // Guard area
    for (int line = 47; line <= 558; line++)
        frame[line - 1][106 - 1] = 128;
    for (int line = 609; line <= 1120; line++)
        frame[line - 1][106 - 1] = 128;

    // Clamping level
    for (int pixel = 107; pixel <= 480; pixel++) {
        frame[563 - 1][pixel - 1] = 128;
        frame[1125 - 1][pixel - 1] = 128;
    }

    // Control signal for program information
    for (int pixel = 12; pixel <= 480; pixel++)
        frame[564 - 1][pixel - 1] = 128; // no signal

    fill_y_area(frame, picture1, 0, 1, (frame_index + 1) % 2);
    fill_y_area(frame, picture2, 1, 0, frame_index % 2);

    fill_c_area(frame, picture1, 0, (frame_index + 1) % 2);
    fill_c_area(frame, picture2, 1, frame_index % 2);

    // field 0 control signal contains data for field 1
    fill_control_signal(frame, 0, 0, frame_index % 2, frame_index % 2);
    // field 1 control signal contains data for the next frame field 0
    fill_control_signal(frame, 1, 1, (frame_index + 1 + 1) % 2, (frame_index + 1 + 1) % 2);

    // fill in the HD signal last since it depends on the other data
    for (int line = 1; line <= 1125; line++) {
        bool sync_positive = line == 1 || (line > 3 && line % 2 == 0);
        for (int pixel = 2; pixel <= 5; pixel++)
            frame[line - 1][pixel - 1] = sync_positive ? low_hd_value : high_hd_value;
        frame[line - 1][6 - 1] = 128;
        for (int pixel = 7; pixel <= 10; pixel++)
            frame[line - 1][pixel - 1] = sync_positive ? high_hd_value : low_hd_value;

        frame[line - 1][1 - 1] = 0.5f * ((line == 1 ? 128.0f : frame[line - 1][0 - 1]) + frame[line - 1][2 - 1]);
        frame[line - 1][11 - 1] = 0.5f * (frame[line - 1][10 - 1] + frame[line - 1][12 - 1]);
    }

    return frame;
}

int main(int argc, char *argv[]) {

    auto *picture = new VideoPicture();
    for (int i = 0; i < MUSE_BUF_HEIGHT * 2; i++)
        for (int j = 0; j < MUSE_Y_BUF_WIDTH * 3; j++) {
            double v = (6 * j / (MUSE_Y_BUF_WIDTH * 3)) / 5.0;
            picture->red[i][j] = v;
            picture->green[i][j] = v;
            picture->blue[i][j] = v;
        }

    for (int i = 0; i < 2; i++)
        frame_data.push_back(make_frame(i, *picture, *picture));

//    int fd = open("muse.bin", O_WRONLY | O_TRUNC | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
//    for (int i = 0; i < 10; i++) {
//        auto data = get_next_frame_callback();
//        for (auto line : *data)
//            for (auto p : line) {
//                uint16_t d = p * 4;
//                write(fd, &d, 2);
//            }
//    }
//    close(fd);
//    exit(0);

    MuseSignalGenerator signal_generator(get_next_frame_callback);
    signal_generator.start();

	for (;;) {
        sleep(1000);
    }

    delete picture;
	return 0;
}
