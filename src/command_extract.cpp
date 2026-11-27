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

#include "command_extract.hpp"

#include "config_util.hpp"
#include "exception.hpp"

#include "extract/config.hpp"
#include "extract/extract_bbox.hpp"
#include "extract/extract_polygon.hpp"
#include "extract/geojson_file_parser.hpp"
#include "extract/geometry_util.hpp"
#include "extract/osm_file_parser.hpp"
#include "extract/poly_file_parser.hpp"
#include "extract/strategy_complete_ways.hpp"
#include "extract/strategy_complete_ways_with_history.hpp"
#include "extract/strategy_simple.hpp"
#include "extract/strategy_smart.hpp"
#include "util.hpp"

#include <osmium/builder/osm_object_builder.hpp>
#include <osmium/geom/coordinates.hpp>
#include <osmium/handler/check_order.hpp>
#include <osmium/io/any_input.hpp>
#include <osmium/io/header.hpp>
#include <osmium/io/writer_options.hpp>
#include <osmium/osm.hpp>
#include <osmium/osm/box.hpp>
#include <osmium/util/progress_bar.hpp>
#include <osmium/util/string.hpp>
#include <osmium/util/verbose_output.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <system_error>
#include <variant>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#   define WIN32_LEAN_AND_MEAN // Prevent winsock.h inclusion; avoid winsock2.h conflict
#  endif
# include <io.h>
#endif

#ifndef _WIN32
# include <unistd.h>
#endif

