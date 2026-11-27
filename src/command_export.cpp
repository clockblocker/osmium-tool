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

#include "command_export.hpp"

#include "config_util.hpp"
#include "exception.hpp"
#include "util.hpp"

#include "export/config.hpp"
#include "export/export_format_json.hpp"
#include "export/export_format_pg.hpp"
#include "export/export_format_text.hpp"
#include "export/export_handler.hpp"

#include <osmium/area/assembler.hpp>
#include <osmium/area/multipolygon_manager.hpp>
#include <osmium/handler/check_order.hpp>
#include <osmium/io/any_input.hpp>
#include <osmium/io/reader_with_progress_bar.hpp>
#include <osmium/memory/buffer.hpp>
#include <osmium/osm.hpp>
#include <osmium/relations/manager_util.hpp>
#include <osmium/util/string.hpp>
#include <osmium/util/verbose_output.hpp>
#include <osmium/visitor.hpp>

#include <cctype>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

std::string resolve_attr_string(const osmium_export::AttributeValue& value, const char* key) {
    if (const auto* str = std::get_if<std::string>(&value)) {
        return *str;
    }

    if (std::get<bool>(value)) {
        return std::string{"@"} + key;
    }

    return {};
}

Ruleset build_tags_ruleset(const osmium_export::TagRulesValue& value) {
    Ruleset ruleset;

    if (!value) {
        // When this is not set or null, the default is "other". This is later
        // changed to "any" if both linear_tags and area_tags are missing.
        ruleset.set_rule_type(tags_filter_rule_type::other);
        return ruleset;
    }

    if (const auto* flag = std::get_if<bool>(&*value)) {
        ruleset.set_rule_type(*flag ? tags_filter_rule_type::any : tags_filter_rule_type::none);
        return ruleset;
    }

    const auto& list = std::get<std::vector<std::string>>(*value);
    if (list.empty()) {
        std::cerr << "Warning! An empty array for 'linear_tags' or 'area_tags' matches any tags.\n"
                  << "         Please use 'true' instead of the array.\n";
        ruleset.set_rule_type(tags_filter_rule_type::any);
        return ruleset;
    }

    ruleset.set_rule_type(tags_filter_rule_type::list);
    for (const auto& str : list) {
        if (!str.empty()) {
            ruleset.add_rule(str);
        }
    }

    return ruleset;
}

} // anonymous namespace

void CommandExport::parse_config_file() {
    std::ifstream config_file{m_config_file_name};
    if (!config_file) {
        throw config_error{"Could not open config file '" + m_config_file_name + "'."};
    }

    const auto parsed = osmium_export::read_export_config(config_file);
    if (!parsed) {
        throw config_error{osmium_config::format_rfl_error(parsed.error())};
    }

    const auto& cfg = *parsed;
    const auto& attrs = cfg.attributes.value();

    m_options.type      = resolve_attr_string(attrs.type.value(),      "type");
    m_options.id        = resolve_attr_string(attrs.id.value(),        "id");
    m_options.version   = resolve_attr_string(attrs.version.value(),   "version");
    m_options.changeset = resolve_attr_string(attrs.changeset.value(), "changeset");
    m_options.timestamp = resolve_attr_string(attrs.timestamp.value(), "timestamp");
    m_options.uid       = resolve_attr_string(attrs.uid.value(),       "uid");
    m_options.user      = resolve_attr_string(attrs.user.value(),      "user");
    m_options.way_nodes = resolve_attr_string(attrs.way_nodes.value(), "way_nodes");

    for (const auto& [key, opt_val] : cfg.format_options.value()) {
        if (!opt_val) {
            m_options.format_options.set(key, false);
            continue;
        }
        std::visit(
            [&](const auto& v) {
                using V = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<V, bool>) {
                    m_options.format_options.set(key, v);
                } else if constexpr (std::is_same_v<V, std::string>) {
                    m_options.format_options.set(key, v);
                } else {
                    m_options.format_options.set(key, std::to_string(v));
                }
            },
            *opt_val);
    }

    m_linear_ruleset = build_tags_ruleset(cfg.linear_tags.value());
    m_area_ruleset   = build_tags_ruleset(cfg.area_tags.value());

    if (m_linear_ruleset.rule_type() == tags_filter_rule_type::other &&
        m_area_ruleset.rule_type() == tags_filter_rule_type::other) {
        m_linear_ruleset.set_rule_type(tags_filter_rule_type::any);
        m_area_ruleset.set_rule_type(tags_filter_rule_type::any);
    }

    for (const auto& str : cfg.include_tags.value()) {
        if (!str.empty()) {
            m_include_tags.emplace_back(str);
        }
    }

    for (const auto& str : cfg.exclude_tags.value()) {
        if (!str.empty()) {
            m_exclude_tags.emplace_back(str);
        }
    }
}

