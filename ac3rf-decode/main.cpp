#include <sys/fcntl.h>
#include <unistd.h>
#include <iostream>
#include <functional>
#include <filesystem>
#include <format>

#include "Ac3RfDemodulator.h"
#include "InputReader.h"
#include "Logger.h"

int main(int argc, char *argv[]) {
    auto log_selection = eWarn;
    enum InputFormat {
        eUint8,
        eSint8,
        eUint16,
        eSint16
      };
    InputFormat input_format = eSint16;
    double input_sample_frequency = 40e6;
    std::optional<std::string> output_filename;

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
        input_format = eUint8;
    });
    options.emplace_back("--sint8", [&] () mutable  -> void {
        input_format = eSint8;
    });
    options.emplace_back("--uint16", [&] () mutable  -> void {
        input_format = eUint16;
    });
    options.emplace_back("--sint16", [&] () mutable  -> void {
        input_format = eSint16;
    });
    options.emplace_back("--sample-freq", [&] () mutable  -> void {
        input_sample_frequency = stod(*(it++));
    });
    options.emplace_back("--output-filename", [&] () mutable  -> void {
        output_filename = std::make_optional(*it++);
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
                Logger log(log_selection);

                if (!std::filesystem::exists(*it)) {
                    std::cerr << "File not found: " << *it << std::endl;
                    exit(EXIT_FAILURE);
                }
                int out_fd = 1;
                if (output_filename.has_value())
                    out_fd = open(output_filename.value().c_str(),
                        O_WRONLY | O_TRUNC | O_CREAT,
                        S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

                InputReader *reader;
                int block_size = 512 * 1024;
                switch (input_format) {
                    case eUint8: reader = new InputReaderImpl<uint8_t>(*it, block_size); break;
                    case eUint16: reader = new InputReaderImpl<uint16_t>(*it, block_size); break;
                    case eSint8: reader = new InputReaderImpl<int8_t>(*it, block_size); break;
                    case eSint16: reader = new InputReaderImpl<int16_t>(*it, block_size); break;
                    default: throw std::runtime_error("Unsupported input format");
                }
                log.info(std::format("Processing input file {}", *it));

                Ac3RfDemodulator demodulator(log, input_sample_frequency, block_size, out_fd);

                float input_buffer[block_size];
                while (reader->readFloats(input_buffer) == block_size) {
                    for (auto output: demodulator.demodulate(input_buffer))
                        if (write(out_fd, output.data(), output.size()) == -1)
                            throw std::runtime_error(std::format("Error writing to output: {}", strerror(errno)));
                }
                log.info(demodulator.reedSolomonStatistics());

                delete reader;

                if (out_fd != 1)
                    close(out_fd);

                it++;
            }
        }
    } catch (const std::exception &x) {
        Logger log(eDebug);
        log.error(x.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