namespace {

constexpr const std::size_t max_extracts = 500;

osmium::Box parse_bbox(const osmium_extract::BBoxSpec& spec) {
    if (const auto* arr = std::get_if<std::array<double, 4>>(&spec)) {
        const auto value0 = (*arr)[0];
        const auto value1 = (*arr)[1];
        const auto value2 = (*arr)[2];
        const auto value3 = (*arr)[3];

        if (value0 < -180.0 || value0 > 180.0) {
            throw config_error{"Invalid coordinate in bbox: " + std::to_string(value0) + "."};
        }

        if (value1 < -90.0 || value1 > 90.0) {
            throw config_error{"Invalid coordinate in bbox: " + std::to_string(value1) + "."};
        }

        if (value2 < -180.0 || value2 > 180.0) {
            throw config_error{"Invalid coordinate in bbox: " + std::to_string(value2) + "."};
        }

        if (value3 < -90.0 || value3 > 90.0) {
            throw config_error{"Invalid coordinate in bbox: " + std::to_string(value3) + "."};
        }

        const osmium::Location location1{value0, value1};
        const osmium::Location location2{value2, value3};

        osmium::Box box;
        box.extend(location1);
        box.extend(location2);

        return box;
    }

    const auto& obj = std::get<osmium_extract::BBoxObject>(spec);
    const osmium::Location bottom_left{obj.left.value().value(), obj.bottom.value().value()};
    const osmium::Location top_right{obj.right.value().value(), obj.top.value().value()};

    if (bottom_left.x() < top_right.x() &&
        bottom_left.y() < top_right.y()) {
        return osmium::Box{bottom_left, top_right};
    }

    throw config_error{"Need 'left' < 'right' and 'bottom' < 'top' in 'bbox' object."};
}

#ifdef _WIN32
bool is_valid_driver_char(const char value) noexcept {
    return ((value | 0x20) - 'a') <= ('z' - 'a');
}

bool is_path_rooted(const std::string& path) noexcept {
    const std::size_t len = path.size();

    return (len >= 1 && (path[0] == '\\' || path[0] == '/'))
        || (len >= 2 && is_valid_driver_char(path[0]) && path[1] == ':');
}
#endif

std::size_t parse_multipolygon_object(const std::string& directory, std::string file_name, std::string file_type, osmium::memory::Buffer* buffer) {
    assert(buffer);

    if (file_name.empty()) {
        throw config_error{"Missing 'file_name' in '(multi)polygon' object."};
    }

#ifdef _WIN32
    const bool is_relative = !is_path_rooted(file_name);
#else
    const bool is_relative = file_name[0] != '/';
#endif

    if (is_relative) {
        // relative file name
        file_name = directory + file_name;
    }

    // If the file type is not set, try to deduce it from the file name
    // suffix.
    if (file_type.empty()) {
        if (ends_with(file_name, ".poly")) {
            file_type = "poly";
        } else if (ends_with(file_name, ".json") || ends_with(file_name, ".geojson")) {
            file_type = "geojson";
        } else {
            const std::string suffix{get_filename_suffix(file_name)};
            const osmium::io::File osmfile{"", suffix};
            if (osmfile.format() != osmium::io::file_format::unknown) {
                file_type = "osm";
            }
        }
    }

    if (file_type == "osm") {
        try {
            OSMFileParser parser{*buffer, file_name};
            return parser();
        } catch (const std::system_error& e) {
            throw osmium::io_error{std::string{"While reading file '"} + file_name + "':\n" + e.what()};
        } catch (const osmium::io_error& e) {
            throw osmium::io_error{std::string{"While reading file '"} + file_name + "':\n" + e.what()};
        } catch (const osmium::out_of_order_error& e) {
            throw osmium::io_error{std::string{"While reading file '"} + file_name + "':\n" + e.what()};
        }
    } else if (file_type == "geojson") {
        GeoJSONFileParser parser{*buffer, file_name};
        try {
            return parser();
        } catch (const config_error& e) {
            throw geojson_error{e.what()};
        }
    } else if (file_type == "poly") {
        PolyFileParser parser{*buffer, file_name};
        return parser();
    } else if (file_type.empty()) {
        throw config_error{"Could not autodetect file type in '(multi)polygon' object. Add a 'file_type'."};
    }

    throw config_error{std::string{"Unknown file type: '"} + file_type + "' in '(multi)polygon.file_type'"};
}

std::vector<osmium::geom::Coordinates> ring_to_coordinates(const osmium_extract::Ring& ring) {
    std::vector<osmium::geom::Coordinates> coordinates;
    coordinates.reserve(ring.value().size());
    for (const auto& pt : ring.value()) {
        coordinates.emplace_back(pt[0], pt[1]);
    }
    return coordinates;
}

void add_inline_rings(const osmium_extract::InlinePolygon& polygon, osmium::builder::AreaBuilder* builder) {
    const auto& rings = polygon.value();

    {
        auto outer_ring = ring_to_coordinates(rings[0]);
        if (!is_ccw(outer_ring)) {
            std::reverse(outer_ring.begin(), outer_ring.end());
        }
        osmium::builder::OuterRingBuilder ring_builder{*builder};
        for (const auto& c : outer_ring) {
            const osmium::Location loc{c.x, c.y};
            if (loc.valid()) {
                ring_builder.add_node_ref(0, loc);
            } else {
                throw config_error{"Invalid location in boundary (multi)polygon: (" +
                                   std::to_string(c.x) +
                                   ", " +
                                   std::to_string(c.y) + ")."};
            }
        }
    }

    for (std::size_t i = 1; i < rings.size(); ++i) {
        auto inner_ring = ring_to_coordinates(rings[i]);
        if (is_ccw(inner_ring)) {
            std::reverse(inner_ring.begin(), inner_ring.end());
        }
        osmium::builder::InnerRingBuilder ring_builder{*builder};
        for (const auto& c : inner_ring) {
            const osmium::Location loc{c.x, c.y};
            if (loc.valid()) {
                ring_builder.add_node_ref(0, loc);
            } else {
                throw config_error{"Invalid location in boundary (multi)polygon: (" +
                                   std::to_string(c.x) +
                                   ", " +
                                   std::to_string(c.y) + ")."};
            }
        }
    }
}

std::size_t build_inline_polygon(const osmium_extract::InlinePolygon& polygon, osmium::memory::Buffer* buffer) {
    {
        osmium::builder::AreaBuilder builder{*buffer};
        add_inline_rings(polygon, &builder);
    }

    return buffer->commit();
}

std::size_t build_inline_multipolygon(const osmium_extract::InlineMultiPolygon& multipolygon, osmium::memory::Buffer* buffer) {
    {
        osmium::builder::AreaBuilder builder{*buffer};
        for (const auto& polygon : multipolygon.value()) {
            add_inline_rings(polygon, &builder);
        }
    }

    return buffer->commit();
}

std::size_t parse_polygon(const std::string& directory, const osmium_extract::PolygonSpec& value, osmium::memory::Buffer* buffer) {
    assert(buffer);

    if (const auto* inline_poly = std::get_if<osmium_extract::InlinePolygon>(&value)) {
        return build_inline_polygon(*inline_poly, buffer);
    }

    const auto& file_spec = std::get<osmium_extract::PolygonFileSpec>(value);
    return parse_multipolygon_object(directory,
                                     file_spec.file_name.value(),
                                     file_spec.file_type.value().value_or(""),
                                     buffer);
}

std::size_t parse_multipolygon(const std::string& directory, const osmium_extract::MultiPolygonSpec& value, osmium::memory::Buffer* buffer) {
    assert(buffer);

    if (const auto* inline_mp = std::get_if<osmium_extract::InlineMultiPolygon>(&value)) {
        return build_inline_multipolygon(*inline_mp, buffer);
    }

    const auto& file_spec = std::get<osmium_extract::PolygonFileSpec>(value);
    return parse_multipolygon_object(directory,
                                     file_spec.file_name.value(),
                                     file_spec.file_type.value().value_or(""),
                                     buffer);
}

bool is_existing_directory(const char* name) {
#ifdef _MSC_VER
    // Windows implementation
    // https://msdn.microsoft.com/en-us/library/14h5k7ff.aspx
    struct _stat64 s{};
    if (::_stati64(name, &s) != 0) {
        return false;
    }
    return (s.st_mode & _S_IFDIR) != 0;
#else
    // Unix implementation
    struct stat s; // NOLINT clang-tidy
    if (::stat(name, &s) != 0) {
        return false;
    }
    return S_ISDIR(s.st_mode); // NOLINT(hicpp-signed-bitwise)
#endif
}

} // anonymous namespace

