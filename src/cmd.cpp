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

#include "cmd.hpp"

#include "exception.hpp"

#include <osmium/index/map.hpp>
#include <osmium/osm/entity_bits.hpp>
#include <osmium/osm/location.hpp>
#include <osmium/osm/types.hpp>
#include <osmium/util/memory.hpp>
#include <osmium/util/verbose_output.hpp>

#include <cstdlib>
#include <iostream>
#include <vector>

void Command::setup_common(const osmium_cli::CommonOptions& opts) {
    if (opts.verbose) {
        m_vout.verbose(true);
    }

    const bool progress = *opts.progress;
    const bool no_progress = *opts.no_progress;
    if (progress && no_progress) {
        throw argument_error{"Can not use --progress and --no-progress together."};
    }

    if (progress) {
        m_display_progress = display_progress_type::always;
    }

    if (no_progress) {
        m_display_progress = display_progress_type::never;
    }
}

void Command::setup_common(const osmium_cli::CommonOptionsNoProgress& opts) {
    if (opts.verbose) {
        m_vout.verbose(true);
    }
}

void Command::setup_object_type_nwrc(const std::optional<std::vector<std::string>>& types) {
    if (types) {
        m_osm_entity_bits = osmium::osm_entity_bits::nothing;
        for (const auto& t : *types) {
            if (t == "n" || t == "node") {
                m_osm_entity_bits |= osmium::osm_entity_bits::node;
            } else if (t == "w" || t == "way") {
                m_osm_entity_bits |= osmium::osm_entity_bits::way;
            } else if (t == "r" || t == "relation") {
                m_osm_entity_bits |= osmium::osm_entity_bits::relation;
            } else if (t == "c" || t == "changeset") {
                m_osm_entity_bits |= osmium::osm_entity_bits::changeset;
            } else {
                throw argument_error{std::string{"Unknown object type '"}
                                     + t
                                     + "' (Allowed are 'node', 'way', 'relation', and 'changeset')."};
            }
        }
    } else {
        m_osm_entity_bits = osmium::osm_entity_bits::all;
    }
}

void Command::setup_object_type_nwr(const std::optional<std::vector<std::string>>& types) {
    if (types) {
        m_osm_entity_bits = osmium::osm_entity_bits::nothing;
        for (const auto& t : *types) {
            if (t == "n" || t == "node") {
                m_osm_entity_bits |= osmium::osm_entity_bits::node;
            } else if (t == "w" || t == "way") {
                m_osm_entity_bits |= osmium::osm_entity_bits::way;
            } else if (t == "r" || t == "relation") {
                m_osm_entity_bits |= osmium::osm_entity_bits::relation;
            } else {
                throw argument_error{std::string{"Unknown object type '"} + t + "' (Allowed are 'node', 'way', and 'relation')."};
            }
        }
    } else {
        m_osm_entity_bits = osmium::osm_entity_bits::nwr;
    }
}

void Command::show_object_types(osmium::VerboseOutput& vout) {
    vout << "    object types:";
    if (osm_entity_bits() & osmium::osm_entity_bits::node) {
        vout << " node";
    }
    if (osm_entity_bits() & osmium::osm_entity_bits::way) {
        vout << " way";
    }
    if (osm_entity_bits() & osmium::osm_entity_bits::relation) {
        vout << " relation";
    }
    if (osm_entity_bits() & osmium::osm_entity_bits::changeset) {
        vout << " changeset";
    }
    m_vout << '\n';
}

void Command::print_arguments(const std::string& command) {
    if (m_vout.verbose()) {
        m_vout << "Started osmium " << command << '\n'
               << "  " << get_osmium_long_version() << '\n'
               << "  " << get_libosmium_version() << '\n'
               << "Command line options and default settings:\n";
        show_arguments();
    }
}

void Command::show_memory_used() {
    const osmium::MemoryUsage mem;
    if (mem.current() > 0) {
        m_vout << "Peak memory used: " << mem.peak() << " MBytes\n";
    }
}

std::string check_index_type(const std::string& index_type_name, bool allow_none) {
    if (allow_none && index_type_name == "none") {
        return index_type_name;
    }

    std::string type{index_type_name};
    const auto pos = type.find(',');
    if (pos != std::string::npos) {
        type.resize(pos);
    }

    const auto& map_factory = osmium::index::MapFactory<osmium::unsigned_object_id_type, osmium::Location>::instance();
    if (!map_factory.has_map_type(type)) {
        throw argument_error{"Unknown index type '" + index_type_name + "'. Use --show-index-types or -I to get a list."};
    }

    return index_type_name;
}
