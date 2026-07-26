// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <cstdint>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <functional>
#include <filesystem>
#include <format>

#ifdef _WIN32
#  include <io.h>
#  define fsync _commit
#endif
#ifndef O_BINARY
#  define O_BINARY 0
#endif

#include "logging/StreamLogger.h"
#include "input/InputReader.h"
#include "input/InputReaderFactory.h"
#include "ac3/Ac3Decoder.h"
#include "ac3/Ac3RfDemodulator.h"
#include "efm/EfmDemodulator.h"
#include "efm/EfmDecoder.h"
#include "efm/TwoChannelSample.h"
#include "efm/EfmPcmProcessor.h"
#include "CliOptions.h"
#include "Version.h"

enum Operation {
    Ac3,
    Efm,
    EfmRf,
    EfmTValues,
    Resample,
};

void processFile(Logger &logger, Operation input_type,
    double initial_seek_seconds, std::optional<double> duration_seconds,
    double input_sample_frequency,
    std::unique_ptr<InputReader> reader, uint32_t block_size, int out_fd, bool use_simd,
    std::optional<int> efm_log2_decimation_opt,
    int efm_adaptive_filter_size, std::optional<std::string> efm_retiming_debug_filename,
    std::optional<std::string> efm_t_values_output_filename, std::optional<std::string> efm_circ_debug_filename,
    ErasureConcealer::ConcealmentImplementation concealment_impl,
    double target_sample_frequency) {

    reader->initialize();
    if (initial_seek_seconds != 0)
        reader->seek((off_t)(input_sample_frequency * initial_seek_seconds));
    auto *input_buffer = new float[block_size];

    switch (input_type) {
        case Efm:
        case EfmRf: {
            logger.info(eApplication,
                std::format("Processing EFM using input sample frequency {} MHz", input_sample_frequency / 1e6));
            if (input_sample_frequency < 4.3218e6)
                throw std::runtime_error("Efm input sample frequency must be at least 4.3218 MHz");

            int efm_log2_decimation = efm_log2_decimation_opt.value_or(EfmDemodulator::defaultLog2Decimation(input_sample_frequency));

            EfmDemodulator efm_demodulator(logger, input_sample_frequency, reader->block_size(), use_simd,
                input_type == EfmRf, efm_log2_decimation, efm_adaptive_filter_size, efm_retiming_debug_filename);
            EfmDecoder efm_decoder(logger, efm_circ_debug_filename, efm_t_values_output_filename);
            EfmPcmProcessor efm_pcm_processor(logger, concealment_impl);

            std::vector<float> reclocked_data;
            double processed_time = 0.0;
            double prev_processed_time = 0.0;

            while (reader->readFloats(input_buffer) == reader->block_size() &&
                (!duration_seconds.has_value() || processed_time < duration_seconds.value())) {

                efm_demodulator.demodulate(input_buffer, reclocked_data);

                processed_time += block_size / input_sample_frequency;
                bool log_now = (int)processed_time > (int)prev_processed_time;
                std::vector<TwoChannelSampleWithErasureFlags> decoded_samples = efm_decoder.decode(reclocked_data, log_now);
                if (log_now) {
                    fsync(out_fd);
                    logger.sync();
                    prev_processed_time = processed_time;
                }
                std::vector<TwoChannelSample> output = efm_pcm_processor.processSamples(decoded_samples, efm_decoder.preEmphasis());

                if (write(out_fd, output.data(), output.size() * sizeof(TwoChannelSample)) == -1)
                    throw std::runtime_error(std::format("Error writing to output: {}", strerror(errno)));
            }
            logger.info(eAudio, efm_decoder.reedSolomonStatistics());
            logger.info(eAudio, efm_pcm_processor.statistics());
            break;
        }

        case EfmTValues: {
            logger.info(eApplication, "Processing EFM t-values");

            EfmDecoder efm_decoder(logger, efm_circ_debug_filename, efm_t_values_output_filename);
            EfmPcmProcessor efm_pcm_processor(logger, concealment_impl);

            // Reading floats is "wrong" but this is a very unusual use case, so whatever makes the shortest code
            std::vector<float> reclocked_data;
            int processed_input_blocks = 0;
            float data = 1.f;
            while (reader->readFloats(input_buffer) == reader->block_size()) {
                reclocked_data.clear();
                for (int i = 0; i < block_size; i++) {
                    data *= -1.f;
                    for (int j = 0; j < (int)input_buffer[i]; j++)
                        reclocked_data.push_back(data);
                }

                processed_input_blocks++;
                std::vector<TwoChannelSampleWithErasureFlags> output_with_erasures =
                    efm_decoder.decode(reclocked_data, processed_input_blocks % (int)(input_sample_frequency / block_size) == 0);

                std::vector<TwoChannelSample> output = efm_pcm_processor.processSamples(output_with_erasures, efm_decoder.preEmphasis());

                if (write(out_fd, output.data(), output.size() * sizeof(TwoChannelSample)) == -1)
                    throw std::runtime_error(std::format("Error writing to output: {}", strerror(errno)));
            }
            logger.info(eAudio, efm_decoder.reedSolomonStatistics());
            logger.info(eAudio, efm_pcm_processor.statistics());
            break;
        }

        case Ac3: {
            logger.info(eApplication,
                std::format("Processing AC3 using input sample frequency {} MHz", input_sample_frequency / 1e6));

            Ac3RfDemodulator ac3Demodulator(logger, input_sample_frequency, reader->block_size(), use_simd);
            Ac3Decoder ac3Decoder(logger);

            int processed_input_blocks = 0;
            while (reader->readFloats(input_buffer) == reader->block_size() &&
                (!duration_seconds.has_value() || processed_input_blocks < duration_seconds.value() * input_sample_frequency / block_size)) {

                for (auto output: ac3Decoder.decodeSymbols(ac3Demodulator.demodulateToSymbols(input_buffer, reader->block_size())))
                    if (write(out_fd, output.data(), output.size()) == -1)
                        throw std::runtime_error(std::format("Error writing to output: {}", strerror(errno)));

                processed_input_blocks++;
            }
            logger.info(eAudio, ac3Decoder.reedSolomonStatistics());
            break;
        }

        case Resample: {
            logger.info(eApplication,
                std::format("Resampling input at {} MHz to {} MHz", input_sample_frequency / 1e6, target_sample_frequency / 1e6));
            FractionalResampler resampler(block_size);
            double step_size = input_sample_frequency / target_sample_frequency;
            uint8_t samples[block_size];
            float s;
            while (reader->readFloats(input_buffer) == reader->block_size()) {
                resampler.updateInput(input_buffer);
                int i = 0;
                while (resampler.advanceTimeAndResample(step_size, s))
                    samples[i++] = (int)s;
                if (write(out_fd, samples, i) == -1)
                    throw std::runtime_error(std::format("Error writing to output: {}", strerror(errno)));
            }
            break;
        }
    }

    delete[] input_buffer;
}

