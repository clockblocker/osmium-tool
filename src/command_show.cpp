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

#include "command_show.hpp"

#include "exception.hpp"
#include "util.hpp"

#include <osmium/io/file.hpp>
#include <osmium/io/header.hpp>
#include <osmium/io/reader.hpp>
#include <osmium/io/writer.hpp>
#include <osmium/memory/buffer.hpp>
#include <osmium/util/verbose_output.hpp>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifndef _WIN32
# include <unistd.h>
#endif

#ifndef _WIN32
void CommandShow::setup_pager_from_env() noexcept {
    m_pager = "less";
    const char* pager = ::getenv("OSMIUM_PAGER"); // NOLINT(concurrency-mt-unsafe)
    if (pager) {
        m_pager = pager;
    } else {
        pager = ::getenv("PAGER"); // NOLINT(concurrency-mt-unsafe)
        if (pager) {
            m_pager = pager;
        }
    }

    if (m_pager == "cat") {
        m_pager = "";
    }
}
#endif

namespace {

struct ShowOptions {
    osmium_cli::CmdShortOpt<"d", "Use debug format", osmium_cli::Flag> format_debug = false;
    osmium_cli::CmdShortOpt<"o", "Use OPL format", osmium_cli::Flag> format_opl = false;
    osmium_cli::CmdShortOpt<"x", "Use XML format", osmium_cli::Flag> format_xml = false;
#ifndef _WIN32
    osmium_cli::CmdOpt<"Do not run pager program", osmium_cli::Flag> no_pager = false;
#endif
    osmium_cli::CmdShortOpt<"t", "Read only objects of given type (node, way, relation, changeset)", osmium_cli::OptStrVec> object_type;
    osmium_cli::CmdShortOpt<"f", "Format of output file", osmium_cli::OptStr> output_format;
    rfl::cli::OptGroup<"COMMON OPTIONS", osmium_cli::CommonOptionsNoProgress> common;
    rfl::cli::OptGroup<"INPUT OPTIONS", osmium_cli::SingleInputOptions> input;
    rfl::Flatten<osmium_cli::SingleInputPositional> positionals;
};

} // anonymous namespace

bool CommandShow::setup(const std::vector<std::string>& arguments) {
    const auto opts = parse_options<ShowOptions>(arguments);
    if (!opts) {
        return false;
    }

    setup_common(opts->common());
    setup_object_type_nwrc(rfl::unwrap(opts->object_type));
    setup_input_file(opts->input(), rfl::unwrap(opts->positionals().input_filename));

#ifndef _WIN32
    if (rfl::unwrap(opts->no_pager)) {
        m_pager = "";
    } else {
        setup_pager_from_env();
    }
#endif

    const auto& fmt = rfl::unwrap(opts->output_format);
    const bool fmt_debug = rfl::unwrap(opts->format_debug);
    const bool fmt_opl = rfl::unwrap(opts->format_opl);
    const bool fmt_xml = rfl::unwrap(opts->format_xml);

    if (fmt && fmt_debug && fmt_opl && fmt_xml) {
        throw argument_error{"You can only use at most one of the following options: --output-format/-f, --format-debug/-d, --format-opl/-o, and --format-xml/-x."};
    }

    if (fmt) {
        m_output_format = *fmt;
    } else if (fmt_debug) {
        m_output_format = "debug,color=true";
    } else if (fmt_opl) {
        m_output_format = "opl";
    } else if (fmt_xml) {
        m_output_format = "xml";
    } else {
        const char* output_format_from_env = ::getenv("OSMIUM_SHOW_FORMAT"); // NOLINT(concurrency-mt-unsafe)
        if (output_format_from_env) {
            m_output_format = output_format_from_env;
        }
    }

    m_color_output = m_output_format.find("color=true") != std::string::npos;

    return true;
}

void CommandShow::show_arguments() {
    show_single_input_arguments(m_vout);
    m_vout << "  other options:\n";
    m_vout << "    file format: " << m_output_format << "\n";
    m_vout << "    use color: " << yes_no(m_color_output);
    m_vout << "    use pager: " << (m_pager.empty() ? "(no pager)" : m_pager) << "\n";
    show_object_types(m_vout);
}

#ifndef _WIN32
namespace {

int execute_pager(const std::string& pager, bool with_color) {
    int pipefd[2];
    if (::pipe(pipefd) < 0) { // NOLINT(cppcoreguidelines-pro-bounds-array-to-pointer-decay,hicpp-no-array-decay)
        throw std::system_error{errno, std::system_category(), "Could not run pager: pipe() call failed"};
    }

    const pid_t pid = fork();
    if (pid < 0) {
        throw std::system_error{errno, std::system_category(), "Could not run pager: fork() call failed"};
    }

    if (pid == 0) {
        // child
        ::close(pipefd[1]); // close write end of the pipe
        ::close(0); // close stdin
        if (::dup2(pipefd[0], 0) < 0) { // put end of pipe as stdin
            throw std::system_error{errno, std::system_category(), "Could not run pager: dup2() call failed"};
        }

        if (with_color && pager.size() >= 4 && pager.substr(pager.size() - 4, 4) == "less") {
            ::execlp(pager.c_str(), pager.c_str(), "-R", nullptr);
        } else {
            // execute pager without arguments
            ::execlp(pager.c_str(), pager.c_str(), nullptr);
        }

        // Exec will either succeed and never return here, or it fails and
        // we'll exit.
        throw std::system_error{errno, std::system_category(), "Could not run pager: execlp() call failed"};
    }

    // parent
    ::close(pipefd[0]); // close read end of the pipe

    return pipefd[1];
}

} // anonymous namespace
#endif

bool CommandShow::run() {
    osmium::io::Reader reader{m_input_file, osm_entity_bits()};
    const osmium::io::Header header{reader.header()};

    if (m_pager.empty()) {
        const osmium::io::File file{"-", m_output_format};
        osmium::io::Writer writer{file, header};
        while (osmium::memory::Buffer buffer = reader.read()) {
            writer(std::move(buffer));
        }
        writer.close();
    } else {
#ifndef _WIN32
        const int fd = execute_pager(m_pager, m_color_output);

        if (::dup2(fd, 1) < 0) { // put end of pipe as stdout
            throw std::system_error{errno, std::system_category(), "Could not run pager: dup2() call failed"};
        }
        ::close(fd);

        const osmium::io::File file{"-", m_output_format};
        osmium::io::Writer writer{file, header};
        try {
            while (osmium::memory::Buffer buffer = reader.read()) {
                writer(std::move(buffer));
            }
        } catch (const std::system_error& e) {
            if (e.code().value() != EPIPE) {
                throw;
            }
        }

        writer.close();
        ::close(1); // close stdout to signal EOF to pager

        int status = 0;
        const int pid = ::wait(&status);
        if (pid < 0) {
            throw std::system_error{errno, std::system_category(), "Could not run pager: wait() call failed"};
        }
        if (WIFEXITED(status) && WEXITSTATUS(status) == 1) { // NOLINT(hicpp-signed-bitwise)
            throw argument_error{std::string{"Could not run pager '"} + m_pager + "'"};
        }
#endif
    }

    reader.close();

    return true;
}

