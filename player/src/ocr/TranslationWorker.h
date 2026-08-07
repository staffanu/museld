// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_TRANSLATION_WORKER_H
#define MUSELD_TRANSLATION_WORKER_H

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "logging/Logger.h"

// Background Japanese-to-English subtitle translation ("museld-xlate" thread)
// against an OpenAI-compatible /v1/chat/completions server (llama.cpp, Ollama,
// vLLM, or a hosted endpoint).  Cues are translated one request each, FIFO,
// with a rolling window of recent dialogue as context so pronouns and names
// stay consistent -- the same scheme as tools/subocr/translate_srt.py.  If no
// model is given, the first one reported by /v1/models is used.
class TranslationWorker {
public:
    struct Result {
        long id;
        std::vector<std::string> lines;
    };

    // source_language / target_language are plain English language names
    // ("Japanese", "French", ...) interpolated into the prompt.
    TranslationWorker(Logger &log, const std::string &base_url,
                      const std::string &model, const std::string &api_key,
                      const std::string &source_language, const std::string &target_language);
    ~TranslationWorker(); // joins the thread

    TranslationWorker(const TranslationWorker &) = delete;
    TranslationWorker &operator=(const TranslationWorker &) = delete;

    // Render thread: queue one cue for translation.
    void submit(long id, const std::vector<std::string> &ja_lines);

    // Render thread: fetch the translations finished since the last call.
    std::vector<Result> drainResults();

private:
    void threadFunc();
    std::string translate(const std::string &ja_text);

    Logger &m_log;
    std::string m_base_url; // normalized to end with /v1
    std::string m_model;
    std::string m_api_key;
    std::string m_source_language;
    std::string m_target_language;
    std::string m_system_prompt;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop = false;
    std::deque<std::pair<long, std::string>> m_jobs; // id, ja text ('\n'-joined)
    std::vector<Result> m_results;
    std::vector<std::pair<std::string, std::string>> m_history; // recent (ja, en)

    std::thread m_thread;
};

#endif // MUSELD_TRANSLATION_WORKER_H