void CommandExport::canonicalize_output_format() {
    for (auto& c : m_output_format) {
        c = static_cast<char>(std::tolower(c));
    }

    if (m_output_format == "json") {
        m_output_format = "geojson";
        return;
    }

    if (m_output_format == "jsonseq") {
        m_output_format = "geojsonseq";
        return;
    }

    if (m_output_format == "txt") {
        m_output_format = "text";
        return;
    }
}

namespace {

struct ExportOptions {
    osmium_cli::CmdShortOpt<"u", "Add unique id to each feature ('counter' or 'type_id')", osmium_cli::OptStr> add_unique_id;
    osmium_cli::CmdShortOpt<"c", "Config file", osmium_cli::OptStr> config;
    osmium_cli::CmdShortOpt<"x", "Output format options", osmium_cli::OptStrVec> format_option;
    osmium_cli::CmdOpt<"Call fsync after writing file", osmium_cli::Flag> fsync = false;
    osmium_cli::CmdOpt<"Geometry types that should be written (default: 'point,linestring,polygon')", osmium_cli::OptStr> geometry_types;
    osmium_cli::CmdShortOpt<"i", "Index type to use", rfl::DefaultVal<std::string>> index_type = "flex_mem";
    osmium_cli::CmdShortOpt<"n", "Keep features that don't have any tags", osmium_cli::Flag> keep_untagged = false;
    osmium_cli::CmdShortOpt<"o", "Output file (default: STDOUT)", osmium_cli::OptStr> output;
    osmium_cli::CmdShortOpt<"f", "Output format (default depends on output file suffix)", osmium_cli::OptStr> output_format;
    osmium_cli::CmdShortOpt<"O", "Allow existing output file to be overwritten", osmium_cli::Flag> overwrite = false;
    osmium_cli::CmdShortOpt<"C", "Print default config on STDOUT", osmium_cli::Flag> print_default_config = false;
    osmium_cli::CmdOpt<"Print JSON Schema for export config on STDOUT", osmium_cli::Flag> print_config_schema = false;
    osmium_cli::CmdShortOpt<"e", "Output any geometry errors on STDOUT", osmium_cli::Flag> show_errors = false;
    osmium_cli::CmdShortOpt<"E", "Stop on the first error encountered", osmium_cli::Flag> stop_on_error = false;
    osmium_cli::CmdShortOpt<"I", "Show available index types", osmium_cli::Flag> show_index_types = false;
    osmium_cli::CmdShortOpt<"a", "Comma-separated list of attributes to add to the output (default: none)", osmium_cli::OptStr> attributes;
    rfl::cli::OptGroup<"COMMON OPTIONS", osmium_cli::CommonOptions> common;
    rfl::cli::OptGroup<"INPUT OPTIONS", osmium_cli::SingleInputOptions> input;
    rfl::Flatten<osmium_cli::SingleInputPositional> positionals;
};

} // anonymous namespace

