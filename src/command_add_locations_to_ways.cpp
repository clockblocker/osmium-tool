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

#include "command_add_locations_to_ways.hpp"

#include "exception.hpp"
#include "util.hpp"

#include <osmium/index/map.hpp>
#include <osmium/io/file.hpp>
#include <osmium/io/header.hpp>
#include <osmium/io/reader.hpp>
#include <osmium/io/writer.hpp>
#include <osmium/memory/buffer.hpp>
#include <osmium/osm/item_type.hpp>
#include <osmium/osm/location.hpp>
#include <osmium/osm/node.hpp>
#include <osmium/osm/types.hpp>
#include <osmium/util/progress_bar.hpp>
#include <osmium/util/verbose_output.hpp>
#include <osmium/visitor.hpp>

#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct AddLocationsToWaysOptions {
    osmium_cli::CmdShortOpt<"i", "Index type for positive IDs", rfl::DefaultVal<std::string>> index_type = "flex_mem";
    osmium_cli::CmdOpt<"Index type for negative IDs", rfl::DefaultVal<std::string>> index_type_neg = "flex_mem";
    osmium_cli::CmdShortOpt<"I", "Show available index types", osmium_cli::Flag> show_index_types = false;
    osmium_cli::CmdOpt<"Keep node members of relations", osmium_cli::Flag> keep_member_nodes = false;
    osmium_cli::CmdShortOpt<"n", "Keep untagged nodes", osmium_cli::Flag> keep_untagged_nodes = false;
    osmium_cli::CmdOpt<"Ignore missing nodes", osmium_cli::Flag> ignore_missing_nodes = false;
    rfl::Flatten<osmium_cli::CommonOptions> common;
    rfl::Flatten<osmium_cli::MultipleInputsOptions> input;
    rfl::Flatten<osmium_cli::OutputOptions> output;
    rfl::Flatten<osmium_cli::MultipleInputsPositional> positionals;
};

} // anonymous namespace

bool CommandAddLocationsToWays::setup(const std::vector<std::string>& arguments) {
    const auto opts = parse_options<AddLocationsToWaysOptions>(arguments);
    if (!opts) {
        return false;
    }

    if (rfl::unwrap(opts->show_index_types)) {
        const auto& map_factory = osmium::index::MapFactory<osmium::unsigned_object_id_type, osmium::Location>::instance();
        for (const auto& map_type : map_factory.map_types()) {
            std::cout << map_type << '\n';
        }
        return false;
    }

    m_index_type_name_pos = check_index_type(rfl::unwrap(opts->index_type));
    m_index_type_name_neg = check_index_type(rfl::unwrap(opts->index_type_neg));

    setup_common(opts->common());
    setup_input_files(opts->input(), rfl::unwrap(opts->positionals().input_filenames));
    setup_output_file(opts->output());

    if (rfl::unwrap(opts->keep_untagged_nodes)) {
        m_keep_untagged_nodes = true;
    }

    if (rfl::unwrap(opts->keep_member_nodes)) {
        m_keep_member_nodes = true;
    }

    if (rfl::unwrap(opts->ignore_missing_nodes)) {
        m_ignore_missing_nodes = true;
    }

    // If we keep all nodes anyway, the member nodes don't need special consideration
    if (m_keep_untagged_nodes && m_keep_member_nodes) {
        std::cerr << "Warning! Option --keep-member-nodes is unnecessary when --keep-untagged-nodes is set.\n";
        m_keep_member_nodes = false;
    }

    return true;
}

void CommandAddLocationsToWays::show_arguments() {
    show_multiple_inputs_arguments(m_vout);
    show_output_arguments(m_vout);

    m_vout << "  other options:\n";
    m_vout << "    index type (for positive ids): " << m_index_type_name_pos << '\n';
    m_vout << "    index type (for negative ids): " << m_index_type_name_neg << '\n';
    m_vout << "    keep untagged nodes: " << yes_no(m_keep_untagged_nodes);
    m_vout << "    keep nodes that are relation members: " << yes_no(m_keep_member_nodes);
    m_vout << '\n';
}

void CommandAddLocationsToWays::copy_data(osmium::ProgressBar& progress_bar, osmium::io::Reader& reader, osmium::io::Writer& writer, location_handler_type& location_handler) const {
    while (osmium::memory::Buffer buffer = reader.read()) {
        progress_bar.update(reader.offset());
        osmium::apply(buffer, location_handler);

        if (m_keep_untagged_nodes) {
            writer(std::move(buffer));
        } else {
            for (const auto& object : buffer) {
                if (object.type() == osmium::item_type::node) {
                    const auto &node = static_cast<const osmium::Node&>(object);
                    if (!node.tags().empty() || m_member_node_ids.get_binary_search(node.positive_id())) {
                        writer(object);
                    }
                } else {
                    writer(object);
                }
            }
        }
    }
}

void CommandAddLocationsToWays::find_member_nodes() {
    for (const auto& input_file : m_input_files) {
        osmium::io::Reader reader{input_file, osmium::osm_entity_bits::relation, osmium::io::read_meta::no};
        while (osmium::memory::Buffer buffer = reader.read()) {
            for (const auto& relation : buffer.select<osmium::Relation>()) {
                for (const auto& member : relation.members()) {
                    if (member.type() == osmium::item_type::node) {
                        m_member_node_ids.set(member.positive_ref());
                    }
                }
            }
        }
    }
    m_member_node_ids.sort_unique();
}

bool CommandAddLocationsToWays::run() {
    m_output_file.set("locations_on_ways");
    osmium::io::Writer writer{m_output_file, m_output_overwrite, m_fsync};

    if (m_keep_member_nodes) {
        m_vout << "Getting all nodes referenced from relations...\n";
        find_member_nodes();
        m_vout << "Found " << m_member_node_ids.size() << " nodes referenced from relations.\n";
    }

    const auto& map_factory = osmium::index::MapFactory<osmium::unsigned_object_id_type, osmium::Location>::instance();
    auto location_index_pos = map_factory.create_map(m_index_type_name_pos);
    auto location_index_neg = map_factory.create_map(m_index_type_name_neg);
    location_handler_type location_handler{*location_index_pos, *location_index_neg};

    if (m_ignore_missing_nodes) {
        location_handler.ignore_errors();
    }

    if (m_input_files.size() == 1) { // single input file
        m_vout << "Copying input file '" << m_input_files[0].filename() << "'...\n";
        osmium::io::Reader reader{m_input_files[0]};
        osmium::io::Header header{reader.header()};
        setup_header(header);
        writer.set_header(header);

        osmium::ProgressBar progress_bar{reader.file_size(), display_progress()};
        copy_data(progress_bar, reader, writer, location_handler);
        progress_bar.done();

        writer.close();
        reader.close();
    } else { // multiple input files
        osmium::io::Header header;
        setup_header(header);
        writer.set_header(header);

        osmium::ProgressBar progress_bar{file_size_sum(m_input_files), display_progress()};
        for (const auto& input_file : m_input_files) {
            progress_bar.remove();
            m_vout << "Copying input file '" << input_file.filename() << "'...\n";
            osmium::io::Reader reader{input_file};

            copy_data(progress_bar, reader, writer, location_handler);

            progress_bar.file_done(reader.file_size());
            reader.close();
        }
        progress_bar.done();
        writer.close();
    }

    const auto mem = location_index_pos->used_memory() + location_index_neg->used_memory();
    m_vout << "About " << show_mbytes(mem) << " MBytes used for node location index (in main memory or on disk).\n";
    show_memory_used();
    m_vout << "Done.\n";

    return true;
}

