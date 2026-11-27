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

#include "command_create_locations_index.hpp"

#include "exception.hpp"
#include "util.hpp"

#include <osmium/index/map/dense_file_array.hpp>
#include <osmium/io/file.hpp>
#include <osmium/io/reader.hpp>
#include <osmium/memory/buffer.hpp>
#include <osmium/osm/location.hpp>
#include <osmium/osm/node.hpp>
#include <osmium/osm/types.hpp>
#include <osmium/util/progress_bar.hpp>
#include <osmium/util/verbose_output.hpp>
#include <osmium/visitor.hpp>

#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>
#include <vector>

namespace {

struct CreateLocationsIndexOptions {
    osmium_cli::CmdShortOpt<"i", "Index file name (required)", osmium_cli::OptStr> index_file;
    osmium_cli::CmdShortOpt<"u", "Update existing index file", osmium_cli::Flag> update = false;
    rfl::cli::OptGroup<"COMMON OPTIONS", osmium_cli::CommonOptions> common;
    rfl::cli::OptGroup<"INPUT OPTIONS", osmium_cli::SingleInputOptions> input;
    rfl::Flatten<osmium_cli::SingleInputPositional> positionals;
};

} // anonymous namespace

osmium_cli::CommandSpec CommandCreateLocationsIndex::describe_options() const {
    return describe_command_options<CreateLocationsIndexOptions>();
}

bool CommandCreateLocationsIndex::setup(const std::vector<std::string>& arguments) {
    const auto opts = parse_options<CreateLocationsIndexOptions>(arguments);
    if (!opts) {
        return false;
    }

    setup_common(opts->common());
    setup_input_file(opts->input(), rfl::unwrap(opts->positionals().input_filename));

    if (const auto& idx = rfl::unwrap(opts->index_file)) {
        m_index_file_name = *idx;
    } else {
        throw argument_error{"Missing --index-file,-i option."};
    }

    if (rfl::unwrap(opts->update)) {
        m_update = true;
    }

    return true;
}

void CommandCreateLocationsIndex::show_arguments() {
    show_single_input_arguments(m_vout);

    m_vout << "  other options:\n";
    m_vout << "    index file: " << m_index_file_name << '\n';
    m_vout << "    allow update of existing index file: " << yes_no(m_update);
}

bool CommandCreateLocationsIndex::run() {
    int flags = O_RDWR | O_CREAT; // NOLINT(hicpp-signed-bitwise)

    if (!m_update) {
        flags |= O_EXCL; // NOLINT(hicpp-signed-bitwise)
    }

#ifdef _WIN32
    flags |= O_BINARY; // NOLINT(hicpp-signed-bitwise)
#endif

    const int fd = ::open(m_index_file_name.c_str(), flags, 0666);
    if (fd == -1) {
        if (errno == EEXIST) {
            throw argument_error{"Index file exists and you haven't specified --update/-u."};
        }
        throw std::system_error{errno, std::system_category(), std::string("Can not open index file '") + m_index_file_name + "'"};
    }

    osmium::index::map::DenseFileArray<osmium::unsigned_object_id_type, osmium::Location> location_index{fd};

    m_vout << "Reading input file '" << m_input_file.filename() << "'\n";
    osmium::io::Reader reader{m_input_file, osmium::osm_entity_bits::node};

    osmium::ProgressBar progress_bar{reader.file_size(), display_progress()};
    while (const auto buffer = reader.read()) {
        progress_bar.update(reader.offset());
        osmium::apply(buffer, [&](const osmium::Node& node) {
            location_index.set(node.positive_id(), node.location());
        });
    }
    progress_bar.done();

    reader.close();

    m_vout << "About " << (location_index.used_memory() / (1024LLU * 1024LLU * 1024LLU)) << " GBytes used for node location index on disk.\n";
    m_vout << "Done.\n";

    return true;
}