bool CommandExport::setup(const std::vector<std::string>& arguments) {
    const auto opts = parse_options<ExportOptions>(arguments);
    if (!opts) {
        return false;
    }

    if (rfl::unwrap(opts->print_default_config)) {
        std::cout << osmium_export::default_export_config_json();
        return false;
    }

    if (rfl::unwrap(opts->print_config_schema)) {
        std::cout << osmium_export::export_config_schema_json() << '\n';
        return false;
    }

    if (rfl::unwrap(opts->show_index_types)) {
        const auto& map_factory = osmium::index::MapFactory<osmium::unsigned_object_id_type, osmium::Location>::instance();
        for (const auto& map_type : map_factory.map_types()) {
            std::cout << map_type << '\n';
        }
        std::cout << "none\n";
        return false;
    }

    setup_common(opts->common());
    setup_input_file(opts->input(), rfl::unwrap(opts->positionals().input_filename));

    if (const auto& out = rfl::unwrap(opts->output)) {
        m_output_filename = *out;

        const auto pos = m_output_filename.rfind('.');
        if (pos != std::string::npos) {
            m_output_format = m_output_filename.substr(pos + 1);
        }
    } else {
        m_output_filename = "-";
    }

    if (const auto& fmt = rfl::unwrap(opts->output_format)) {
        m_output_format = *fmt;
    }

    canonicalize_output_format();

    if (m_output_format != "geojson" &&
        m_output_format != "geojsonseq" &&
        m_output_format != "pg" &&
        m_output_format != "text") {
        throw argument_error{"Set output format with --output-format or -f to 'geojson', 'geojsonseq', 'pg', or 'text'."};
    }

    // Set defaults for output format options depending on output format
    if (m_output_format == "geojsonseq") {
        m_options.format_options.set("print_record_separator", true);
    }
    if (m_output_format == "pg") {
        m_options.format_options.set("tags_type", "json");
    }

    if (const auto& cfg = rfl::unwrap(opts->config)) {
        m_config_file_name = *cfg;

        try {
            parse_config_file();
        } catch (const config_error&) {
            std::cerr << "Error while reading config file '" << m_config_file_name << "':\n";
            throw;
        }
    }

    if (const auto& value = rfl::unwrap(opts->add_unique_id)) {
        if (*value == "counter") {
            m_options.unique_id = unique_id_type::counter;
        } else if (*value == "type_id") {
            m_options.unique_id = unique_id_type::type_id;
        } else {
            throw argument_error{"Unknown --add-unique-id, -u setting. Use 'counter' or 'type_id'."};
        }
    }

    if (rfl::unwrap(opts->fsync)) {
        m_fsync = osmium::io::fsync::yes;
    }

    if (const auto& gt = rfl::unwrap(opts->geometry_types)) {
        m_geometry_types.clear();
        const auto types = osmium::split_string(*gt, ',');
        for (const auto& type : types) {
            if (type == "point") {
                m_geometry_types.point = true;
            } else if (type == "linestring") {
                m_geometry_types.linestring = true;
            } else if (type == "polygon") {
                m_geometry_types.polygon = true;
            } else if (type == "multipolygon") {
                m_geometry_types.polygon = true;
            } else {
                throw argument_error{"Unknown geometry type in --geometry-types option: " + type + "."};
            }
        }
        if (m_geometry_types.empty()) {
            throw argument_error{"No geometry types in --geometry-types option."};
        }
    }

    if (const auto& attr_str = rfl::unwrap(opts->attributes)) {
        const auto attrs = osmium::split_string(*attr_str, ',');
        for (const auto& attr : attrs) {
            if (attr == "type") {
                m_options.type = "@type";
            } else if (attr == "id") {
                m_options.id = "@id";
            } else if (attr == "version") {
                m_options.version = "@version";
            } else if (attr == "changeset") {
                m_options.changeset = "@changeset";
            } else if (attr == "timestamp") {
                m_options.timestamp = "@timestamp";
            } else if (attr == "uid") {
                m_options.uid = "@uid";
            } else if (attr == "user") {
                m_options.user = "@user";
            } else if (attr == "way_nodes") {
                m_options.way_nodes = "@way_nodes";
            } else {
                throw argument_error{"Unknown attribute in --attributes option: " + attr + "."};
            }
        }
    }

    m_index_type_name = check_index_type(rfl::unwrap(opts->index_type), true);

    if (rfl::unwrap(opts->keep_untagged)) {
        m_options.keep_untagged = true;
    }

    if (rfl::unwrap(opts->overwrite)) {
        m_output_overwrite = osmium::io::overwrite::allow;
    }

    if (const auto& fmt_opts = rfl::unwrap(opts->format_option)) {
        for (const auto& str : *fmt_opts) {
            m_options.format_options.set(str);
        }
    }

    if (rfl::unwrap(opts->show_errors)) {
        m_show_errors = true;
    }

    if (rfl::unwrap(opts->stop_on_error)) {
        m_show_errors = true;
        m_stop_on_error = true;
    }

    if (!m_include_tags.empty() && !m_exclude_tags.empty()) {
        throw config_error{"Setting both 'include_tags' and 'exclude_tags' is not allowed."};
    }

    if (!m_include_tags.empty()) {
        initialize_tags_filter(m_options.tags_filter, false, m_include_tags);
    } else if (!m_exclude_tags.empty()) {
        initialize_tags_filter(m_options.tags_filter, true, m_exclude_tags);
    }

    if (m_input_file.filename().empty()) {
        throw config_error{"Can not read from STDIN, because input file has to be read twice."};
    }

    return true;
}