void CommandExtract::set_directory(const std::string& directory) {
    if (!is_existing_directory(directory.c_str())) {
        throw config_error{"Output directory is missing or not accessible: " + directory};
    }
    m_output_directory = directory;
    if (m_output_directory.empty() || m_output_directory.back() != '/') {
        m_output_directory += '/';
    }
}

void CommandExtract::parse_config_file() {
    std::ifstream config_file{m_config_file_name};
    if (!config_file) {
        throw config_error{"Could not open config file '" + m_config_file_name + "'."};
    }

    const auto parsed = osmium_extract::read_extract_config(config_file);
    if (!parsed) {
        throw config_error{osmium_config::format_rfl_error(parsed.error())};
    }

    const auto& doc = *parsed;

    if (const auto& directory = doc.directory.value(); directory && !directory->empty() && m_output_directory.empty()) {
        m_vout << "  Directory set to '" << *directory << "'.\n";
        set_directory(*directory);
    }

    m_vout << "  Reading extracts from config file...\n";
    int extract_num = 1;
    for (const auto& item : doc.extracts.value()) {
        std::string output;
        try {
            output = item.output.value();
            if (output.empty()) {
                throw config_error{"Missing 'output' field for extract."};
            }

            m_vout << "    Looking at extract '" << output << "'...\n";

            const std::string output_format{item.output_format.value().value_or("")};
            const std::string description{item.description.value().value_or("")};

            osmium::io::File output_file{m_output_directory + output, output_format};
            if (m_with_history) {
                output_file.set_has_multiple_object_versions(true);
            } else if (output_file.has_multiple_object_versions()) {
                throw config_error{"Looks like you are trying to write a history file, but option --with-history is not set."};
            }

            if (const auto& bbox = item.bbox.value()) {
                m_extracts.push_back(std::make_unique<ExtractBBox>(output_file, description, parse_bbox(*bbox)));
            } else if (const auto& polygon = item.polygon.value()) {
                m_extracts.push_back(std::make_unique<ExtractPolygon>(output_file, description, m_buffer, parse_polygon(m_config_directory, *polygon, &m_buffer)));
            } else if (const auto& multipolygon = item.multipolygon.value()) {
                m_extracts.push_back(std::make_unique<ExtractPolygon>(output_file, description, m_buffer, parse_multipolygon(m_config_directory, *multipolygon, &m_buffer)));
            } else {
                throw config_error{"Missing geometry for extract. Need 'bbox', 'polygon', or 'multipolygon'."};
            }

            if (const auto& output_header = item.output_header.value()) {
                Extract& extract = *m_extracts.back();
                for (const auto& [key, val] : *output_header) {
                    if (!val) {
                        extract.add_header_option(key);
                    } else {
                        extract.add_header_option(key, *val);
                    }
                }
            }
        } catch (const config_error& e) {
            std::string message{"In extract "};
            message += std::to_string(extract_num);
            message += ": ";
            message += e.what();
            throw config_error{message};
        } catch (const poly_error&) {
            std::cerr << "Error while reading poly file for extract " << extract_num << " (" << output << "):\n";
            throw;
        } catch (const geojson_error&) {
            std::cerr << "Error while reading GeoJSON file for extract " << extract_num << " (" << output << "):\n";
            throw;
        } catch (const std::system_error&) {
            std::cerr << "Error while reading OSM file for extract " << extract_num << " (" << output << "):\n";
            throw;
        } catch (const osmium::io_error&) {
            std::cerr << "Error while reading OSM file for extract " << extract_num << " (" << output << "):\n";
            throw;
        } catch (const osmium::out_of_order_error&) {
            std::cerr << "Error while reading OSM file for extract " << extract_num << " (" << output << "):\n";
            throw;
        }

        ++extract_num;
    }
    m_vout << '\n';
}

