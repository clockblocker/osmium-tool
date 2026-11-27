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

#include "command_tags_count.hpp"

#include "exception.hpp"
#include "util.hpp"

#include <osmium/io/reader.hpp>
#include <osmium/osm.hpp>
#include <osmium/util/progress_bar.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

void CommandTagsCount::add_matcher(const std::string& expression) {
    bool has_value_matcher = false;
    auto matcher = get_tag_matcher(expression, &has_value_matcher);
    if (has_value_matcher) {
        m_tags_filter.add_rule(true, std::move(matcher));
    } else {
        m_keys_filter.add_rule(true, std::move(matcher));
    }
}

void CommandTagsCount::read_expressions_file(const std::string& file_name) {
    m_vout << "Reading expressions file...\n";

    std::ifstream file{file_name};
    if (!file.is_open()) {
        throw argument_error{"Could not open file '" + file_name + "'"};
    }

    for (std::string line; std::getline(file, line);) {
        const auto pos = line.find_first_of('#');
        if (pos != std::string::npos) {
            line.erase(pos);
        }
        if (!line.empty()) {
            if (line.back() == '\r') {
                line.resize(line.size() - 1);
            }
            add_matcher(line);
        }
    }
}

namespace {

sort_func_type get_sort_function(const std::string& sort_order) {
    static const std::pair<std::string, sort_func_type> sort_options[] = {
        { "count-desc", [](const element_type& a, const element_type& b){
                if (a.count == b.count) {
                    return *a.name < *b.name;
                }
                return a.count > b.count;
            }
        },
        { "count-asc", [](const element_type& a, const element_type& b){
                if (a.count == b.count) {
                    return *a.name < *b.name;
                }
                return a.count < b.count;
            }
        },
        { "name-desc", [](const element_type& a, const element_type& b){
                return *a.name > *b.name;
            }
        },
        { "name-asc", [](const element_type& a, const element_type& b){
                return *a.name < *b.name;
            }
        }
    };

    for (const auto& nf : sort_options) {
        if (nf.first == sort_order) {
            return nf.second;
        }
    }

    throw argument_error{"Unknown sort order '" + sort_order + "'"};
}

struct TagsCountOptions {
    osmium_cli::CmdShortOpt<"e", "Read tag expressions from file", osmium_cli::OptStr> expressions;
    osmium_cli::CmdShortOpt<"m", "Min count shown (default: 0)", std::optional<counter_type>> min_count;
    osmium_cli::CmdShortOpt<"M", "Max count shown (default: none)", std::optional<counter_type>> max_count;
    osmium_cli::CmdShortOpt<"o", "Output file (default: stdout)", osmium_cli::OptStr> output;
    osmium_cli::CmdShortOpt<"O", "Allow existing output file to be overwritten", osmium_cli::Flag> overwrite = false;
    osmium_cli::CmdShortOpt<"s", "Sort order of results ('count-asc', 'count-desc' (default), 'name-asc', or 'name-desc')", osmium_cli::OptStr> sort;
    osmium_cli::CmdShortOpt<"t", "Read only objects of given type (node, way, relation)", osmium_cli::OptStrVec> object_type;
    rfl::cli::OptGroup<"COMMON OPTIONS", osmium_cli::CommonOptions> common;
    rfl::cli::OptGroup<"INPUT OPTIONS", osmium_cli::SingleInputOptions> input;
    rfl::Hidden<rfl::Positional<osmium_cli::OptStr>> input_filename;
    rfl::Hidden<rfl::Positional<osmium_cli::OptStrVec>> expression_list;
};

} // anonymous namespace

osmium_cli::CommandSpec CommandTagsCount::describe_options() const {
    return describe_command_options<TagsCountOptions>();
}

