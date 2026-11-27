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

#include "export_format_json.hpp"

#include "../exception.hpp"
#include "../util.hpp"

#include <osmium/geom/coordinates.hpp>
#include <osmium/io/detail/read_write.hpp>
#include <osmium/osm.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

static constexpr const std::size_t initial_buffer_size = 1024UL * 1024UL;
static constexpr const std::size_t flush_buffer_size   =  800UL * 1024UL;

namespace export_json {

using UniqueIdValue = std::variant<std::uint64_t, std::string>;
using PropertyValue = std::variant<std::string, std::int64_t, std::uint64_t, std::vector<std::int64_t>>;

struct GeoJSONGeometry {
    std::string type;
    rfl::json::Raw coordinates;
};

struct GeoJSONFeature {
    std::string type{"Feature"};
    std::optional<UniqueIdValue> id;
    GeoJSONGeometry geometry;
    rfl::Object<PropertyValue> properties;
};

} // namespace export_json

namespace {

void append_coordinate(std::string* buffer, double coord) {
    std::array<char, 20> tmp{};
    auto n = std::snprintf(tmp.data(), tmp.size(), "%.7f", coord);

    // remove trailing zeros
    while (n >= 2 && tmp[static_cast<std::size_t>(n - 1)] == '0' && tmp[static_cast<std::size_t>(n - 2)] != '.') {
        --n;
    }

    buffer->append(tmp.data(), static_cast<std::size_t>(n));
}

} // anonymous namespace

void ExportFormatJSON::FdStreamBuf::set_fd(int fd) {
    m_fd = fd;
    m_buffer.reserve(initial_buffer_size);
}

void ExportFormatJSON::FdStreamBuf::flush_buffer() {
    if (m_fd >= 0 && !m_buffer.empty()) {
        osmium::io::detail::reliable_write(m_fd, m_buffer.data(), m_buffer.size());
        m_buffer.clear();
    }
}

void ExportFormatJSON::FdStreamBuf::flush_all() {
    flush_buffer();
}

std::streambuf::int_type ExportFormatJSON::FdStreamBuf::overflow(int_type ch) {
    if (ch == traits_type::eof()) {
        return traits_type::not_eof(ch);
    }
    m_buffer.push_back(traits_type::to_char_type(ch));
    if (m_buffer.size() > flush_buffer_size) {
        flush_buffer();
    }
    return ch;
}

std::streamsize ExportFormatJSON::FdStreamBuf::xsputn(const char* s, std::streamsize n) {
    m_buffer.append(s, static_cast<std::size_t>(n));
    if (m_buffer.size() > flush_buffer_size) {
        flush_buffer();
    }
    return n;
}

int ExportFormatJSON::FdStreamBuf::sync() {
    flush_buffer();
    return 0;
}

ExportFormatJSON::ExportFormatJSON(const std::string& output_format,
                                   const std::string& output_filename,
                                   osmium::io::overwrite overwrite,
                                   osmium::io::fsync fsync,
                                   const options_type& options) :
    ExportFormat(options),
    m_fd(osmium::io::detail::open_for_writing(output_filename, overwrite)),
    m_fsync(fsync),
    m_text_sequence_format(output_format == "geojsonseq"),
    m_with_record_separator(m_text_sequence_format && options.format_options.is_true("print_record_separator")),
    m_out(&m_streambuf) {
    m_streambuf.set_fd(m_fd);

    if (m_text_sequence_format) {
        const auto prs = options.format_options.get("print_record_separator");
        if (prs != "true" && prs != "false") {
            throw config_error{"Unknown value for print_record_separator option: '" + prs + "'."};
        }
        m_stream_writer.emplace(
            m_out,
            m_with_record_separator ? rfl::json::StreamFormat::json_seq
                                    : rfl::json::StreamFormat::ndjson);
    } else {
        m_out << R"({"type":"FeatureCollection","features":[)" << '\n';
    }
}

void ExportFormatJSON::append_coordinate_pair(std::string* buffer, const osmium::Location& location) {
    *buffer += '[';
    append_coordinate(buffer, location.lon());
    *buffer += ',';
    append_coordinate(buffer, location.lat());
    *buffer += ']';
}

void ExportFormatJSON::append_coordinate_list(std::string* buffer, const osmium::NodeRefList& nrl) {
    *buffer += '[';
    for (const auto& nr : nrl) {
        append_coordinate_pair(buffer, nr.location());
        *buffer += ',';
    }

    if (buffer->back() == ',') {
        buffer->back() = ']';
    } else {
        *buffer += ']';
    }
}

