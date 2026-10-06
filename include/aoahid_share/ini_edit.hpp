// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aoas {

// An ini text as format_globals() or format_device() writes it, changed one
// key at a time. It keeps sections and keys in their order and adds what is
// missing at the end. It does not check values: the parsers do.
class IniText {
  public:
    explicit IniText(std::string_view text);
    [[nodiscard]] std::optional<std::string> get(std::string_view section,
                                                 std::string_view key) const;
    void set(std::string_view section, std::string_view key, std::string_view value);
    void erase(std::string_view section, std::string_view key);
    [[nodiscard]] std::string str() const;

  private:
    struct Section {
        std::string name;
        std::vector<std::pair<std::string, std::string>> lines;
    };
    std::vector<Section> sections_;
};

} // namespace aoas
