// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include "TranslationWorker.h"

#include <chrono>
#include <format>
#include <sstream>

#include "httplib.h"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

namespace {

constexpr int c_context_pairs = 8; // recent dialogue pairs sent as context

// Same instructions as tools/subocr/translate_srt.py, parameterized on the
// language pair
std::string makeSystemPrompt(const std::string &source, const std::string &target) {
    return std::format(
        "You translate {0} film subtitles to {1}, one cue at a time.\n"
        "You are given the recent dialogue ({0} with your {1} translations) "
        "as context, then one new {0} cue.\n"
        "Reply with ONLY the {1} subtitle text for the new cue - no quotes, no "
        "notes, no {0}. Keep it concise and natural, as a subtitle: idiomatic "
        "{1}, consistent names and pronouns with the context, preserve line "
        "breaks if the cue has two lines.", source, target);
}

std::vector<std::string> splitLines(const std::string &text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line))
        if (!line.empty()) lines.push_back(line);
    return lines;
}

} // namespace

TranslationWorker::TranslationWorker(Logger &log, const std::string &base_url,
                                     const std::string &model, const std::string &api_key,
                                     const std::string &source_language,
                                     const std::string &target_language)
        : m_log(log),
          m_model(model),
          m_api_key(api_key),
          m_source_language(source_language),
          m_target_language(target_language),
          m_system_prompt(makeSystemPrompt(source_language, target_language)) {
    m_base_url = base_url;
    while (!m_base_url.empty() && m_base_url.back() == '/') m_base_url.pop_back();
    if (!m_base_url.ends_with("/v1")) m_base_url += "/v1";
    m_thread = std::thread(&TranslationWorker::threadFunc, this);
}

TranslationWorker::~TranslationWorker() {
    {
        std::scoped_lock lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();
    m_thread.join();
}

void TranslationWorker::submit(long id, const std::vector<std::string> &ja_lines) {
    std::string text;
    for (const auto &line : ja_lines) {
        if (!text.empty()) text += '\n';
        text += line;
    }
    {
        std::scoped_lock lock(m_mutex);
        m_jobs.emplace_back(id, std::move(text));
    }
    m_cv.notify_all();
}

std::vector<TranslationWorker::Result> TranslationWorker::drainResults() {
    std::scoped_lock lock(m_mutex);
    return std::exchange(m_results, {});
}

std::string TranslationWorker::translate(const std::string &ja_text) {
    // Split "http(s)://host:port" off the path prefix for httplib
    const auto scheme_end = m_base_url.find("://");
    const auto path_start = m_base_url.find('/', scheme_end == std::string::npos ? 0 : scheme_end + 3);
    const std::string host = m_base_url.substr(0, path_start);
    const std::string prefix = path_start == std::string::npos ? "" : m_base_url.substr(path_start);

    httplib::Client client(host);
    client.set_connection_timeout(10);
    client.set_read_timeout(120);
    httplib::Headers headers;
    if (!m_api_key.empty()) headers.emplace("Authorization", "Bearer " + m_api_key);

    if (m_model.empty()) {
        auto response = client.Get(prefix + "/models", headers);
        if (!response || response->status != 200)
            throw std::runtime_error(std::format("GET /v1/models failed: {}",
                    response ? std::to_string(response->status) : httplib::to_string(response.error())));
        m_model = json::parse(response->body).at("data").at(0).at("id").get<std::string>();
        m_log.info(eApplication, std::format("Translation model: {}", m_model));
    }

    std::string prompt;
    if (!m_history.empty()) {
        prompt += "Recent dialogue:\n";
        const size_t from = m_history.size() > c_context_pairs ? m_history.size() - c_context_pairs : 0;
        for (size_t i = from; i < m_history.size(); i++) {
            prompt += m_source_language + ": " + m_history[i].first + "\n";
            prompt += m_target_language + ": " + m_history[i].second + "\n";
        }
        prompt += "\n";
    }
    prompt += "New cue to translate:\n" + ja_text;

    const json request = {
        {"model", m_model},
        {"max_tokens", 256},
        // Reasoning models otherwise burn seconds (and the token budget) on
        // thinking before the one-line answer
        {"chat_template_kwargs", {{"enable_thinking", false}}},
        {"messages", {
            {{"role", "system"}, {"content", m_system_prompt}},
            {{"role", "user"}, {"content", prompt}},
        }},
    };
    auto response = client.Post(prefix + "/chat/completions", headers, request.dump(),
                                "application/json");
    if (!response || response->status != 200)
        throw std::runtime_error(std::format("POST /v1/chat/completions failed: {}",
                response ? std::to_string(response->status) : httplib::to_string(response.error())));
    std::string english = json::parse(response->body)
            .at("choices").at(0).at("message").at("content").get<std::string>();
    while (!english.empty() && (english.back() == '\n' || english.back() == ' '))
        english.pop_back();

    // A model meta-answer instead of a translation is far longer than the cue
    if (english.empty() || english.size() > std::max<size_t>(80, 5 * ja_text.size()))
        return ja_text;
    m_history.emplace_back(ja_text, english);
    if (m_history.size() > 4 * c_context_pairs)
        m_history.erase(m_history.begin(), m_history.end() - c_context_pairs);
    return english;
}

void TranslationWorker::threadFunc() {
    for (;;) {
        long id;
        std::string ja_text;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [this] { return m_stop || !m_jobs.empty(); });
            if (m_stop) break;
            std::tie(id, ja_text) = std::move(m_jobs.front());
            m_jobs.pop_front();
        }
        std::string english;
        const auto t0 = std::chrono::steady_clock::now();
        try {
            english = translate(ja_text);
        } catch (const std::exception &x) {
            m_log.warn(eApplication, std::format("Translation failed: {}", x.what()));
            english = ja_text; // show the untranslated cue rather than nothing
        }
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
        // Sustained slowness (server busy, or its thinking mode enabled after
        // all) makes translations trail their cues -- surface it
        m_log.log(elapsed_ms > 3000 ? eWarn : eDebug, eApplication,
                  std::format("Translation took {} ms", elapsed_ms));
        std::scoped_lock lock(m_mutex);
        m_results.push_back({id, splitLines(english)});
    }
}