void ExportFormatJSON::emit_feature(const std::string& prefix,
                                    const osmium::OSMObject& object,
                                    const char* geom_type,
                                    std::string coordinates_json) {
    rfl::Object<export_json::PropertyValue> props;

    if (!options().type.empty()) {
        props.insert(options().type, export_json::PropertyValue{std::string{object_type_as_string(object)}});
    }
    if (!options().id.empty()) {
        const auto id_val = object.type() == osmium::item_type::area
            ? osmium::area_id_to_object_id(object.id())
            : object.id();
        props.insert(options().id, export_json::PropertyValue{static_cast<std::int64_t>(id_val)});
    }
    if (!options().version.empty()) {
        props.insert(options().version, export_json::PropertyValue{static_cast<std::uint64_t>(object.version())});
    }
    if (!options().changeset.empty()) {
        props.insert(options().changeset, export_json::PropertyValue{static_cast<std::uint64_t>(object.changeset())});
    }
    if (!options().uid.empty()) {
        props.insert(options().uid, export_json::PropertyValue{static_cast<std::uint64_t>(object.uid())});
    }
    if (!options().user.empty()) {
        props.insert(options().user, export_json::PropertyValue{std::string{object.user()}});
    }
    if (!options().timestamp.empty()) {
        props.insert(options().timestamp, export_json::PropertyValue{static_cast<std::uint64_t>(object.timestamp().seconds_since_epoch())});
    }
    if (!options().way_nodes.empty() && object.type() == osmium::item_type::way) {
        std::vector<std::int64_t> refs;
        const auto& nodes = static_cast<const osmium::Way&>(object).nodes();
        refs.reserve(nodes.size());
        for (const auto& nr : nodes) {
            refs.push_back(nr.ref());
        }
        props.insert(options().way_nodes, export_json::PropertyValue{std::move(refs)});
    }

    const bool has_tags = add_tags(object, [&](const osmium::Tag& tag) {
        props.insert(std::string{tag.key()}, export_json::PropertyValue{std::string{tag.value()}});
    });

    if (!has_tags && !options().keep_untagged) {
        return;
    }

    export_json::GeoJSONFeature feature;
    if (options().unique_id == unique_id_type::counter) {
        feature.id = m_count + 1;
    } else if (options().unique_id == unique_id_type::type_id) {
        feature.id = prefix + std::to_string(object.id());
    }
    feature.geometry = {geom_type, rfl::json::Raw{std::move(coordinates_json)}};
    feature.properties = std::move(props);

    if (m_stream_writer) {
        const auto res = m_stream_writer->write(feature);
        if (!res) {
            throw std::runtime_error{res.error().what()};
        }
    } else {
        if (m_count > 0) {
            m_out << ",\n";
        }
        m_out << rfl::json::write(feature);
    }

    ++m_count;
}

void ExportFormatJSON::node(const osmium::Node& node) {
    std::string coords;
    coords.reserve(32);
    append_coordinate_pair(&coords, node.location());
    emit_feature("n", node, "Point", std::move(coords));
}

void ExportFormatJSON::way(const osmium::Way& way) {
    std::string coords;
    coords.reserve(way.nodes().size() * 24 + 2);
    append_coordinate_list(&coords, way.nodes());
    emit_feature("w", way, "LineString", std::move(coords));
}

void ExportFormatJSON::area(const osmium::Area& area) {
    std::string coords;
    coords += '[';
    for (const auto& outer_ring : area.outer_rings()) {
        coords += '[';
        append_coordinate_list(&coords, outer_ring);
        for (const auto& inner_ring : area.inner_rings(outer_ring)) {
            coords += ',';
            append_coordinate_list(&coords, inner_ring);
        }
        coords += "],";
    }
    coords.pop_back(); // remove trailing comma
    coords += ']';
    emit_feature("a", area, "MultiPolygon", std::move(coords));
}

void ExportFormatJSON::close() {
    if (m_fd > 0) {
        if (m_stream_writer) {
            (void)m_stream_writer->finish();
            (void)m_stream_writer->flush();
            m_stream_writer.reset();
        } else {
            m_out << "\n]}\n";
        }

        m_streambuf.flush_all();
        if (m_fsync == osmium::io::fsync::yes) {
            osmium::io::detail::reliable_fsync(m_fd);
        }
        ::close(m_fd);
        m_fd = -1;
    }
}