namespace {

void print_taglist(osmium::VerboseOutput* vout, const std::vector<std::string>& strings) {
    assert(vout);
    for (const auto& str : strings) {
        *vout << "    " << str << '\n';
    }
}

void print_ruleset(osmium::VerboseOutput* vout, const Ruleset& ruleset) {
    assert(vout);
    switch (ruleset.rule_type()) {
        case tags_filter_rule_type::none:
            *vout << "none\n";
            break;
        case tags_filter_rule_type::any:
            *vout << "any\n";
            break;
        case tags_filter_rule_type::list:
            *vout << "one of the following:\n";
            print_taglist(vout, ruleset.tags());
            break;
        case tags_filter_rule_type::other:
            *vout << "if other tag list doesn't match\n";
            break;
    }
}

const char* print_unique_id_type(unique_id_type unique_id) {
    switch (unique_id) {
        case unique_id_type::counter:
            return "counter";
        case unique_id_type::type_id:
            return "type and id";
        default:
            break;
    }

    return "no";
}

std::unique_ptr<ExportFormat> create_handler(const std::string& output_format,
                                             const std::string& output_filename,
                                             osmium::io::overwrite overwrite,
                                             osmium::io::fsync fsync,
                                             const options_type& options) {
    if (output_format == "geojson" || output_format == "geojsonseq") {
        return std::make_unique<ExportFormatJSON>(output_format, output_filename, overwrite, fsync, options);
    }

    if (output_format == "pg") {
        return std::make_unique<ExportFormatPg>(output_format, output_filename, overwrite, fsync, options);
    }

    if (output_format == "text") {
        return std::make_unique<ExportFormatText>(output_format, output_filename, overwrite, fsync, options);
    }

    throw argument_error{"Unknown output format"};
}

} // anonymous namespace