std::unique_ptr<ExtractStrategy> CommandExtract::make_strategy(const std::string& name) {
    if (name == "simple") {
        if (m_with_history) {
            throw argument_error{"The 'simple' strategy is not supported for history files."};
        }
        return std::make_unique<strategy_simple::Strategy>(m_extracts, m_options);
    }

    if (name == "complete_ways") {
        if (m_with_history) {
            return std::make_unique<strategy_complete_ways_with_history::Strategy>(m_extracts, m_options);
        }
        return std::make_unique<strategy_complete_ways::Strategy>(m_extracts, m_options);
    }

    if (name == "smart") {
        if (m_with_history) {
            throw argument_error{"The 'smart' strategy is not supported for history files."};
        }
        return std::make_unique<strategy_smart::Strategy>(m_extracts, m_options);
    }

    throw argument_error{std::string{"Unknown extract strategy: '"} + name + "'."};
}

namespace {

struct ExtractOptions {
    osmium_cli::CmdShortOpt<"b", "Bounding box", osmium_cli::OptStr> bbox;
    osmium_cli::CmdShortOpt<"c", "Config file", osmium_cli::OptStr> config;
    osmium_cli::CmdShortOpt<"d", "Output directory (default: from config)", osmium_cli::OptStr> directory;
    osmium_cli::CmdShortOpt<"S", "Set strategy option", osmium_cli::OptStrVec> option;
    osmium_cli::CmdShortOpt<"p", "Polygon file", osmium_cli::OptStr> polygon;
    osmium_cli::CmdOpt<"Print JSON Schema for extract config on STDOUT", osmium_cli::Flag> print_config_schema = false;
    osmium_cli::CmdShortOpt<"s", "Use named extract strategy", rfl::DefaultVal<std::string>> strategy = "complete_ways";
    osmium_cli::CmdShortOpt<"H", "Input file and output files are history files", osmium_cli::Flag> with_history = false;
    osmium_cli::CmdOpt<"Sets bounds (bounding box) in header", osmium_cli::Flag> set_bounds = false;
    osmium_cli::CmdOpt<"Clean attribute (version, changeset, timestamp, uid, user)", osmium_cli::OptStrVec> clean;
    rfl::cli::OptGroup<"COMMON OPTIONS", osmium_cli::CommonOptions> common;
    rfl::cli::OptGroup<"INPUT OPTIONS", osmium_cli::SingleInputOptions> input;
    rfl::cli::OptGroup<"OUTPUT OPTIONS", osmium_cli::OutputOptions> output;
    rfl::Flatten<osmium_cli::SingleInputPositional> positionals;
};

} // anonymous namespace

osmium_cli::CommandSpec CommandExtract::describe_options() const {
    return describe_command_options<ExtractOptions>();
}

bool CommandExtract::setup(const std::vector<std::string>& arguments) {
    const auto opts = parse_options<ExtractOptions>(arguments);
    if (!opts) {
        return false;
    }

    if (rfl::unwrap(opts->print_config_schema)) {
        std::cout << osmium_extract::extract_config_schema_json() << '\n';
        return false;
    }

    setup_common(opts->common());
    setup_input_file(opts->input(), rfl::unwrap(opts->positionals().input_filename));
    init_output_file(opts->output());

    m_clean.setup(rfl::unwrap(opts->clean));

    const int mode_count = static_cast<int>(rfl::unwrap(opts->config).has_value()) +
                           static_cast<int>(rfl::unwrap(opts->bbox).has_value()) +
                           static_cast<int>(rfl::unwrap(opts->polygon).has_value());
    if (mode_count > 1) {
        throw argument_error{"Can only use one of --config/-c, --bbox/-b, or --polygon/-p."};
    }

    if (rfl::unwrap(opts->with_history)) {
        m_with_history = true;
    }

    if (const auto& cfg = rfl::unwrap(opts->config)) {
        if (const auto& dir = rfl::unwrap(opts->directory)) {
            set_directory(*dir);
        }
        if (rfl::unwrap(opts->output().output)) {
            warning("Ignoring --output/-o option.\n");
        }
        if (rfl::unwrap(opts->output().output_format)) {
            warning("Ignoring --output-format/-f option.\n");
        }
        m_config_file_name = *cfg;
        const auto slash = m_config_file_name.find_last_of('/');
        if (slash != std::string::npos) {
            m_config_directory = m_config_file_name;
            m_config_directory.resize(slash + 1);
        }
    }

    if (const auto& b = rfl::unwrap(opts->bbox)) {
        if (rfl::unwrap(opts->directory)) {
            warning("Ignoring --directory/-d option.\n");
        }
        check_output_file();
        if (m_with_history) {
            m_output_file.set_has_multiple_object_versions(true);
        }
        m_extracts.push_back(std::make_unique<ExtractBBox>(m_output_file, "", parse_bbox(*b, "--box/-b")));
    }

    if (const auto& poly = rfl::unwrap(opts->polygon)) {
        if (rfl::unwrap(opts->directory)) {
            warning("Ignoring --directory/-d option.\n");
        }
        check_output_file();
        if (m_with_history) {
            m_output_file.set_has_multiple_object_versions(true);
        }
        m_extracts.push_back(std::make_unique<ExtractPolygon>(m_output_file, "", m_buffer, parse_multipolygon_object("./", *poly, "", &m_buffer)));
    }

    if (const auto& strat_opts = rfl::unwrap(opts->option)) {
        for (const auto& option : *strat_opts) {
            m_options.set(option);
        }
    }

    if (rfl::unwrap(opts->set_bounds)) {
        m_set_bounds = true;
    }

    m_strategy_name = rfl::unwrap(opts->strategy);

    return true;
}

