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

#include "command_changeset_filter.hpp"

#include "exception.hpp"
#include "util.hpp"

#include <osmium/geom/relations.hpp>
#include <osmium/io/header.hpp>
#include <osmium/io/input_iterator.hpp>
#include <osmium/io/output_iterator.hpp>
#include <osmium/io/reader.hpp>
#include <osmium/io/writer.hpp>
#include <osmium/osm/changeset.hpp>
#include <osmium/osm/entity_bits.hpp>
#include <osmium/osm/timestamp.hpp>
#include <osmium/osm/types.hpp>
#include <osmium/util/progress_bar.hpp>
#include <osmium/util/verbose_output.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct ChangesetFilterOptions {
    osmium_cli::CmdShortOpt<"d", "Changesets with discussions (comments)", osmium_cli::Flag> with_discussion = false;
    osmium_cli::CmdShortOpt<"D", "Changesets without discussions (no comments)", osmium_cli::Flag> without_discussion = false;
    osmium_cli::CmdShortOpt<"c", "Changesets with changes", osmium_cli::Flag> with_changes = false;
    osmium_cli::CmdShortOpt<"C", "Changesets without any changes", osmium_cli::Flag> without_changes = false;
    osmium_cli::CmdOpt<"Open changesets", osmium_cli::Flag> open = false;
    osmium_cli::CmdOpt<"Closed changesets", osmium_cli::Flag> closed = false;
    osmium_cli::CmdShortOpt<"u", "Changesets by given user", osmium_cli::OptStr> user;
    osmium_cli::CmdShortOpt<"U", "Changesets by given user ID", std::optional<osmium::user_id_type>> uid;
    osmium_cli::CmdShortOpt<"a", "Changesets opened after this time", osmium_cli::OptStr> after;
    osmium_cli::CmdShortOpt<"b", "Changesets closed before this time", osmium_cli::OptStr> before;
    osmium_cli::CmdShortOpt<"B", "Changesets overlapping this bounding box", osmium_cli::OptStr> bbox;
    rfl::Flatten<osmium_cli::CommonOptions> common;
    rfl::Flatten<osmium_cli::SingleInputOptions> input;
    rfl::Flatten<osmium_cli::OutputOptions> output;
    rfl::Flatten<osmium_cli::SingleInputPositional> positionals;
};

} // anonymous namespace

bool CommandChangesetFilter::setup(const std::vector<std::string>& arguments) {
    const auto opts = parse_options<ChangesetFilterOptions>(arguments);
    if (!opts) {
        return false;
    }

    setup_common(opts->common());
    setup_input_file(opts->input(), rfl::unwrap(opts->positionals().input_filename));
    setup_output_file(opts->output());

    if (rfl::unwrap(opts->with_discussion)) {
        m_with_discussion = true;
    }

    if (rfl::unwrap(opts->without_discussion)) {
        m_without_discussion = true;
    }

    if (rfl::unwrap(opts->with_changes)) {
        m_with_changes = true;
    }

    if (rfl::unwrap(opts->without_changes)) {
        m_without_changes = true;
    }

    if (rfl::unwrap(opts->open)) {
        m_open = true;
    }

    if (rfl::unwrap(opts->closed)) {
        m_closed = true;
    }

    if (const auto& u = rfl::unwrap(opts->uid)) {
        m_uid = *u;
    }

    if (const auto& u = rfl::unwrap(opts->user)) {
        m_user = *u;
    }

    if (const auto& ts = rfl::unwrap(opts->after)) {
        try {
            m_after = osmium::Timestamp(*ts);
        } catch (const std::invalid_argument&) {
            throw argument_error{"Wrong format for --after/-a timestamp (use YYYY-MM-DDThh:mm:ssZ)."};
        }
    }

    if (const auto& ts = rfl::unwrap(opts->before)) {
        try {
            m_before = osmium::Timestamp(*ts);
        } catch (const std::invalid_argument&) {
            throw argument_error{"Wrong format for --before/-b timestamp (use YYYY-MM-DDThh:mm:ssZ)."};
        }
    }

    if (const auto& b = rfl::unwrap(opts->bbox)) {
        m_box = parse_bbox(*b, "--bbox/-B");
    }

    if (m_with_discussion && m_without_discussion) {
        throw argument_error{"You can not use --with-discussion/-d and --without-discussion/-D together."};
    }

    if (m_with_changes && m_without_changes) {
        throw argument_error{"You can not use --with-changes/-c and --without-changes/-C together."};
    }

    if (m_open && m_closed) {
        throw argument_error{"You can not use --open and --closed together."};
    }

    if (m_after > m_before) {
        throw argument_error{"Timestamp 'after' is after 'before'."};
    }

    return true;
}

