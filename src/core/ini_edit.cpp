// SPDX-License-Identifier: MIT
#include "aoahid_share/ini_edit.hpp"

#include <algorithm>

namespace aoas {
namespace {

std::string_view trim(std::string_view text) {
    const size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos)
        return {};
    const size_t last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

} // namespace

IniText::IniText(std::string_view text) {
    while (!text.empty()) {
        const size_t newline = text.find('\n');
        const std::string_view line = trim(text.substr(0, newline));
        text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);
        if (line.empty())
            continue;
        if (line.front() == '[' && line.back() == ']') {
            sections_.push_back({std::string(line.substr(1, line.size() - 2)), {}});
            continue;
        }
        const size_t equals = line.find('=');
        if (equals == std::string_view::npos || sections_.empty())
            continue;
        sections_.back().lines.emplace_back(std::string(trim(line.substr(0, equals))),
                                            std::string(trim(line.substr(equals + 1))));
    }
}

std::optional<std::string> IniText::get(const std::string_view section,
                                        const std::string_view key) const {
    for (const Section& each : sections_) {
        if (each.name != section)
            continue;
        for (const auto& line : each.lines) {
            if (line.first == key)
                return line.second;
        }
    }
    return std::nullopt;
}

void IniText::set(const std::string_view section, const std::string_view key,
                  const std::string_view value) {
    for (Section& each : sections_) {
        if (each.name != section)
            continue;
        for (auto& line : each.lines) {
            if (line.first == key) {
                line.second = std::string(value);
                return;
            }
        }
        each.lines.emplace_back(std::string(key), std::string(value));
        return;
    }
    sections_.push_back({std::string(section), {{std::string(key), std::string(value)}}});
}

void IniText::erase(const std::string_view section, const std::string_view key) {
    for (Section& each : sections_) {
        if (each.name != section)
            continue;
        each.lines.erase(std::remove_if(each.lines.begin(), each.lines.end(),
                                        [&](const auto& line) { return line.first == key; }),
                         each.lines.end());
    }
}

std::string IniText::str() const {
    std::string text;
    for (const Section& each : sections_) {
        if (!text.empty())
            text += '\n';
        text += '[' + each.name + "]\n";
        for (const auto& line : each.lines)
            text += line.first + " = " + line.second + '\n';
    }
    return text;
}

} // namespace aoas
