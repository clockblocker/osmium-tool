#ifndef EXPORT_EXPORT_FORMAT_JSON_HPP
#define EXPORT_EXPORT_FORMAT_JSON_HPP

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

#include "export_format.hpp"

#include <osmium/fwd.hpp>
#include <osmium/io/writer_options.hpp>

#include <rfl/json.hpp>
#include <rfl/json/Raw.hpp>
#include <rfl/json/stream.hpp>

#include <optional>
#include <ostream>
#include <streambuf>
#include <string>

class ExportFormatJSON : public ExportFormat {

    class FdStreamBuf : public std::streambuf {
        int m_fd = -1;
        std::string m_buffer;

        void flush_buffer();

    public:
        FdStreamBuf() = default;
        void set_fd(int fd);
        void flush_all();

    protected:
        int_type overflow(int_type ch) override;
        std::streamsize xsputn(const char* s, std::streamsize n) override;
        int sync() override;
    };

    int m_fd;
    osmium::io::fsync m_fsync;
    bool m_text_sequence_format;
    bool m_with_record_separator;
    FdStreamBuf m_streambuf;
    std::ostream m_out;
    std::optional<rfl::json::StreamWriter<>> m_stream_writer;

    void emit_feature(const std::string& prefix,
                      const osmium::OSMObject& object,
                      const char* geom_type,
                      std::string coordinates_json);

    static void append_coordinate_pair(std::string* buffer, const osmium::Location& location);
    static void append_coordinate_list(std::string* buffer, const osmium::NodeRefList& nrl);

public:

    ExportFormatJSON(const std::string& output_format,
                     const std::string& output_filename,
                     osmium::io::overwrite overwrite,
                     osmium::io::fsync fsync,
                     const options_type& options);

    ~ExportFormatJSON() noexcept override {
        try {
            close();
        } catch (...) {
        }
    }

    void node(const osmium::Node& node) override;

    void way(const osmium::Way& way) override;

    void area(const osmium::Area& area) override;

    void close() override;

}; // class ExportFormatJSON

#endif // EXPORT_EXPORT_FORMAT_JSON_HPP
