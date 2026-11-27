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

#include "command_derive_changes.hpp"

#include "exception.hpp"
#include "util.hpp"

#include <osmium/builder/attr.hpp>
#include <osmium/io/file.hpp>
#include <osmium/io/file_format.hpp>
#include <osmium/io/header.hpp>
#include <osmium/io/input_iterator.hpp>
#include <osmium/io/reader.hpp>
#include <osmium/io/reader_with_progress_bar.hpp>
#include <osmium/io/writer.hpp>
#include <osmium/osm/entity_bits.hpp>
#include <osmium/osm/object.hpp>
#include <osmium/util/verbose_output.hpp>

#include <ctime>
#include <string>
#include <vector>

namespace {

struct DeriveChangesOptions {
    osmium_cli::CmdOpt<"Increment version of deleted objects", osmium_cli::Flag> increment_version = false;
    osmium_cli::CmdOpt<"Keep tags (and nodes of ways, members of relations) of deleted objects", osmium_cli::Flag> keep_details = false;
    osmium_cli::CmdOpt<"Set timestamp of deleted objects to current time", osmium_cli::Flag> update_timestamp = false;
    rfl::cli::OptGroup<"COMMON OPTIONS", osmium_cli::CommonOptions> common;
    rfl::cli::OptGroup<"INPUT OPTIONS", osmium_cli::MultipleInputsOptions> input;
    rfl::cli::OptGroup<"OUTPUT OPTIONS", osmium_cli::OutputOptions> output;
    rfl::Flatten<osmium_cli::MultipleInputsPositional> positionals;
};

} // anonymous namespace

bool CommandDeriveChanges::setup(const std::vector<std::string>& arguments) {
    const auto opts = parse_options<DeriveChangesOptions>(arguments);
    if (!opts) {
        return false;
    }

    setup_common(opts->common());
    setup_input_files(opts->input(), rfl::unwrap(opts->positionals().input_filenames));
    setup_output_file(opts->output());

    if (m_input_files.size() != 2) {
        throw argument_error{"You need exactly two input files for this command."};
    }

    if (rfl::unwrap(opts->increment_version)) {
        m_increment_version = true;
    }

    if (rfl::unwrap(opts->keep_details)) {
        m_keep_details = true;
    }

    if (rfl::unwrap(opts->update_timestamp)) {
        m_update_timestamp = true;
    }

    return true;
}

void CommandDeriveChanges::show_arguments() {
    show_multiple_inputs_arguments(m_vout);
    show_output_arguments(m_vout);
    m_vout << "  other options:\n";
    m_vout << "    on deleted objects:\n";
    m_vout << "      increment version: " << yes_no(m_increment_version);
    m_vout << "      keep details: "      << yes_no(m_keep_details);
    m_vout << "      update timestamp: "  << yes_no(m_update_timestamp);
}

void CommandDeriveChanges::write_deleted(osmium::io::Writer& writer, osmium::OSMObject& object) {
    if (m_increment_version) {
        object.set_version(object.version() + 1);
    }

    if (m_update_timestamp) {
        object.set_timestamp(std::time(nullptr));
    }

    if (m_keep_details) {
        object.set_visible(false);
        writer(object);
    } else {
        using namespace osmium::builder::attr; // NOLINT(google-build-using-namespace)
        if (object.type() == osmium::item_type::node) {
            osmium::builder::add_node(m_buffer,
                _deleted(),
                _id(object.id()),
                _version(object.version()),
                _timestamp(object.timestamp())
            );
        } else if (object.type() == osmium::item_type::way) {
            osmium::builder::add_way(m_buffer,
                 _deleted(),
                 _id(object.id()),
                 _version(object.version()),
                 _timestamp(object.timestamp())
             );
        } else if (object.type() == osmium::item_type::relation) {
            osmium::builder::add_relation(m_buffer,
                 _deleted(),
                 _id(object.id()),
                 _version(object.version()),
                 _timestamp(object.timestamp())
             );
        }
        writer(m_buffer.get<osmium::OSMObject>(0));
        m_buffer.clear();
    }
}

bool CommandDeriveChanges::run() {
    m_vout << "Opening output file...\n";
    if (m_output_file.format() != osmium::io::file_format::xml || !m_output_file.is_true("xml_change_format")) {
        warning("Output format chosen is not the XML change format. Use .osc(.gz|bz2) as suffix or -f option.\n");
    }
    osmium::io::Writer writer{m_output_file, m_output_overwrite, m_fsync};

    m_vout << "Opening input files...\n";
    osmium::io::Reader reader1{m_input_files[0], osmium::osm_entity_bits::object};
    osmium::io::ReaderWithProgressBar reader2{display_progress(), m_input_files[1], osmium::osm_entity_bits::object};
    auto in1 = osmium::io::make_input_iterator_range<osmium::OSMObject>(reader1);
    auto in2 = osmium::io::make_input_iterator_range<osmium::OSMObject>(reader2);
    auto it1 = in1.begin();
    auto it2 = in2.begin();
    auto end1 = in1.end();
    auto end2 = in2.end();

    osmium::io::Header header;
    setup_header(header);
    writer.set_header(header);

    reader2.progress_bar().remove();
    m_vout << "Deriving changes...\n";
    while (it1 != end1 || it2 != end2) {
        if (it2 == end2) {
            write_deleted(writer, *it1);
            ++it1;
        } else if (it1 == end1 || *it2 < *it1) {
            writer(*it2);
            ++it2;
        } else if (*it1 < *it2) {
            if (it2->id() != it1->id()) {
                write_deleted(writer, *it1);
            }
            ++it1;
        } else { /* *it1 == *it2 */
            ++it1;
            ++it2;
        }
    }

    writer.close();
    reader2.close();
    reader1.close();

    show_memory_used();
    m_vout << "Done.\n";

    return true;
}