void CommandExport::show_arguments() {
    show_single_input_arguments(m_vout);

    m_vout << "  output options:\n";
    m_vout << "    file name: " << m_output_filename << '\n';
    m_vout << "    file format: " << m_output_format << '\n';
    m_vout << "    overwrite: " << yes_no(m_output_overwrite == osmium::io::overwrite::allow);
    m_vout << "    fsync: " << yes_no(m_fsync == osmium::io::fsync::yes);
    m_vout << "  attributes:\n";
    m_vout << "    type:      " << (m_options.type.empty()      ? "(omitted)" : m_options.type)      << '\n';
    m_vout << "    id:        " << (m_options.id.empty()        ? "(omitted)" : m_options.id)        << '\n';
    m_vout << "    version:   " << (m_options.version.empty()   ? "(omitted)" : m_options.version)   << '\n';
    m_vout << "    changeset: " << (m_options.changeset.empty() ? "(omitted)" : m_options.changeset) << '\n';
    m_vout << "    timestamp: " << (m_options.timestamp.empty() ? "(omitted)" : m_options.timestamp) << '\n';
    m_vout << "    uid:       " << (m_options.uid.empty()       ? "(omitted)" : m_options.uid)       << '\n';
    m_vout << "    user:      " << (m_options.user.empty()      ? "(omitted)" : m_options.user)      << '\n';
    m_vout << "    way_nodes: " << (m_options.way_nodes.empty() ? "(omitted)" : m_options.way_nodes) << '\n';

    if (!m_options.format_options.empty()) {
        m_vout << "  output format options:\n";
        for (const auto& option : m_options.format_options) {
            m_vout << "    " << option.first << " = " << option.second << '\n';
        }
    }

    m_vout << "  linear tags: ";
    print_ruleset(&m_vout, m_linear_ruleset);
    m_vout << "  area tags:   ";
    print_ruleset(&m_vout, m_area_ruleset);

    if (!m_include_tags.empty()) {
        m_vout << "  include only these tags:\n";
        print_taglist(&m_vout, m_include_tags);
    } else if (!m_exclude_tags.empty()) {
        m_vout << "  exclude these tags:\n";
        print_taglist(&m_vout, m_exclude_tags);
    }

    m_vout << "  other options:\n";
    m_vout << "    index type: " << m_index_type_name << '\n';
    m_vout << "    add unique IDs: " << print_unique_id_type(m_options.unique_id) << '\n';
    m_vout << "    keep untagged features: " << yes_no(m_options.keep_untagged);
}

bool CommandExport::run() {
    auto handler = create_handler(m_output_format, m_output_filename, m_output_overwrite, m_fsync, m_options);
    if (m_vout.verbose()) {
        handler->debug_output(m_vout, m_output_filename);
    }

    const osmium::area::Assembler::config_type assembler_config;
    osmium::area::MultipolygonManager<osmium::area::Assembler> mp_manager{assembler_config};

    m_vout << "First pass (of two) through input file (reading relations)...\n";
    osmium::relations::read_relations(m_input_file, mp_manager);
    m_vout << "First pass done.\n";

    m_vout << "Second pass (of two) through input file...\n";
    m_linear_ruleset.init_filter();
    m_area_ruleset.init_filter();

    ExportHandler export_handler{std::move(handler), m_linear_ruleset, m_area_ruleset, m_geometry_types, m_show_errors, m_stop_on_error};
    osmium::handler::CheckOrder check_order_handler;

    if (m_index_type_name == "none") {
        osmium::io::ReaderWithProgressBar reader{display_progress(), m_input_file};
        osmium::apply(reader, check_order_handler, export_handler, mp_manager.handler([&export_handler](const osmium::memory::Buffer& buffer) {
            osmium::apply(buffer, export_handler);
        }));
        reader.close();
    } else {
        const auto& map_factory = osmium::index::MapFactory<osmium::unsigned_object_id_type, osmium::Location>::instance();
        auto location_index_pos = map_factory.create_map(m_index_type_name);
        auto location_index_neg = map_factory.create_map(m_index_type_name);
        location_handler_type location_handler{*location_index_pos, *location_index_neg};
        if (!m_stop_on_error) {
            location_handler.ignore_errors();
        }

        osmium::io::ReaderWithProgressBar reader{display_progress(), m_input_filename};
        osmium::apply(reader, check_order_handler, location_handler, export_handler, mp_manager.handler([&export_handler](const osmium::memory::Buffer& buffer) {
            osmium::apply(buffer, export_handler);
        }));
        reader.close();
        m_vout << "About "
               << show_mbytes(location_index_pos->used_memory() + location_index_neg->used_memory())
               << " MBytes used for node location index (in main memory or on disk).\n";
    }

    if (m_stop_on_error) {
        const auto incomplete_relations = mp_manager.relations_database().count_relations();
        if (incomplete_relations > 0) {
            throw osmium::geometry_error{"Found " + std::to_string(incomplete_relations) + " incomplete relation(s)"};
        }
    }

    m_vout << "Second pass done.\n";
    export_handler.close();

    m_vout << "Wrote " << export_handler.count() << " features.\n";
    m_vout << "Encountered " << export_handler.error_count() << " errors.\n";

    show_memory_used();

    m_vout << "Done.\n";

    return true;
}

