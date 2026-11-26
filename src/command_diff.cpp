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

#include "command_diff.hpp"

#include "exception.hpp"
#include "util.hpp"

#include <osmium/io/detail/read_write.hpp>
#include <osmium/io/file.hpp>
#include <osmium/io/file_format.hpp>
#include <osmium/io/input_iterator.hpp>
#include <osmium/io/reader.hpp>
#include <osmium/io/reader_with_progress_bar.hpp>
#include <osmium/io/writer.hpp>
#include <osmium/io/writer_options.hpp>
#include <osmium/memory/item.hpp>
#include <osmium/osm.hpp>
#include <osmium/osm/crc.hpp>
#include <osmium/osm/crc_zlib.hpp>
#include <osmium/osm/item_type.hpp>
#include <osmium/util/verbose_output.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct DiffOptions {
    osmium_cli::CmdOpt<"Ignore changeset id when comparing objects", osmium_cli::Flag> ignore_changeset = false;
    osmium_cli::CmdOpt<"Ignore user id when comparing objects", osmium_cli::Flag> ignore_uid = false;
    osmium_cli::CmdOpt<"Ignore user name when comparing objects", osmium_cli::Flag> ignore_user = false;
    osmium_cli::CmdShortOpt<"t", "Read only objects of given type (node, way, relation)", osmium_cli::OptStrVec> object_type;
    osmium_cli::CmdShortOpt<"o", "Output file", osmium_cli::OptStr> output;
    osmium_cli::CmdShortOpt<"f", "Format of output file", osmium_cli::OptStr> output_format;
    osmium_cli::CmdShortOpt<"O", "Allow existing output file to be overwritten", osmium_cli::Flag> overwrite = false;
    osmium_cli::CmdShortOpt<"q", "Report only when files differ", osmium_cli::Flag> quiet = false;
    osmium_cli::CmdShortOpt<"s", "Show summary on STDERR", osmium_cli::Flag> summary = false;
    osmium_cli::CmdShortOpt<"c", "Suppress common objects", osmium_cli::Flag> suppress_common = false;
    rfl::Flatten<osmium_cli::CommonOptions> common;
    rfl::Flatten<osmium_cli::MultipleInputsOptions> input;
    rfl::Flatten<osmium_cli::MultipleInputsPositional> positionals;
};

} // anonymous namespace

bool CommandDiff::setup(const std::vector<std::string>& arguments) {
    const auto opts = parse_options<DiffOptions>(arguments);
    if (!opts) {
        return false;
    }

    setup_common(opts->common());
    setup_object_type_nwr(rfl::unwrap(opts->object_type));
    setup_input_files(opts->input(), rfl::unwrap(opts->positionals().input_filenames));

    if (m_input_files.size() != 2) {
        throw argument_error("You need exactly two input files for this command.");
    }

    if (rfl::unwrap(opts->ignore_changeset)) {
        m_ignore_attrs_changeset = true;
    }

    if (rfl::unwrap(opts->ignore_uid)) {
        m_ignore_attrs_uid = true;
    }

    if (rfl::unwrap(opts->ignore_user)) {
        m_ignore_attrs_user = true;
    }

    if (const auto& out = rfl::unwrap(opts->output)) {
        m_output_filename = *out;
    }

    if (const auto& fmt = rfl::unwrap(opts->output_format)) {
        m_output_format = *fmt;
    }

    if (rfl::unwrap(opts->overwrite)) {
        m_output_overwrite = osmium::io::overwrite::allow;
    }

    if (rfl::unwrap(opts->summary)) {
        m_show_summary = true;
    }

    if (rfl::unwrap(opts->quiet)) {
        if (rfl::unwrap(opts->output) || rfl::unwrap(opts->output_format) ||
            rfl::unwrap(opts->overwrite) || rfl::unwrap(opts->suppress_common)) {
            throw argument_error("Do not use --quiet/-q with any of the output options.");
        }
        m_output_action = "none";
        m_output_format = "no output";
    }

    if (m_output_format == "compact") {
        m_output_action = "compact";
    }

    if (m_output_action.empty()) {
        if (m_output_format.empty() && (m_output_filename.empty() || m_output_filename == "-")) {
            m_output_format = "compact";
            m_output_action = "compact";
        } else {
            m_output_action = "osm";
            m_output_file = osmium::io::File{m_output_filename, m_output_format};
            m_output_file.check();

            std::string metadata{"version+timestamp"};
            if (!m_ignore_attrs_changeset) {
                metadata += "+changeset";
            }
            if (!m_ignore_attrs_uid) {
                metadata += "+uid";
            }
            if (!m_ignore_attrs_changeset) {
                metadata += "+user";
            }
            m_output_file.set("add_metadata", metadata);

            auto f = m_output_file.format();
            if (f != osmium::io::file_format::opl && f != osmium::io::file_format::debug) {
                throw argument_error("File format does not support diff output. Use 'compact', 'opl' or 'debug' format.");
            }
        }
    }

    if (rfl::unwrap(opts->suppress_common)) {
        m_suppress_common = true;
    }

    return true;
}

