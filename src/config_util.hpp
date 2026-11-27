#ifndef CONFIG_UTIL_HPP
#define CONFIG_UTIL_HPP

/*

Osmium -- OpenStreetMap data manipulation command line tool
https://osmcode.org/osmium-tool/

Copyright (C) 2013-2026  Jochen Topf <jochen@topf.org>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

*/

#include <rfl/Result.hpp>
#include <rfl/error_format.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace osmium_config {

inline std::string_view error_kind_name(const rfl::ErrorKind kind) noexcept {
    switch (kind) {
        case rfl::ErrorKind::io_error:
            return "io_error";
        case rfl::ErrorKind::syntax_error:
            return "syntax_error";
        case rfl::ErrorKind::missing_field:
            return "missing_field";
        case rfl::ErrorKind::unknown_field:
            return "unknown_field";
        case rfl::ErrorKind::duplicate_field:
            return "duplicate_field";
        case rfl::ErrorKind::invalid_length:
            return "invalid_length";
        case rfl::ErrorKind::invalid_type:
            return "invalid_type";
        case rfl::ErrorKind::invalid_value:
            return "invalid_value";
        case rfl::ErrorKind::constraint_violation:
            return "constraint_violation";
        case rfl::ErrorKind::other:
        default:
            return "error";
    }
}

inline std::string format_error_detail(const rfl::ErrorDetail& detail) {
    std::string out = "[";
    out += error_kind_name(detail.kind);
    out += "] ";
    const auto path = rfl::format_path(detail.path);
    if (!path.empty()) {
        out += path;
        out += ": ";
    }
    out += detail.message;
    return out;
}

inline std::string format_rfl_error(const rfl::Error& err) {
    const auto details = err.details();
    if (details.empty()) {
        return err.what();
    }
    if (details.size() == 1) {
        return format_error_detail(details.front());
    }
    std::string out = "Found " + std::to_string(details.size()) + " errors:";
    for (std::size_t i = 0; i < details.size(); ++i) {
        out += "\n  ";
        out += std::to_string(i + 1);
        out += ") ";
        out += format_error_detail(details[i]);
    }
    return out;
}

} // namespace osmium_config

#endif // CONFIG_UTIL_HPP
