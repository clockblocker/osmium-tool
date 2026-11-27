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

#include "command_query_locations_index.hpp"

#include "exception.hpp"
#include "util.hpp"

#include <osmium/builder/osm_object_builder.hpp>
#include <osmium/index/map/dense_file_array.hpp>
#include <osmium/io/writer.hpp>
#include <osmium/memory/buffer.hpp>
#include <osmium/osm/location.hpp>
#include <osmium/osm/types.hpp>
#include <osmium/osm/types_from_string.hpp>
#include <osmium/util/verbose_output.hpp>

#include <cstring>
#include <iostream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

struct QueryLocationsIndexOptions {
    osmium_cli::CmdShortOpt<"i", "Index file name (required)", osmium_cli::OptStr> index_file;
    osmium_cli::CmdOpt<"Dump all locations to STDOUT", osmium_cli::Flag> dump = false;
    rfl::cli::OptGroup<"COMMON OPTIONS", osmium_cli::CommonOptionsNoProgress> common;
    rfl::cli::OptGroup<"OUTPUT OPTIONS", osmium_cli::OutputOptions> output;
    rfl::Hidden<rfl::Positional<osmium_cli::OptStr>> node_id;
};

} // anonymous namespace

bool CommandQueryLocationsIndex::setup(const std::vector<std::string>& arguments) {
    const auto opts = parse_options<QueryLocationsIndexOptions>(arguments);
    if (!opts) {
        return false;
    }

    setup_common(opts->common());
    init_output_file(opts->output());

    if (const auto& idx = rfl::unwrap(opts->index_file)) {
        m_index_file_name = *idx;
    } else {
        throw argument_error{"Missing --index-file,-i option."};
    }

    if (rfl::unwrap(opts->dump)) {
        m_dump = true;
        if ((m_output_filename.empty() || m_output_filename == "-") && m_output_format.empty()) {
            m_output_format = "opl,add_metadata=none";
        }
        if (m_output_format.empty()) {
            m_output_file = osmium::io::File{m_output_filename};
            m_output_file.set("add_metadata", "none");
        } else {
            m_output_file = osmium::io::File{m_output_filename, m_output_format};
        }
        m_output_file.check();
    }

    if (const auto& id = rfl::unwrap(opts->node_id)) {
        if (m_dump) {
            throw argument_error{"Either use --dump or use node ID, not both."};
        }
        const auto r = osmium::string_to_object_id(id->c_str(), osmium::osm_entity_bits::node, osmium::item_type::node);
        m_id = r.second;
    } else if (!m_dump) {
        throw argument_error{"Missing node ID on command line."};
    }

    return true;
}

void CommandQueryLocationsIndex::show_arguments() {
    show_output_arguments(m_vout);
    m_vout << "  other options:\n";
    m_vout << "    index file: " << m_index_file_name << '\n';
}

bool CommandQueryLocationsIndex::run() {
    const int fd = ::open(m_index_file_name.c_str(), O_RDWR);
    if (fd == -1) {
        throw std::system_error{errno, std::system_category(), std::string("Can not open index file '") + m_index_file_name + "'"};
    }

    const osmium::index::map::DenseFileArray<osmium::unsigned_object_id_type, osmium::Location> location_index{fd};

    if (m_dump) {
        const std::size_t max_buffer_size = 11UL * 1024UL * 1024UL;
        osmium::memory::Buffer buffer{max_buffer_size};

        m_vout << "Opening output file...\n";
        osmium::io::Header header{};
        setup_header(header);
        osmium::io::Writer writer{m_output_file, header, m_output_overwrite, m_fsync};

        m_vout << "Dumping index as OSM file...\n";
        for (std::size_t i = 0; i < location_index.size(); ++i) {
            if (location_index.get_noexcept(i).valid()) {
                osmium::builder::NodeBuilder builder{buffer};
                builder.set_id(static_cast<osmium::object_id_type>(i));
                builder.set_location(location_index.get_noexcept(i));
            }
            buffer.commit();
            if (buffer.committed() > 10UL * 1024UL * 1024UL) {
                writer(std::move(buffer));
                buffer = osmium::memory::Buffer{max_buffer_size};
            }
        }
        if (buffer.committed() > 0) {
            writer(std::move(buffer));
        }
    } else {
        m_vout << "Looking up location in index...\n";
        const auto location = location_index.get(m_id);
        std::cout << location << '\n';
    }

    m_vout << "Done.\n";

    return true;
}