void CommandDiff::show_arguments() {
    show_multiple_inputs_arguments(m_vout);
    show_output_arguments(m_vout);

    m_vout << "  other options:\n";
    m_vout << "    show summary: " << yes_no(m_show_summary);
    m_vout << "    suppress common objects: " << yes_no(m_suppress_common);
    show_object_types(m_vout);
}


class OutputAction {

public:

    OutputAction() noexcept = default;

    virtual ~OutputAction() noexcept = default;

    OutputAction(const OutputAction&) = delete;
    OutputAction& operator=(const OutputAction&) = delete;

    OutputAction(OutputAction&&) noexcept = delete;
    OutputAction& operator=(OutputAction&&) noexcept = delete;

    virtual void left(const osmium::OSMObject& /* object */) {
    }

    virtual void right(const osmium::OSMObject& /* object */) {
    }

    virtual void same(const osmium::OSMObject& /* object */) {
    }

    virtual void different(const osmium::OSMObject& /* left */, const osmium::OSMObject& /* right */) {
    }

}; // class OutputAction

class OutputActionCompact : public OutputAction {

    int m_fd;

    void print(const char diff, const osmium::OSMObject& object) const {
        std::stringstream ss;
        ss << diff
           << osmium::item_type_to_char(object.type()) << object.id()
           << " v" << object.version() << '\n';
        osmium::io::detail::reliable_write(m_fd, ss.str().c_str(), ss.str().size());
    }

public:

    explicit OutputActionCompact(int fd) noexcept :
        m_fd(fd) {
    }

    void left(const osmium::OSMObject& object) override {
        print('-', object);
    }

    void right(const osmium::OSMObject& object) override {
        print('+', object);
    }

    void same(const osmium::OSMObject& object) override {
        print(' ', object);
    }

    void different(const osmium::OSMObject& left, const osmium::OSMObject& /* right */) override {
        print('*', left);
    }

}; // class OutputActionCompact

class OutputActionOSM : public OutputAction {

    osmium::io::Writer m_writer;

public:

    OutputActionOSM(const osmium::io::File& file, osmium::io::overwrite ow) :
        m_writer(file, ow) {
    }

    void left(const osmium::OSMObject& object) override {
        m_writer(object);
    }

    void right(const osmium::OSMObject& object) override {
        m_writer(object);
    }

    void same(const osmium::OSMObject& object) override {
        m_writer(object);
    }

    void different(const osmium::OSMObject& left, const osmium::OSMObject& right) override {
        m_writer(left);
        m_writer(right);
    }

}; // class OutputActionOSM

