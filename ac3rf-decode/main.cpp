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

#include "Logger.h"
#include "input/InputReader.h"
#include "input/LdfInputReader.h"
#include "input/LdsInputReader.h"
#include "ac3/Ac3RfDemodulator.h"
#include "efm/EfmDemodulator.h"
#include "efm/EfmDecoder.h"
#include "efm/TwoChannelSample.h"
#include "efm/concealment/ErasureConcealer.h"
#include "efm/PopDetector.h"
#include "Version.h"

enum Operation {
    Ac3,
    Efm,
    EfmRf,
    EfmTValues,
    Resample,
};

void processFile(Logger &logger, Operation input_type, InputFormat input_format,
    double initial_seek_seconds, std::optional<double> duration_seconds,
    double input_sample_frequency,
    int in_fd, uint32_t block_size, int out_fd, bool use_simd,
    std::optional<int> efm_log2_decimation_opt,
    int efm_adaptive_filter_size, std::optional<std::string> efm_retiming_debug_filename,
    ErasureConcealer::ConcealmentImplementation concealment_impl,
    double target_sample_frequency) {

    InputReader *reader;
    switch (input_format) {
        case eUint8: reader = new InputReaderImpl<uint8_t>(in_fd, block_size); break;
        case eUint16: reader = new InputReaderImpl<uint16_t>(in_fd, block_size); break;
        case eSint8: reader = new InputReaderImpl<int8_t>(in_fd, block_size); break;
        case eSint16: reader = new InputReaderImpl<int16_t>(in_fd, block_size); break;
        case eLds: reader = new LdsInputReader(in_fd, block_size); break;
        case eFlac: case eFlacOgg: reader = new LdfInputReader(in_fd, block_size, input_format); break;
        default: throw std::runtime_error("Unsupported input format");
    }
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

            int efm_log2_decimation = efm_log2_decimation_opt.value_or(std::max(0, (int)(log(input_sample_frequency / 8e6) / log(2))));

            EfmDemodulator efm_demodulator(logger, input_sample_frequency, reader->block_size(), out_fd, use_simd,
                input_type == EfmRf, efm_log2_decimation, efm_adaptive_filter_size, efm_retiming_debug_filename);
            EfmDecoder efm_decoder(logger);
            std::unique_ptr<ErasureConcealer> erasure_concealer =
                ErasureConcealer::create(concealment_impl, logger, std::nullopt);
            PopDetector pop_detector{logger};

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
                std::vector<TwoChannelSampleWithErasureFlags> pop_detected_samples = pop_detector.processSamples(decoded_samples);
                std::vector<TwoChannelSample> output = erasure_concealer->processSamples(pop_detected_samples);

                if (write(out_fd, output.data(), output.size() * sizeof(TwoChannelSample)) == -1)
                    throw std::runtime_error(std::format("Error writing to output: {}", strerror(errno)));
            }
            logger.info(eAudio, efm_decoder.reedSolomonStatistics());
            logger.info(eAudio, erasure_concealer->erasureStatistics());
            break;
        }

        case EfmTValues: {
            logger.info(eApplication, "Processing EFM t-values");

            EfmDecoder efm_decoder(logger);
            std::unique_ptr<ErasureConcealer> erasure_concealer =
                ErasureConcealer::create(concealment_impl, logger, std::nullopt);

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

                std::vector<TwoChannelSample> output = erasure_concealer->processSamples(output_with_erasures);

                if (write(out_fd, output.data(), output.size() * sizeof(TwoChannelSample)) == -1)
                    throw std::runtime_error(std::format("Error writing to output: {}", strerror(errno)));
            }
            logger.info(eAudio, efm_decoder.reedSolomonStatistics());
            logger.info(eAudio, erasure_concealer->erasureStatistics());
            break;
        }

        case Ac3: {
            logger.info(eApplication,
                std::format("Processing AC3 using input sample frequency {} MHz", input_sample_frequency / 1e6));

            Ac3RfDemodulator ac3Demodulator(logger, input_sample_frequency, reader->block_size(), out_fd, use_simd);

            int processed_input_blocks = 0;
            while (reader->readFloats(input_buffer) == reader->block_size() &&
                (!duration_seconds.has_value() || processed_input_blocks < duration_seconds.value() * input_sample_frequency / block_size)) {

                for (auto output: ac3Demodulator.demodulate(input_buffer))
                    if (write(out_fd, output.data(), output.size()) == -1)
                        throw std::runtime_error(std::format("Error writing to output: {}", strerror(errno)));

                processed_input_blocks++;
            }
            logger.info(eAudio, ac3Demodulator.reedSolomonStatistics());
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
    delete reader;
}