int main(int argc, char *argv[]) {
    auto log_selection = StreamLogger::c_log_warn;
    std::optional<InputFormat> input_format_option = std::nullopt;
    double input_sample_frequency = 40e6;
    double target_sample_frequency = -1;
    int out_fd = STDOUT_FILENO;
    bool force_stdout = false;
    std::ostream *log_stream = &std::cerr;
    uint32_t block_size = 1024 * 1024;
    bool use_simd = FirFilterStage::simdSupported();
    Operation operation = Ac3;
    double initial_seek_seconds = 0;
    std::optional<double> duration_seconds = std::nullopt;
    std::optional<int> efm_log2_decimation = std::nullopt;
    int efm_adaptive_filter_size = 3;
    std::optional<std::string> efm_retiming_debug_filename = std::nullopt;
    std::optional<std::string> efm_t_values_output_filename = std::nullopt;
    std::optional<std::string> efm_circ_debug_filename = std::nullopt;
    ErasureConcealer::ConcealmentImplementation concealment_impl = ErasureConcealer::AutoregressiveModel;
    bool did_show_help_or_version = false;

    const std::vector<std::string> args(argv + 1, argv + argc);
    auto it = args.cbegin();

    CliOptions options;

    auto usage = [&options] (std::ostream &out, int status) -> void {
        options.printHelp(out, "ac3rf-efm-decode [options] [<input_file> ...]");
        out << "\nInput is read from stdin when no file is given, and the output is written to\n"
               "stdout unless --output-filename says otherwise.  Several input files can be given,\n"
               "with options in between; each one is decoded with the options in effect where it\n"
               "appears.  An argument starting with ! is ignored, which is practical for disabling\n"
               "an option in a saved command line.\n";
        exit(status);
    };

    options.section("Modes:");
    options.flag("--ac3", "Decode AC3-RF surround audio (the default)", [&] () -> void {
        operation = Ac3;
    });
    options.flag("--efm", "Decode EFM audio from a baseband capture, as taken from the EFM output "
                          "of some players", [&] () -> void {
        operation = Efm;
    });
    options.flag("--efm-rf", "Decode EFM audio from an RF capture (bandpass and envelope detection "
                             "first)", [&] () -> void {
        operation = EfmRf;
    });
    options.flag("--efm-t-values", "Decode ld-decode EFM t-values, the run lengths between NRZI "
                                   "transitions (implies --input-format u8)", [&] () -> void {
        input_format_option = std::make_optional(eUint8);
        operation = EfmTValues;
    });
    options.option("--resample", "HZ", "Resample the input to HZ and write it as raw u8; no audio "
                                       "is decoded", [&] () -> void {
        target_sample_frequency = stod(*(it++));
        operation = Resample;
    });

    options.section("Input options:");
    options.option("--input-format", "FMT",
                   "Input sample type: u8, s8, u16, s16, u16be, s16be, lds, flac, ldf "
                   "(default: from the filename extension; required for stdin)", [&] () -> void {
        input_format_option = inputFormatFromString(*(it++));
    });
    options.option("--sample-freq", "HZ",
                   "Input sample rate, written as 40e6 rather than 40 (default 40e6)", [&] () -> void {
        input_sample_frequency = stod(*(it++));
    });
    options.option("--seek", "SECONDS", "Skip this much input before decoding", [&] () -> void {
        initial_seek_seconds = stod(*(it++));
    });
    options.option("--duration", "SECONDS", "Stop after decoding this much audio", [&] () -> void {
        duration_seconds = stod(*(it++));
    });

    options.section("Decoding options:");
    options.option("--decimation", "N",
                   "Log2 of the decimation applied before EFM decoding (default: the most that "
                   "keeps the rate before the fractional resampler above 8 MHz)", [&] () -> void {
        efm_log2_decimation = stoi(*(it++));
        if (efm_log2_decimation < 0)
            throw std::runtime_error("Invalid decimation specification");
    });
    options.option("--adaptive-filter-size", "N",
                   "Adaptive FIR filter size for the EFM signal (default 3, 0 disables it); try 9 "
                   "or 11 when a distorted signal causes many errors", [&] () -> void {
        efm_adaptive_filter_size = stoi(*(it++));
        if (efm_adaptive_filter_size < 0 || efm_adaptive_filter_size > 100)
            throw std::runtime_error("Invalid adaptive filter size");
    });
    options.flag("--simd", "Use SIMD (AVX/NEON) FIR filtering, which is the default where the CPU "
                           "supports it", [&] () -> void {
        use_simd = true;
    });
    options.flag("--no-simd", "Do not use SIMD FIR filtering", [&] () -> void {
        use_simd = false;
    });
    options.option("--error-concealment", "MODE",
                   "Erasure concealment: none, repeat (previous sample), li (linear interpolation), "
                   "ar (autoregressive, the default), sar (slower, more advanced autoregressive)",
                   [&] () -> void {
        std::string concealment_impl_name = *(it++);
        if (concealment_impl_name == "none")
            concealment_impl = ErasureConcealer::None;
        else if (concealment_impl_name == "sar")
            concealment_impl = ErasureConcealer::SlowAutoregressiveModel;
        else if (concealment_impl_name == "ar")
            concealment_impl = ErasureConcealer::AutoregressiveModel;
        else if (concealment_impl_name == "li")
            concealment_impl = ErasureConcealer::LinearInterpolation;
        else if (concealment_impl_name == "repeat")
            concealment_impl = ErasureConcealer::RepeatingSample;
        else
            throw std::runtime_error("Invalid error concealment model");
    });
    options.section("Output options:");
    options.option("--output-filename", "FILE",
                   "Write the decoded audio to FILE instead of stdout; \"-\" writes to stdout even "
                   "when it is a terminal", [&] () -> void {
        auto output_filename = *it++;
        if (out_fd != 1)
            close(out_fd);
        if (output_filename == "-") {
            out_fd = STDOUT_FILENO;
            force_stdout = true;
        } else {
            out_fd = open(output_filename.c_str(),
                O_WRONLY | O_TRUNC | O_CREAT | O_BINARY,
                S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
            if (out_fd == -1)
                throw std::runtime_error(std::format("Unable to open output file {}: {}", output_filename, strerror(errno)));
        }
    });
    options.section("Logging and debugging:");
    options.option("--log", "LEVEL", "Log verbosity: 0 off, 1 error, 2 warn (the default), 3 info, "
                                     "4 debug", [&] () -> void {
        int level_number = stod(*it++);
        std::map<LogCategoryFlags, LogPriority> level;
        switch (level_number) {
            case 0:
                level = StreamLogger::c_log_off;
                break;
            case 1:
                level = StreamLogger::c_log_error;
                break;
            case 2:
                level = StreamLogger::c_log_warn;
                break;
            case 3:
                level = StreamLogger::c_log_info;
                break;
            case 4:
                level = StreamLogger::c_log_all;
                break;
            default: throw std::runtime_error("Invalid log level");
        }
        log_selection = level;
    });
    options.option("--log-filename", "FILE", "Write the log to FILE instead of stderr", [&] () -> void {
        auto log_filename = *it++;
        if (log_stream != &std::cerr) {
            delete log_stream;
            log_stream = nullptr;
        }
        const auto file_stream = new std::ofstream(log_filename);
        if (!file_stream->good())
            throw std::runtime_error(std::format("Unable to open log file {}", log_filename));
        log_stream = file_stream;
    });
    options.option("--reclock-debug-filename", "FILE",
                   "Write the EFM timing recovery data to FILE as binary float pairs: the symbol "
                   "sample and the computed timing error", [&] () -> void {
        efm_retiming_debug_filename = *(it++);
    });
    options.option("--t-values-output-filename", "FILE",
                   "Write the EFM t-values to FILE, one unsigned byte per run length, in the same "
                   "format as ld-decode .efm files", [&] () -> void {
        efm_t_values_output_filename = *(it++);
    });
    options.option("--circ-debug-filename", "FILE",
                   "Write the CIRC decoder's frame-by-frame dump (data and erasure flags around C1 "
                   "and C2) to FILE", [&] () -> void {
        efm_circ_debug_filename = *(it++);
    });
    options.flag("--help", "This text", [&] () -> void {
        usage(std::cout, EXIT_SUCCESS);
    });
    options.flag("--version", "Print the version and exit", [&] () -> void {
        std::cerr << "ac3rf-decode version " << AC3RF_DECODE_VERSION << std::endl;
        did_show_help_or_version = true;
    });

    try {
        bool filename_found = false; // If no filename given, we read from stdin
        while (it != args.cend()) {
            if (const CliOptions::Option *option = options.find(*it)) {
                it++;
                if (option->takesArgument() && it == args.cend())
                    throw std::runtime_error(std::format("{} needs an argument ({})", option->name, option->argument));
                option->action();
            } else if (it->find("!", 0) == 0) {
                it++; // used to ignore options (to easily enable/disable options in debug settings etc.)
            } else if (it->find("-", 0) == 0) {
                std::cerr << "Unknown option: " << *it << std::endl;
                usage(std::cerr, EXIT_FAILURE);
            } else {
                filename_found = true;
                auto filename = *it;
                InputFormat input_format;
                if (input_format_option.has_value())
                    input_format = input_format_option.value();
                else if (auto detected = inputFormatFromFilename(filename); detected.has_value())
                    input_format = detected.value();
                else
                    throw std::runtime_error("Input format not given and unknown input file suffix");

                if (out_fd == STDOUT_FILENO && isatty(out_fd) && !force_stdout)
                    throw std::runtime_error("Writing output to stdout requires non-terminal stdout, or force using \"-\" output filename");

                StreamLogger log(log_selection, *log_stream, false);
                log.debug(eApplication, std::format("ac3rf-decode version {}", AC3RF_DECODE_VERSION));
                log.info(eApplication, std::format("Processing input file {}", filename));
                processFile(log, operation, initial_seek_seconds, duration_seconds, input_sample_frequency,
                    [&] { auto r = makeInputReader(filename, input_format, block_size);
                          r->setDcBlocking(true); // RF carries no legitimate DC
                          return r; }(), block_size, out_fd, use_simd,
                    efm_log2_decimation, efm_adaptive_filter_size, efm_retiming_debug_filename,
                    efm_t_values_output_filename, efm_circ_debug_filename, concealment_impl, target_sample_frequency);

                it++;
            }
        }
        if (!filename_found && !did_show_help_or_version) {
            StreamLogger log(log_selection, *log_stream, false);
            if (!input_format_option.has_value())
                throw std::runtime_error("Input format must be given for stdin");

            log.info(eApplication, std::format("Processing stdin"));
            auto stdin_reader = makeStdinInputReader(input_format_option.value(), block_size);
            processFile(log, operation, initial_seek_seconds, duration_seconds, input_sample_frequency,
                std::move(stdin_reader), block_size, out_fd, use_simd,
                efm_log2_decimation, efm_adaptive_filter_size, efm_retiming_debug_filename,
                efm_t_values_output_filename, efm_circ_debug_filename, concealment_impl, target_sample_frequency);
        }
        if (out_fd != 1)
            close(out_fd);
        if (log_stream != &std::cerr)
            delete log_stream;
    } catch (const std::exception &x) {
        StreamLogger log(StreamLogger::c_log_all, std::cerr, false);
        log.error(eApplication, x.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