bool CommandTagsCount::setup(const std::vector<std::string>& arguments) {
    const auto opts = parse_options<TagsCountOptions>(arguments);
    if (!opts) {
        return false;
    }

    setup_common(opts->common());
    setup_object_type_nwr(rfl::unwrap(opts->object_type));
    setup_input_file(opts->input(), rfl::unwrap(opts->input_filename));

    if (const auto& out = rfl::unwrap(opts->output)) {
        m_output_filename = *out;
    }

    if (rfl::unwrap(opts->overwrite)) {
        m_output_overwrite = osmium::io::overwrite::allow;
    }

    if (const auto& exprs = rfl::unwrap(opts->expression_list)) {
        for (const auto& e : *exprs) {
            add_matcher(e);
        }
    } else {
        m_keys_filter = osmium::TagsFilter{true};
    }

    if (const auto& expr_file = rfl::unwrap(opts->expressions)) {
        read_expressions_file(*expr_file);
    }

    if (const auto& mc = rfl::unwrap(opts->min_count)) {
        m_min_count = *mc;
    }

    if (const auto& mc = rfl::unwrap(opts->max_count)) {
        m_max_count = *mc;
    }

    if (const auto& s = rfl::unwrap(opts->sort)) {
        m_sort_order = *s;
        if (m_sort_order == "name") {
            m_sort_order = "name-asc";
        } else if (m_sort_order == "count") {
            m_sort_order = "count-desc";
        }
    }

    m_sort_func = get_sort_function(m_sort_order);

    return true;
}

void CommandTagsCount::show_arguments() {
    show_single_input_arguments(m_vout);

    m_vout << "  output options:\n";
    m_vout << "    file name: " << m_output_filename << '\n';
    m_vout << "    overwrite: " << yes_no(m_output_overwrite == osmium::io::overwrite::allow);
    m_vout << "  other options:\n";
    m_vout << "    sort order: " << m_sort_order << '\n';
    m_vout << "    min count: " << m_min_count << '\n';

    if (m_max_count == std::numeric_limits<counter_type>::max()) {
        m_vout << "    max count: (none)\n";
    } else {
        m_vout << "    max count: " << m_max_count << '\n';
    }
}

std::vector<element_type> CommandTagsCount::sort_results() const {
    std::vector<element_type> results;

    results.reserve(m_counts.size());
    for (const auto& c : m_counts) {
        if (c.second >= m_min_count && c.second <= m_max_count) {
            results.emplace_back(c.first, c.second);
        }
    }

    std::sort(results.begin(), results.end(), m_sort_func);

    return results;
}

namespace {

void append_escaped(std::string* out, const char* str) {
    *out += '"';
    for (; *str != '\0'; ++str) {
        if (*str == '"') {
            *out += '"';
        }
        *out += *str;
    }
    *out += '"';
}

void write_results(const std::vector<element_type>& results, int fd) {
    std::string out;
    const std::size_t buffer_size = 1024UL * 1024UL;
    out.reserve(buffer_size);

    for (const auto& c : results) {
        out += std::to_string(c.count);
        out += '\t';
        append_escaped(&out, c.name->key());
        const char* value = c.name->value();
        if (value) {
            out += '\t';
            append_escaped(&out, value);
        }
        out += '\n';
        if (out.size() > (buffer_size - 1000)) {
            osmium::io::detail::reliable_write(fd, out.data(), out.size());
            out.clear();
        }
    }

    osmium::io::detail::reliable_write(fd, out.data(), out.size());

    close(fd);
}

} // anonymous namespace

bool CommandTagsCount::run() {
    m_vout << "Opening input file...\n";
    osmium::io::Reader reader{m_input_file, osm_entity_bits(), osmium::io::read_meta::no};

    m_vout << "Opening output file...\n";
    int fd = 1;
    if (!m_output_filename.empty()) {
        fd = osmium::io::detail::open_for_writing(m_output_filename,
                                                  m_output_overwrite);
    }

    m_vout << "Count matching keys/tags...\n";
    osmium::ProgressBar progress_bar{reader.file_size(), display_progress()};
    while (osmium::memory::Buffer buffer = reader.read()) {
        progress_bar.update(reader.offset());
        for (const auto& object : buffer.select<osmium::OSMObject>()) {
            for (const auto& tag : object.tags()) {
                if (m_keys_filter(tag)) {
                    ++m_counts[tag.key()];
                }
                if (m_tags_filter(tag)) {
                    ++m_counts[tag];
                }
            }
        }
    }
    progress_bar.done();

    m_vout << "Closing input file...\n";
    reader.close();

    show_memory_used();

    m_vout << "Sorting results...\n";
    const auto results = sort_results();

    show_memory_used();

    m_vout << "Writing results...\n";
    write_results(results, fd);

    m_vout << "Done.\n";

    return true;
}

