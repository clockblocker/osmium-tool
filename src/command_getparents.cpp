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

#include "command_getparents.hpp"

#include "exception.hpp"
#include "id_file.hpp"
#include "util.hpp"

#include <osmium/io/header.hpp>
#include <osmium/io/reader.hpp>
#include <osmium/io/writer.hpp>
#include <osmium/memory/buffer.hpp>
#include <osmium/osm.hpp>
#include <osmium/osm/types_from_string.hpp>
#include <osmium/util/progress_bar.hpp>
#include <osmium/util/string.hpp>
#include <osmium/util/verbose_output.hpp>

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct GetParentsOptions {
    osmium_cli::CmdOpt<"Default item type", rfl::DefaultVal<std::string>> default_type = "node";
    osmium_cli::CmdShortOpt<"i", "Read OSM IDs from text file", osmium_cli::OptStrVec> id_file;
    osmium_cli::CmdShortOpt<"I", "Read OSM IDs from OSM file", osmium_cli::OptStrVec> id_osm_file;
    osmium_cli::CmdShortOpt<"s", "Add objects with specified IDs themselves", osmium_cli::Flag> add_self = false;
    osmium_cli::CmdOpt<"Print all requested IDs", osmium_cli::Flag> verbose_ids = false;
    rfl::Flatten<osmium_cli::CommonOptions> common;
    rfl::Flatten<osmium_cli::SingleInputOptions> input;
    rfl::Flatten<osmium_cli::OutputOptions> output;
    rfl::Hidden<rfl::Positional<osmium_cli::OptStr>> input_filename;
    rfl::Hidden<rfl::Positional<osmium_cli::OptStrVec>> ids;
};

} // anonymous namespace

bool CommandGetParents::setup(const std::vector<std::string>& arguments) {
    const auto opts = parse_options<GetParentsOptions>(arguments);
    if (!opts) {
        return false;
    }

    setup_common(opts->common());
    setup_input_file(opts->input(), rfl::unwrap(opts->input_filename));
    setup_output_file(opts->output());

    if (rfl::unwrap(opts->add_self)) {
        m_add_self = true;
    }

    m_default_item_type = parse_item_type(rfl::unwrap(opts->default_type));

    if (rfl::unwrap(opts->verbose_ids)) {
        m_vout.verbose(true);
        m_verbose_ids = true;
    }

    if (const auto& id_files = rfl::unwrap(opts->id_file)) {
        for (const std::string& filename : *id_files) {
            if (filename == "-") {
                if (m_input_filename.empty() || m_input_filename == "-") {
                    throw argument_error{"Can not read OSM input and IDs both from STDIN."};
                }
                m_vout << "Reading IDs from STDIN...\n";
                read_id_file(std::cin, m_ids, m_default_item_type);
            } else {
                std::ifstream id_file{filename};
                if (!id_file.is_open()) {
                    throw argument_error{"Could not open file '" + filename + "'"};
                }
                m_vout << "Reading ID file...\n";
                read_id_file(id_file, m_ids, m_default_item_type);
            }
        }
    }

    if (const auto& osm_files = rfl::unwrap(opts->id_osm_file)) {
        for (const std::string& filename : *osm_files) {
            m_vout << "Reading OSM ID file...\n";
            read_id_osm_file(filename, m_ids);
        }
    }

    if (const auto& id_list = rfl::unwrap(opts->ids)) {
        std::string sids;
        for (const auto& s : *id_list) {
            sids += s + " ";
        }
        for (const auto& s : osmium::split_string(sids, "\t ;,/|", true)) {
            parse_and_add_id(s, m_ids, m_default_item_type);
        }
    }

    if (no_ids(m_ids)) {
        throw argument_error{"Please specify IDs to look for on command line or with option --id-file/-i or --id-osm-file/-I."};
    }

    return true;
}

void CommandGetParents::show_arguments() {
    show_single_input_arguments(m_vout);
    show_output_arguments(m_vout);

    m_vout << "  other options:\n";
    m_vout << "    add self: " << yes_no(m_add_self);
    m_vout << "    default object type: " << osmium::item_type_to_name(m_default_item_type) << "\n";
    if (m_verbose_ids) {
        m_vout << "    looking for these ids:\n";
        m_vout << "      nodes:";
        for (const auto id : m_ids(osmium::item_type::node)) {
            m_vout << " " << id;
        }
        m_vout << "\n";
        m_vout << "      ways:";
        for (const auto id : m_ids(osmium::item_type::way)) {
            m_vout << " " << id;
        }
        m_vout << "\n";
        m_vout << "      relations:";
        for (const auto id : m_ids(osmium::item_type::relation)) {
            m_vout << " " << id;
        }
        m_vout << "\n";
    } else {
        m_vout << "    looking for " << m_ids(osmium::item_type::node).size() << " node ID(s), "
                                     << m_ids(osmium::item_type::way).size() << " way ID(s), and "
                                     << m_ids(osmium::item_type::relation).size() << " relation ID(s)\n";
    }
}

osmium::osm_entity_bits::type CommandGetParents::get_needed_types() const {
    osmium::osm_entity_bits::type types = osmium::osm_entity_bits::relation;

    if (!m_ids(osmium::item_type::node).empty()) {
        if (m_add_self) {
            types |= osmium::osm_entity_bits::node;
        }
        types |= osmium::osm_entity_bits::way;
    }
    if (!m_ids(osmium::item_type::way).empty()) {
        if (m_add_self) {
            types |= osmium::osm_entity_bits::way;
        }
    }

    return types;
}

bool CommandGetParents::run() {
    m_vout << "Opening input file...\n";
    osmium::io::Reader reader{m_input_file, get_needed_types()};

    m_vout << "Opening output file...\n";
    osmium::io::Header header{reader.header()};
    setup_header(header);

    osmium::io::Writer writer{m_output_file, header, m_output_overwrite, m_fsync};

    m_vout << "Copying matching objects to output file...\n";
    osmium::ProgressBar progress_bar{reader.file_size(), display_progress()};
    while (osmium::memory::Buffer buffer = reader.read()) {
        progress_bar.update(reader.offset());
        for (const auto& object : buffer.select<osmium::OSMObject>()) {
            if (m_add_self && m_ids(object.type()).get(object.positive_id())) {
                writer(object);
                continue;
            }
            if (object.type() == osmium::item_type::way) {
                const auto& way = static_cast<const osmium::Way&>(object);
                for (const auto& nr : way.nodes()) {
                    if (m_ids(osmium::item_type::node).get(nr.positive_ref())) {
                        writer(object);
                        break;
                    }
                }
            } else if (object.type() == osmium::item_type::relation) {
                const auto& relation = static_cast<const osmium::Relation&>(object);
                for (const auto& member : relation.members()) {
                    if (m_ids(member.type()).get(member.positive_ref())) {
                        writer(object);
                        break;
                    }
                }
            }
        }
    }
    progress_bar.done();

    m_vout << "Closing output file...\n";
    writer.close();

    m_vout << "Closing input file...\n";
    reader.close();

    show_memory_used();

    m_vout << "Done.\n";

    return true;
}