int main(int argc, char *argv[]) {
    auto log_selection = Logger::c_log_warn;
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
    ErasureConcealer::ConcealmentImplementation concealment_impl = ErasureConcealer::AutoregressiveModel;
    bool did_show_help_or_version = false;

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
    options.emplace_back("--flac", [&] () mutable  -> void {
        input_format_option = std::make_optional(eFlac);
    });
    options.emplace_back("--ldf", [&] () mutable  -> void {
        input_format_option = std::make_optional(eFlacOgg);
    });
    options.emplace_back("--seek", [&] () mutable -> void {
        initial_seek_seconds = stod(*(it++));
    });
    options.emplace_back("--duration", [&] () mutable -> void {
        duration_seconds = stod(*(it++));
    });
    options.emplace_back("--sample-freq", [&] () mutable  -> void {
        input_sample_frequency = stod(*(it++));
    });
    options.emplace_back("--simd", [&] () mutable  -> void {
        use_simd = true;
    });
    options.emplace_back("--no-simd", [&] () mutable  -> void {
        use_simd = false;
    });
    options.emplace_back("--efm", [&] () mutable  -> void {
        operation = Efm;
    });
    options.emplace_back("--efm-rf", [&] () mutable  -> void {
        operation = EfmRf;
    });
    options.emplace_back("--efm-t-values", [&] () mutable  -> void {
        input_format_option = std::make_optional(eUint8);
        operation = EfmTValues;
    });
    options.emplace_back("--resample", [&] () mutable  -> void {
        target_sample_frequency = stod(*(it++));
        operation = Resample;
    });
    options.emplace_back("--decimation", [&] () mutable  -> void {
        efm_log2_decimation = stoi(*(it++));
        if (efm_log2_decimation < 0)
            throw std::runtime_error("Invalid decimation specification");
    });
    options.emplace_back("--adaptive-filter-size", [&] () mutable  -> void {
        efm_adaptive_filter_size = stoi(*(it++));
        if (efm_adaptive_filter_size < 0 || efm_adaptive_filter_size > 100)
            throw std::runtime_error("Invalid adaptive filter size");
    });
    options.emplace_back("--reclock-debug-filename", [&] () mutable  -> void {
        efm_retiming_debug_filename = *(it++);
    });
    options.emplace_back("--error-concealment", [&] () mutable  -> void {
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
    options.emplace_back("--output-filename", [&] () mutable  -> void {
        auto output_filename = *it++;
        if (out_fd != 1)
            close(out_fd);
        if (*it == "-") {
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
    options.emplace_back("--log", [&] () mutable -> void {
        int level_number = stod(*it++);
        std::map<LogCategoryFlags, LogPriority> level;
        switch (level_number) {
            case 0:
                level = Logger::c_log_off;
                break;
            case 1:
                level = Logger::c_log_error;
                break;
            case 2:
                level = Logger::c_log_warn;
                break;
            case 3:
                level = Logger::c_log_info;
                break;
            case 4:
                level = Logger::c_log_all;
                break;
            default: throw std::runtime_error("Invalid log level");
        }
        log_selection = level;
    });
    options.emplace_back("--log-filename", [&] () mutable  -> void {
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
    options.emplace_back("--help", [&] () mutable -> void {
        usage();
        did_show_help_or_version = true;
    });
    options.emplace_back("--version", [&] () mutable -> void {
        std::cerr << "ac3rf-decode version " << AC3RF_DECODE_VERSION << std::endl;
        did_show_help_or_version = true;
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
                    else if (filename.ends_with(".flac")) input_format = eFlac;
                    else if (filename.ends_with(".ldf")) input_format = eFlacOgg;
                    else throw std::runtime_error("Input format not given and unknown input file suffix");
                } else
                    input_format = input_format_option.value();

                int fd = open(filename.c_str(), O_RDONLY | O_BINARY);
                if (fd == -1)
                    throw std::runtime_error(std::format("Error opening input file {}: {}", filename, strerror(errno)));

                if (out_fd == STDOUT_FILENO && isatty(out_fd) && !force_stdout)
                    throw std::runtime_error("Writing output to stdout requires non-terminal stdout, or force using \"-\" output filename");

                Logger log(log_selection, *log_stream, false);
                log.debug(eApplication, std::format("ac3rf-decode version {}", AC3RF_DECODE_VERSION));
                log.info(eApplication, std::format("Processing input file {}", filename));
                processFile(log, operation, input_format, initial_seek_seconds, duration_seconds, input_sample_frequency, fd, block_size, out_fd, use_simd,
                    efm_log2_decimation, efm_adaptive_filter_size, efm_retiming_debug_filename, concealment_impl, target_sample_frequency);

                close(fd);
                it++;
            }
        }
        if (!filename_found && !did_show_help_or_version) {
            Logger log(log_selection, *log_stream);
            if (!input_format_option.has_value())
                throw std::runtime_error("Input format must be given for stdin");

            log.info(eApplication, std::format("Processing stdin"));
            processFile(log, operation, input_format_option.value(), initial_seek_seconds, duration_seconds, input_sample_frequency, 0, block_size, out_fd, use_simd,
                efm_log2_decimation, efm_adaptive_filter_size, efm_retiming_debug_filename, concealment_impl, target_sample_frequency);
        }
        if (out_fd != 1)
            close(out_fd);
        if (log_stream != &std::cerr)
            delete log_stream;
    } catch (const std::exception &x) {
        Logger log(Logger::c_log_all);
        log.error(eApplication, x.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
