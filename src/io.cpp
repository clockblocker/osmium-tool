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
#include "util.hpp"

#include <osmium/io/any_input.hpp> // IWYU pragma: keep
#include <osmium/io/any_output.hpp> // IWYU pragma: keep
#include <osmium/io/file.hpp>
#include <osmium/io/header.hpp>
#include <osmium/io/writer_options.hpp>
#include <osmium/util/verbose_output.hpp>

#include <string>
#include <vector>

void with_single_osm_input::setup_input_file(const osmium_cli::SingleInputOptions& opts,
                                             const std::optional<std::string>& filename) {
    if (filename) {
        m_input_filename = *filename;
    }

    if (const auto& fmt = rfl::unwrap(opts.input_format)) {
        m_input_format = *fmt;
    }

    if (m_input_format.empty()) {
        if (m_input_filename == "-") {
            throw argument_error{"When reading from STDIN you need to use the --input-format/-F option\n"
                                 "to specify the file format."};
        }

        if (m_input_filename.empty()) {
            throw argument_error{"Missing input file. Use '-' to read from STDIN and add the --input-format/-F\n"
                                 "option to specify the file format or specify the input file name."};
        }
    }

    m_input_file = osmium::io::File{m_input_filename, m_input_format};
}

void with_single_osm_input::show_single_input_arguments(osmium::VerboseOutput& vout) {
    vout << "  input options:\n";
    vout << "    file name: " << m_input_filename << "\n";
    vout << "    file format: " << m_input_format << "\n";
}

void with_multiple_osm_inputs::setup_input_files(const osmium_cli::MultipleInputsOptions& opts,
                                                 const std::optional<std::vector<std::string>>& filenames) {
    if (filenames) {
        m_input_filenames = *filenames;
    } else {
        m_input_filenames.emplace_back("-"); // default is stdin
    }

    bool uses_stdin = false;
    for (const auto& filename : m_input_filenames) {
        if (filename == "-") {
            if (uses_stdin) {
                throw argument_error{"Can read at most one file from STDIN."};
            }
            uses_stdin = true;
        }
    }

    if (const auto& fmt = rfl::unwrap(opts.input_format)) {
        m_input_format = *fmt;
    }

    if (uses_stdin && m_input_format.empty()) {
        throw argument_error{"When reading from STDIN you need to use the --input-format/-F option\n"
                             "to specify the file format. Or are you missing a file name argument?"};
    }

    for (const std::string& input_filename : m_input_filenames) {
        const osmium::io::File input_file{input_filename, m_input_format};
        m_input_files.push_back(input_file);
    }
}


void with_multiple_osm_inputs::show_multiple_inputs_arguments(osmium::VerboseOutput& vout) {
    vout << "  input options:\n";
    vout << "    file names: \n";
    for (const auto& fn : m_input_filenames) {
        vout << "      " << fn << "\n";
    }
    vout << "    file format: " << m_input_format << "\n";
}

void with_osm_output::check_output_file() {
    if (m_output_format.empty()) {
        if (m_output_filename == "-") {
            throw argument_error{"When writing to STDOUT you need to use the --output-format/-f\n"
                                 "option to specify the file format."};
        }
        if (m_output_filename.empty()) {
            throw argument_error{"Missing output file. Set the output file with --output/-o and/or\n"
                                 "add the --output-format/-f option to specify the file format."};
        }
    }

    m_output_file = osmium::io::File{m_output_filename, m_output_format};
    m_output_file.check();
}

void with_osm_output::init_output_file(const osmium_cli::OutputOptions& opts) {
    if (const auto& gen = rfl::unwrap(opts.generator)) {
        m_generator = *gen;
    }

    if (const auto& out = rfl::unwrap(opts.output)) {
        m_output_filename = *out;
    }

    if (const auto& fmt = rfl::unwrap(opts.output_format)) {
        m_output_format = *fmt;
    }

    if (const auto& hdrs = rfl::unwrap(opts.output_header)) {
        m_output_headers = *hdrs;
    }

    if (rfl::unwrap(opts.overwrite)) {
        m_output_overwrite = osmium::io::overwrite::allow;
    }

    if (rfl::unwrap(opts.fsync)) {
        m_fsync = osmium::io::fsync::yes;
    }
}

void with_osm_output::setup_output_file(const osmium_cli::OutputOptions& opts) {
    init_output_file(opts);
    check_output_file();
}

void with_osm_output::show_output_arguments(osmium::VerboseOutput& vout) {
    vout << "  output options:\n";
    vout << "    file name: " << m_output_filename << "\n";
    vout << "    file format: " << m_output_format << "\n";
    vout << "    generator: " << m_generator << "\n";
    vout << "    overwrite: " << yes_no(m_output_overwrite == osmium::io::overwrite::allow);
    vout << "    fsync: " << yes_no(m_fsync == osmium::io::fsync::yes);
    if (!m_output_headers.empty()) {
        vout << "    output header:\n";
        for (const auto& h : m_output_headers) {
            vout << "      " << h << "\n";
        }
    }
}

void with_osm_output::setup_header(osmium::io::Header& header) const {
    header.set("generator", m_generator);
    for (const auto& h : m_output_headers) {
        header.set(h);
    }
}

void init_header(osmium::io::Header& header, const osmium::io::Header& input_header, const std::vector<std::string>& options) {
    for (const auto& h : options) {
        if (!h.empty() && h.back() == '!') {
            std::string hc{h};
            hc.resize(h.size() - 1);
            header.set(hc, input_header.get(hc));
        } else {
            header.set(h);
        }
    }
}

void with_osm_output::setup_header(osmium::io::Header& header, const osmium::io::Header& input_header) const {
    header.set("generator", m_generator);
    init_header(header, input_header, m_output_headers);
}

