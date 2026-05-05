// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/map.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include "logging/Logger.h"
#include "logging/StreamLogger.h"
#include "ac3/Ac3Decoder.h"
#include "ac3/Ac3RfDemodulator.h"
#include "filter/FirFilterStage.h"

namespace nb = nanobind;
using namespace nb::literals;

NB_MODULE(ac3rf, m) {

    // -------------------------------------------------------------------------
    // Enums
    // -------------------------------------------------------------------------

    nb::enum_<LogPriority>(m, "LogPriority")
        .value("DEBUG", eDebug)
        .value("INFO",  eInfo)
        .value("WARN",  eWarn)
        .value("ERROR", eError)
        .value("OFF",   eOff);

    nb::enum_<LogCategoryFlags>(m, "LogCategoryFlags")
        .value("APPLICATION", eApplication)
        .value("PERFORMANCE", ePerformance)
        .value("AUDIO",       eAudio)
        .value("VIDEO",       eVideo)
        .value("DECODER",     eDecoder)
        .value("INPUT",       eInput)
        .value("OUTPUT",      eOutput);

    // -------------------------------------------------------------------------
    // Logger / StreamLogger
    //
    // Logger is the abstract interface; Python code instantiates StreamLogger.
    // Passing a StreamLogger where Logger& is expected works via nanobind's
    // polymorphism support.
    // -------------------------------------------------------------------------

    nb::class_<Logger>(m, "Logger");

    nb::class_<StreamLogger, Logger>(m, "StreamLogger")
        // Only exposes the map argument; always writes to stderr.
        .def("__init__", [](StreamLogger *self, std::map<LogCategoryFlags, LogPriority> level) {
            new (self) StreamLogger(std::move(level), std::cerr, false);
        }, "level"_a)
        .def_ro_static("LOG_ALL",   &StreamLogger::c_log_all)
        .def_ro_static("LOG_INFO",  &StreamLogger::c_log_info)
        .def_ro_static("LOG_WARN",  &StreamLogger::c_log_warn)
        .def_ro_static("LOG_ERROR", &StreamLogger::c_log_error)
        .def_ro_static("LOG_OFF",   &StreamLogger::c_log_off);

    // -------------------------------------------------------------------------
    // Ac3RfDemodulator
    //
    // demodulate_to_symbols accepts a contiguous float32 numpy array and
    // returns the raw QPSK symbols as a bytes object.
    // -------------------------------------------------------------------------

    nb::class_<Ac3RfDemodulator>(m, "Ac3RfDemodulator")
        .def(nb::init<Logger &, double, int, bool>(),
             "log"_a, "input_sample_frequency"_a, "input_block_size"_a, "use_simd"_a,
             // Keep the logger alive for the lifetime of the demodulator.
             nb::keep_alive<1, 2>())
        .def("input_sample_alignment", &Ac3RfDemodulator::inputSampleAlignment)
        .def("demodulate_to_symbols",
             [](Ac3RfDemodulator &self,
                nb::ndarray<float, nb::ndim<1>, nb::c_contig, nb::device::cpu> buf) {
                 auto symbols = self.demodulateToSymbols(buf.data(), buf.size());
                 return nb::bytes(reinterpret_cast<const char *>(symbols.data()), symbols.size());
             },
             "input_buffer"_a);

    // -------------------------------------------------------------------------
    // Ac3Decoder
    //
    // decode_symbols accepts the bytes returned by demodulate_to_symbols and
    // returns a list of bytes objects, one per decoded 1536-byte audio block.
    // -------------------------------------------------------------------------

    nb::class_<Ac3Decoder>(m, "Ac3Decoder")
        .def(nb::init<Logger &>(), "log"_a, nb::keep_alive<1, 2>())
        .def("decode_symbols",
             [](Ac3Decoder &self, nb::bytes symbols) {
                 const auto *data = reinterpret_cast<const uint8_t *>(symbols.c_str());
                 std::vector<uint8_t> vec(data, data + symbols.size());
                 auto blocks = self.decodeSymbols(vec);
                 nb::list result;
                 for (const auto &block : blocks)
                     result.append(nb::bytes(reinterpret_cast<const char *>(block.data()), block.size()));
                 return result;
             },
             "symbols"_a)
        .def("reed_solomon_statistics", &Ac3Decoder::reedSolomonStatistics);

    // -------------------------------------------------------------------------
    // Utilities
    // -------------------------------------------------------------------------

    m.def("simd_supported", &FirFilterStage::simdSupported,
          "Returns True if the current CPU supports the SIMD path.");
}