void CommandExtract::show_arguments() {
    show_single_input_arguments(m_vout);
    show_output_arguments(m_vout);

    m_vout << "  strategy options:\n";
    m_vout << "    strategy: " << m_strategy_name << '\n';
    m_vout << "    with history: " << yes_no(m_with_history);

    m_vout << "  other options:\n";
    m_vout << "    config file: " << m_config_file_name << '\n';
    m_vout << "    output directory: " << m_output_directory << '\n';
    m_vout << "    attributes to clean: " << m_clean.to_string() << '\n';

    m_vout << '\n';
}

void CommandExtract::show_extracts() {
    m_vout << "Extracts:\n";

    int n = 1;
    for (const auto& e : m_extracts) {
        const char old_fill = std::cerr.fill();
        m_vout << "[" << std::setw(2) << std::setfill('0') << n
               << "] Output:      " << e->output() << '\n';
        std::cerr.fill(old_fill);
        m_vout << "     Format:      " << e->output_format()    << '\n';
        m_vout << "     Description: " << e->description()      << '\n';
        if (!e->header_options().empty()) {
            m_vout << "     Header opts: ";
            bool first = true;
            for (const auto& opt : e->header_options()) {
                if (first) {
                    first = false;
                } else {
                    m_vout << "                  ";
                }
                m_vout << opt << '\n';
            }
        }
        m_vout << "     Envelope:    " << e->envelope_as_text() << '\n';
        m_vout << "     Type:        " << e->geometry_type()    << '\n';
        m_vout << "     Geometry:    " << e->geometry_as_text() << '\n';
        ++n;
    }

    m_vout << '\n';
}

bool CommandExtract::run() {
    if (!m_config_file_name.empty()) {
        m_vout << "Reading config file...\n";
        try {
            parse_config_file();
        } catch (const config_error&) {
            std::cerr << "Error while reading config file '" << m_config_file_name << "':\n";
            throw;
        }
    }

    if (m_extracts.empty()) {
        throw config_error{"No extract specified in config file or on the command line."};
    }

    if (m_extracts.size() > max_extracts) {
        throw config_error{"Too many extracts specified in config file (Maximum: " + std::to_string(max_extracts) + ")."};
    }

    show_extracts();

    m_strategy = make_strategy(m_strategy_name);
    m_strategy->show_arguments(m_vout);

    osmium::io::Header header;
    osmium::io::Header input_header;
    if (m_input_file.filename().empty()) {
        setup_header(header);
    } else {
        osmium::io::Reader reader{m_input_file, osmium::osm_entity_bits::nothing};
        input_header = reader.header();
        setup_header(header, input_header);
        reader.close();
    }
    header.set("sorting", "Type_then_ID");
    if (m_with_history) {
        header.set_has_multiple_object_versions(true);
    }

    for (const auto& extract : m_extracts) {
        osmium::io::Header file_header{header};
        if (m_set_bounds) {
            file_header.add_box(extract->envelope());
        }
        init_header(file_header, input_header, extract->header_options());
        extract->open_file(file_header, m_output_overwrite, m_fsync, &m_clean);
    }

    m_strategy->run(m_vout, display_progress(), m_input_file);

    for (const auto& extract : m_extracts) {
        extract->close_file();
    }

    show_memory_used();

    m_vout << "Done.\n";

    return true;
}