void CommandDiff::update_object_crc(osmium::CRC<osmium::CRC_zlib>* crc, const osmium::OSMObject &object) const {
    crc->update_bool(object.visible());
    crc->update(object.timestamp());
    crc->update(object.tags());
    if (!m_ignore_attrs_changeset) {
        crc->update_int32(object.changeset());
    }
    if (!m_ignore_attrs_uid) {
        crc->update_int32(object.uid());
    }
    if (!m_ignore_attrs_user) {
        crc->update_string(object.user());
    }
}

bool CommandDiff::run() {
    osmium::io::Reader reader1{m_input_files[0], osm_entity_bits()};
    osmium::io::ReaderWithProgressBar reader2{display_progress(), m_input_files[1], osm_entity_bits()};
    auto in1 = osmium::io::make_input_iterator_range<osmium::OSMObject>(reader1);
    auto in2 = osmium::io::make_input_iterator_range<osmium::OSMObject>(reader2);
    auto it1 = in1.begin();
    auto it2 = in2.begin();
    auto end1 = in1.end();
    auto end2 = in2.end();

    std::unique_ptr<OutputAction> action;

    if (m_output_action == "compact") {
        const int fd = osmium::io::detail::open_for_writing(m_output_filename, m_output_overwrite);
        action = std::make_unique<OutputActionCompact>(fd);
    } else if (m_output_action == "osm") {
        m_output_file.set("diff");
        action = std::make_unique<OutputActionOSM>(m_output_file, m_output_overwrite);
    }

    uint64_t count_left = 0;
    uint64_t count_right = 0;
    uint64_t count_same = 0;
    uint64_t count_different = 0;

    while (it1 != end1 || it2 != end2) {
        if (it2 == end2) {
            it1->set_diff(osmium::diff_indicator_type::left);
            ++count_left;
            if (action) {
                action->left(*it1);
            }
            ++it1;
        } else if (it1 == end1 || *it2 < *it1) {
            it2->set_diff(osmium::diff_indicator_type::right);
            ++count_right;
            if (action) {
                action->right(*it2);
            }
            ++it2;
        } else if (*it1 < *it2) {
            it1->set_diff(osmium::diff_indicator_type::left);
            ++count_left;
            if (action) {
                action->left(*it1);
            }
            ++it1;
        } else { /* *it1 == *it2 */
            osmium::CRC<osmium::CRC_zlib> crc1;
            osmium::CRC<osmium::CRC_zlib> crc2;
            update_object_crc(&crc1, *it1);
            update_object_crc(&crc2, *it2);
            switch (it1->type()) {
                case osmium::item_type::node:
                    crc1.update(static_cast<const osmium::Node&>(*it1).location());
                    crc2.update(static_cast<const osmium::Node&>(*it2).location());
                    break;
                case osmium::item_type::way:
                    crc1.update(static_cast<const osmium::Way&>(*it1).nodes());
                    crc2.update(static_cast<const osmium::Way&>(*it2).nodes());
                    break;
                case osmium::item_type::relation:
                    crc1.update(static_cast<const osmium::Relation&>(*it1).members());
                    crc2.update(static_cast<const osmium::Relation&>(*it2).members());
                    break;
                default:
                    break;
            }
            if (crc1().checksum() == crc2().checksum()) {
                ++count_same;
                if (!m_suppress_common) {
                    it1->set_diff(osmium::diff_indicator_type::both);
                    it2->set_diff(osmium::diff_indicator_type::both);
                    if (action) {
                        action->same(*it1);
                    }
                }
            } else {
                ++count_different;
                it1->set_diff(osmium::diff_indicator_type::left);
                it2->set_diff(osmium::diff_indicator_type::right);
                if (action) {
                    action->different(*it1, *it2);
                }
            }
            ++it1;
            ++it2;
        }
    }

    if (m_show_summary) {
        std::cerr << "Summary: left=" << count_left <<
                            " right=" << count_right <<
                             " same=" << count_same <<
                        " different=" << count_different << "\n";
    }

    show_memory_used();

    m_vout << "Done.\n";

    return count_left == 0 && count_right == 0 && count_different == 0;
}

