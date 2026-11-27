#ifndef EXTRACT_CONFIG_HPP
#define EXTRACT_CONFIG_HPP

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

#include <array>
#include <istream>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace osmium_extract {

using Longitude = rfl::Validator<double, rfl::Minimum<-180>, rfl::Maximum<180>>;
using Latitude = rfl::Validator<double, rfl::Minimum<-90>, rfl::Maximum<90>>;

using Coordinate = std::array<double, 2>;
using Ring = rfl::Validator<std::vector<Coordinate>, rfl::Size<rfl::Minimum<3>>>;
using InlinePolygon = rfl::Validator<std::vector<Ring>, rfl::Size<rfl::Minimum<1>>>;
using InlineMultiPolygon = rfl::Validator<std::vector<InlinePolygon>, rfl::Size<rfl::Minimum<1>>>;

struct BBoxObject {
    rfl::Description<"Western longitude boundary in degrees (-180..180)", Longitude> left;
    rfl::Description<"Eastern longitude boundary in degrees (-180..180)", Longitude> right;
    rfl::Description<"Northern latitude boundary in degrees (-90..90)", Latitude> top;
    rfl::Description<"Southern latitude boundary in degrees (-90..90)", Latitude> bottom;
};

using BBoxSpec = std::variant<std::array<double, 4>, BBoxObject>;

struct PolygonFileSpec {
    rfl::Description<"Path to polygon file (relative to config directory if not absolute)", std::string> file_name;
    rfl::Description<"Optional polygon file format ('poly', 'geojson', or 'osm'; auto-detected if omitted)", std::optional<std::string>> file_type;
};

using PolygonSpec = std::variant<InlinePolygon, PolygonFileSpec>;
using MultiPolygonSpec = std::variant<InlineMultiPolygon, PolygonFileSpec>;
using OutputHeaderMap = rfl::Object<std::optional<std::string>>;

struct ExtractItemConfig {
    rfl::Description<"Output OSM file name (relative to directory if not absolute, or '-' for STDOUT)", std::string> output;
    rfl::Description<"Optional output OSM file format (deduced from output filename suffix if omitted)", std::optional<std::string>> output_format;
    rfl::Description<"Optional human-readable description of this extract", std::optional<std::string>> description;
    rfl::Description<"Bounding box as [left, bottom, right, top] array or {left, right, top, bottom} object", std::optional<BBoxSpec>> bbox;
    rfl::Description<"Boundary polygon as inline coordinate rings or external file specification", std::optional<PolygonSpec>> polygon;
    rfl::Description<"Boundary multipolygon as inline coordinate polygons or external file specification", std::optional<MultiPolygonSpec>> multipolygon;
    rfl::Description<"Optional header options to add (string value) or remove (null value) in the output file", std::optional<OutputHeaderMap>> output_header;
};

struct ExtractConfig {
    rfl::Description<"Optional base output directory for extract output files", std::optional<std::string>> directory;
    rfl::Description<"List of geographic extracts to produce", std::vector<ExtractItemConfig>> extracts;
};

inline rfl::Result<ExtractConfig> read_extract_config(const std::string_view json_str) {
    return rfl::json::read<ExtractConfig, rfl::NoExtraFields>(json_str);
}

inline rfl::Result<ExtractConfig> read_extract_config(std::istream& stream) {
    return rfl::json::read<ExtractConfig, rfl::NoExtraFields>(stream);
}

inline std::string extract_config_schema_json() {
    return rfl::json::to_schema<ExtractConfig, rfl::NoExtraFields>(
        rfl::json::pretty,
        "JSON Schema for osmium extract configuration files");
}

} // namespace osmium_extract

#endif // EXTRACT_CONFIG_HPP
