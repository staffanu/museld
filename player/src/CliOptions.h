// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_CLIOPTIONS_H
#define MUSECPP_CLIOPTIONS_H

#include <algorithm>
#include <functional>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

// The option table used by museld and ac3rf-efm-decode.  Options are registered
// in the order they should be listed by --help, each with the name of the
// argument it takes, which lets the help text show how the option is written and
// lets the parser report a missing argument instead of reading past the end of
// the command line.  The actions still read their own arguments; only the
// presence of one is checked here.
class CliOptions {
public:
    using Action = std::function<void ()>;

    struct Option {
        std::string name;     // "--seek", or empty for a section heading
        std::string argument; // "SECONDS", or empty when the option takes none
        std::string help;
        Action action;

        bool takesArgument() const { return !argument.empty(); }
    };

    // An option that takes no argument
    void flag(std::string name, std::string help, Action action) {
        m_options.push_back(Option{std::move(name), std::string(), std::move(help), std::move(action)});
    }

    // An option that takes one argument, named for the help text ("SECONDS")
    void option(std::string name, std::string argument, std::string help, Action action) {
        m_options.push_back(Option{std::move(name), std::move(argument), std::move(help), std::move(action)});
    }

    // A heading in the help text, grouping the options that follow it
    void section(std::string title) {
        m_options.push_back(Option{std::string(), std::string(), std::move(title), Action()});
    }

    const Option *find(const std::string &name) const {
        for (const auto &option: m_options)
            if (!option.name.empty() && option.name == name)
                return &option;
        return nullptr;
    }

    void printHelp(std::ostream &out, const std::string &synopsis) const {
        out << "usage: " << synopsis << "\n";

        // Help texts line up in a column just past the longest option, unless
        // that is so far right that little room is left for the text itself
        size_t help_column = 0;
        for (const auto &option: m_options)
            if (!option.name.empty())
                help_column = std::max(help_column, c_indent + nameWithArgument(option).length() + c_gap);
        help_column = std::min(help_column, c_max_help_column);

        for (const auto &option: m_options) {
            if (option.name.empty()) {
                out << "\n" << option.help << "\n";
                continue;
            }
            std::string flag = std::string(c_indent, ' ') + nameWithArgument(option);
            const std::vector<std::string> lines = wrap(option.help, c_line_width - help_column);
            if (lines.empty() || flag.length() + c_gap > help_column) { // no room beside the option
                out << flag << "\n";
                flag.clear();
            }
            for (const auto &line: lines) {
                out << flag << std::string(help_column - flag.length(), ' ') << line << "\n";
                flag.clear();
            }
        }
    }

private:
    static constexpr size_t c_indent = 2;    // spaces before the option name
    static constexpr size_t c_gap = 2;       // least space between an option and its help text
    static constexpr size_t c_max_help_column = 30;
    static constexpr size_t c_line_width = 96;

    static std::string nameWithArgument(const Option &option) {
        return option.takesArgument() ? option.name + " " + option.argument : option.name;
    }

    // Breaks the text into lines of at most `width` characters
    static std::vector<std::string> wrap(const std::string &text, size_t width) {
        std::vector<std::string> lines;
        std::istringstream words(text);
        std::string line, word;
        while (words >> word) {
            if (!line.empty() && line.length() + 1 + word.length() > width) {
                lines.push_back(line);
                line.clear();
            }
            line += line.empty() ? word : " " + word;
        }
        if (!line.empty())
            lines.push_back(line);
        return lines;
    }

    std::vector<Option> m_options;
};

#endif //MUSECPP_CLIOPTIONS_H
