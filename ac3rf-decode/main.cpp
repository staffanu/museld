#include <sys/fcntl.h>
#include <unistd.h>
#include <iostream>
#include <functional>
#include <filesystem>
#include <format>

#include "Ac3RfDemodulator.h"
#include "InputReader.h"
#include "LdfInputReader.h"
#include "LdsInputReader.h"
#include "Logger.h"

enum InputFormat {
    eUint8,
    eSint8,
    eUint16,
    eSint16,
    eLds,
    eLdf
  };

void demodulateFile(Logger &log, InputFormat input_format, double input_sample_frequency,
    int in_fd, uint32_t block_size, int out_fd, bool use_simd) {

    InputReader *reader;
    switch (input_format) {
        case eUint8: reader = new InputReaderImpl<uint8_t>(in_fd, block_size); break;
        case eUint16: reader = new InputReaderImpl<uint16_t>(in_fd, block_size); break;
        case eSint8: reader = new InputReaderImpl<int8_t>(in_fd, block_size); break;
        case eSint16: reader = new InputReaderImpl<int16_t>(in_fd, block_size); break;
        case eLds: reader = new LdsInputReader(in_fd, block_size); break;
        case eLdf: reader = new LdfInputReader(in_fd, block_size); break;
        default: throw std::runtime_error("Unsupported input format");
    }
    reader->initialize();

    Ac3RfDemodulator demodulator(log, input_sample_frequency, reader->block_size(), out_fd, use_simd);

    float input_buffer[reader->block_size()];
    while (reader->readFloats(input_buffer) == reader->block_size()) {
        for (auto output: demodulator.demodulate(input_buffer))
            if (write(out_fd, output.data(), output.size()) == -1)
                throw std::runtime_error(std::format("Error writing to output: {}", strerror(errno)));
    }
    log.info(demodulator.reedSolomonStatistics());

    delete reader;
}

int main(int argc, char *argv[]) {
    auto log_selection = eWarn;
    std::optional<InputFormat> input_format_option = std::nullopt;
    double input_sample_frequency = 40e6;
    int out_fd = 1;
    uint32_t block_size = 16 * 1024;
    bool use_simd = false;

    const std::vector<std::string> args(argv + 1, argv + argc);
    auto it = args.cbegin();

    std::vector<std::pair<std::string, std::function<void ()>>> options;

    auto usage = [&options] () -> void {
        std::cerr << "usage: ac3rf-decode ";
        for (auto o: options)
            std::cerr << "[" << o.first << "] ";
        std::cerr << "<input_file>" << std::endl;
        exit(EXIT_FAILURE);
    };

    options.emplace_back("--uint8", [&] () mutable  -> void {
        input_format_option = std::make_optional(eUint8);
    });
    options.emplace_back("--sint8", [&] () mutable  -> void {
        input_format_option = std::make_optional(eSint8);
    });
    options.emplace_back("--uint16", [&] () mutable  -> void {
        input_format_option = std::make_optional(eUint16);
    });
    options.emplace_back("--sint16", [&] () mutable  -> void {
        input_format_option = std::make_optional(eSint16);
    });
    options.emplace_back("--lds", [&] () mutable  -> void {
        input_format_option = std::make_optional(eLds);
    });
    options.emplace_back("--ldf", [&] () mutable  -> void {
        input_format_option = std::make_optional(eLdf);
    });
    options.emplace_back("--sample-freq", [&] () mutable  -> void {
        input_sample_frequency = stod(*(it++));
    });
    options.emplace_back("--simd", [&] () mutable  -> void {
        use_simd = true;
    });
    options.emplace_back("--output-filename", [&] () mutable  -> void {
        auto output_filename = *it++;
        if (out_fd != 1)
            close(out_fd);
        out_fd = open(output_filename.c_str(),
            O_WRONLY | O_TRUNC | O_CREAT,
            S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        if (out_fd == -1)
            throw std::runtime_error(std::format("Unable to open output file {}: {}", output_filename, strerror(errno)));
    });
    options.emplace_back("--log", [&] () mutable -> void {
        int level_number = stod(*it++);
        LogPriority level;
        switch (level_number) {
            case 0:
                level = eOff;
                break;
            case 1:
                level = eError;
                break;
            case 2:
                level = eWarn;
                break;
            case 3:
                level = eInfo;
                break;
            case 4:
                level = eDebug;
                break;
            default: throw std::runtime_error("Invalid log level");
        }
        log_selection = level;
    });
    options.emplace_back("--help", [&] () mutable -> void {
        usage();
    });

    try {
        bool filename_found = false; // If no filename given, we read from stdin
        while (it != args.cend()) {
            auto option = std::find_if(options.cbegin(), options.cend(),
                                    [it](const std::pair<std::string, std::function<void()>> &pair) -> bool {
                                        return *it == pair.first;
                                    });
            if (option != options.cend()) {
                it++;
                option->second();
            } else if (it->find("!", 0) == 0) {
                it++; // used to ignore options (to easily enable/disable options in debug settings etc.)
            } else if (it->find("-", 0) == 0) {
                usage();
            } else {
                filename_found = true;
                auto filename = *it;
                InputFormat input_format;
                if (!input_format_option.has_value()) {
                    if (filename.ends_with(".u8")) input_format = eUint8;
                    else if (filename.ends_with(".s8")) input_format = eSint8;
                    else if (filename.ends_with(".u16")) input_format = eUint16;
                    else if (filename.ends_with(".s16")) input_format = eSint16;
                    else if (filename.ends_with(".lds")) input_format = eLds;
                    else if (filename.ends_with(".ldf")) input_format = eLdf;
                    else throw std::runtime_error("Input format not given and unknown input file suffix");
                } else
                    input_format = input_format_option.value();

                int fd = open(filename.c_str(), O_RDONLY);
                if (fd == -1)
                    throw std::runtime_error(std::format("Error opening input file {}: {}", filename, strerror(errno)));

                Logger log(log_selection);
                log.info(std::format("Processing input file {}", filename));
                demodulateFile(log, input_format, input_sample_frequency, fd, block_size, out_fd, use_simd);

                close(fd);
                it++;
            }
        }
        if (!filename_found) {
            Logger log(log_selection);
            if (!input_format_option.has_value())
                throw std::runtime_error("Input format must be given for stdin");

            log.info(std::format("Processing stdin"));
            demodulateFile(log, input_format_option.value(), input_sample_frequency, 0, block_size, out_fd, use_simd);
        }
        if (out_fd != 1)
            close(out_fd);
    } catch (const std::exception &x) {
        Logger log(eDebug);
        log.error(x.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
