#ifndef EXPORT_CONFIG_HPP
#define EXPORT_CONFIG_HPP

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

#include <rfl.hpp>
#include <rfl/json.hpp>

#include <cstdint>
#include <istream>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace osmium_export {

using AttributeValue = std::variant<bool, std::string>;

// uint64_t must precede int64_t because rfl::json::Reader::to_basic_type<int64_t>
// uses yyjson_is_int, which matches unsigned integers as well.
using FormatOptionValue = std::optional<std::variant<bool, uint64_t, int64_t, double, std::string>>;

using TagRulesValue = std::optional<std::variant<bool, std::vector<std::string>>>;

struct ExportAttributes {
    rfl::Description<"Export OSM object type (false, true for '@type', or custom attribute name)", AttributeValue> type = false;
    rfl::Description<"Export OSM object id (false, true for '@id', or custom attribute name)", AttributeValue> id = false;
    rfl::Description<"Export OSM object version (false, true for '@version', or custom attribute name)", AttributeValue> version = false;
    rfl::Description<"Export OSM changeset id (false, true for '@changeset', or custom attribute name)", AttributeValue> changeset = false;
    rfl::Description<"Export OSM timestamp (false, true for '@timestamp', or custom attribute name)", AttributeValue> timestamp = false;
    rfl::Description<"Export OSM user id (false, true for '@uid', or custom attribute name)", AttributeValue> uid = false;
    rfl::Description<"Export OSM user name (false, true for '@user', or custom attribute name)", AttributeValue> user = false;
    rfl::Description<"Export list of way node ids (false, true for '@way_nodes', or custom attribute name)", AttributeValue> way_nodes = false;
};

struct ExportConfig {
    rfl::Description<"OSM object attributes to include in exported features", ExportAttributes> attributes;
    rfl::Description<"Output format-specific options (key-value pairs of scalar values or null)", rfl::Object<FormatOptionValue>> format_options;
    rfl::Description<"Tag filter rules deciding which closed ways are exported as linestrings (true, false, null, or list of tag expressions)", TagRulesValue> linear_tags = std::nullopt;
    rfl::Description<"Tag filter rules deciding which closed ways are exported as areas (true, false, null, or list of tag expressions)", TagRulesValue> area_tags = std::nullopt;
    rfl::Description<"List of tag keys to exclude from output (mutually exclusive with include_tags)", std::vector<std::string>> exclude_tags;
    rfl::Description<"List of tag keys to exclusively include in output (mutually exclusive with exclude_tags)", std::vector<std::string>> include_tags;

    static ExportConfig default_config() {
        ExportConfig cfg;
        cfg.linear_tags = TagRulesValue{true};
        cfg.area_tags = TagRulesValue{true};
        return cfg;
    }
};

inline rfl::Result<ExportConfig> read_export_config(const std::string_view json_str) {
    return rfl::json::read<ExportConfig, rfl::DefaultIfMissing, rfl::NoExtraFields>(json_str);
}

inline rfl::Result<ExportConfig> read_export_config(std::istream& stream) {
    return rfl::json::read<ExportConfig, rfl::DefaultIfMissing, rfl::NoExtraFields>(stream);
}

inline std::string default_export_config_json() {
    return rfl::json::write(ExportConfig::default_config(), rfl::json::pretty) + "\n";
}

inline std::string export_config_schema_json() {
    return rfl::json::to_schema<ExportConfig, rfl::DefaultIfMissing, rfl::NoExtraFields>(
        rfl::json::pretty,
        "JSON Schema for osmium export configuration files");
}

} // namespace osmium_export

#endif // EXPORT_CONFIG_HPP