void CommandChangesetFilter::show_arguments() {
    show_single_input_arguments(m_vout);
    show_output_arguments(m_vout);
    m_vout << "  other options:\n";
    m_vout << "    changesets must\n";
    if (m_with_discussion) {
        m_vout << "      - have a discussion\n";
    }
    if (m_without_discussion) {
        m_vout << "      - not have a discussion\n";
    }
    if (m_with_changes) {
        m_vout << "      - have at least one change\n";
    }
    if (m_without_changes) {
        m_vout << "      - not have any changes\n";
    }
    if (m_open) {
        m_vout << "      - be open\n";
    }
    if (m_closed) {
        m_vout << "      - be closed\n";
    }
    if (m_uid != 0) {
        m_vout << "      - be from uid " << m_uid << "\n";
    }
    if (!m_user.empty()) {
        m_vout << "      - be from user '" << m_user << "'\n";
    }
    if (m_after > osmium::start_of_time()) {
        m_vout << "      - be closed after " << m_after.to_iso() << " or still open\n";
    }
    if (m_before < osmium::end_of_time()) {
        m_vout << "      - be created before " << m_before.to_iso() << "\n";
    }
}

namespace {

bool changeset_after(const osmium::Changeset& changeset, osmium::Timestamp time) {
    return changeset.open() || changeset.closed_at() >= time;
}

bool changeset_before(const osmium::Changeset& changeset, osmium::Timestamp time) {
    return changeset.created_at() <= time;
}

} // anonymous namespace

bool CommandChangesetFilter::run() {
    m_vout << "Opening input file...\n";
    osmium::io::Reader reader{m_input_file, osmium::osm_entity_bits::changeset};

    auto input = osmium::io::make_input_iterator_range<osmium::Changeset>(reader);

    osmium::io::Header header{reader.header()};
    setup_header(header);

    m_vout << "Opening output file...\n";
    osmium::io::Writer writer{m_output_file, header, m_output_overwrite, m_fsync};
    auto out = osmium::io::make_output_iterator(writer);

    m_vout << "Filtering data...\n";

    osmium::ProgressBar progress_bar{reader.file_size(), display_progress()};

    int count = 0;
    std::copy_if(input.begin(), input.end(), out,
        [this, &count, &progress_bar, &reader](const osmium::Changeset& changeset) {
            if (++count > 10000) {
                progress_bar.update(reader.offset());
                count = 0;
            }
            return (!m_with_discussion    || changeset.num_comments() > 0) &&
                   (!m_without_discussion || changeset.num_comments() == 0) &&
                   (!m_with_changes       || changeset.num_changes() > 0) &&
                   (!m_without_changes    || changeset.num_changes() == 0) &&
                   (!m_open               || changeset.open()) &&
                   (!m_closed             || changeset.closed()) &&
                   (m_uid == 0            || changeset.uid() == m_uid) &&
                   (m_user.empty()        || m_user == changeset.user()) &&
                   changeset_after(changeset, m_after) &&
                   changeset_before(changeset, m_before) &&
                   (!m_box.valid()        || (changeset.bounds().valid() && osmium::geom::overlaps(changeset.bounds(), m_box)));

    });

    progress_bar.done();

    writer.close();
    reader.close();

    show_memory_used();
    m_vout << "Done.\n";

    return true;
}

