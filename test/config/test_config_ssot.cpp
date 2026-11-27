#include "test.hpp" // IWYU pragma: keep

#include "config_util.hpp"
#include "export/config.hpp"
#include "extract/config.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <variant>
#include <vector>

namespace {

std::string read_file_to_string(const std::string& path) {
    std::ifstream in{path};
    REQUIRE(in.is_open());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

const nlohmann::json& resolve_schema_node(const nlohmann::json& root, const nlohmann::json& node) {
    if (node.is_object() && node.contains("$ref") && node["$ref"].is_string()) {
        const std::string ref = node["$ref"].get<std::string>();
        const std::string defs_prefix = "#/$defs/";
        if (ref.rfind(defs_prefix, 0) == 0 && root.contains("$defs")) {
            const std::string name = ref.substr(defs_prefix.size());
            if (root["$defs"].contains(name)) {
                return resolve_schema_node(root, root["$defs"][name]);
            }
        }
    }
    return node;
}

bool contains_string(const nlohmann::json& arr, const std::string& value) {
    if (!arr.is_array()) {
        return false;
    }
    for (const auto& item : arr) {
        if (item.is_string() && item.get<std::string>() == value) {
            return true;
        }
    }
    return false;
}

// Lightweight Draft 2020-12 validator covering the keywords emitted by rfl::json::to_schema.
bool validate_against_schema(const nlohmann::json& root, const nlohmann::json& raw_schema, const nlohmann::json& instance) {
    const auto& schema = resolve_schema_node(root, raw_schema);
    if (!schema.is_object()) {
        return false;
    }

    if (schema.contains("anyOf") && schema["anyOf"].is_array()) {
        bool matched = false;
        for (const auto& sub : schema["anyOf"]) {
            if (validate_against_schema(root, sub, instance)) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            return false;
        }
    }

    if (schema.contains("oneOf") && schema["oneOf"].is_array()) {
        int matches = 0;
        for (const auto& sub : schema["oneOf"]) {
            if (validate_against_schema(root, sub, instance)) {
                ++matches;
            }
        }
        if (matches != 1) {
            return false;
        }
    }

    if (schema.contains("allOf") && schema["allOf"].is_array()) {
        for (const auto& sub : schema["allOf"]) {
            if (!validate_against_schema(root, sub, instance)) {
                return false;
            }
        }
    }

    if (schema.contains("type") && schema["type"].is_string()) {
        const std::string t = schema["type"].get<std::string>();
        if (t == "null" && !instance.is_null()) return false;
        if (t == "boolean" && !instance.is_boolean()) return false;
        if (t == "string" && !instance.is_string()) return false;
        if (t == "integer" && !(instance.is_number_integer() || instance.is_number_unsigned())) return false;
        if (t == "number" && !instance.is_number()) return false;
        if (t == "array" && !instance.is_array()) return false;
        if (t == "object" && !instance.is_object()) return false;
    }

    if (instance.is_number()) {
        const double val = instance.get<double>();
        if (schema.contains("minimum") && val < schema["minimum"].get<double>()) return false;
        if (schema.contains("maximum") && val > schema["maximum"].get<double>()) return false;
        if (schema.contains("exclusiveMinimum") && val <= schema["exclusiveMinimum"].get<double>()) return false;
        if (schema.contains("exclusiveMaximum") && val >= schema["exclusiveMaximum"].get<double>()) return false;
    }

    if (instance.is_array()) {
        if (schema.contains("minItems") && instance.size() < schema["minItems"].get<std::size_t>()) return false;
        if (schema.contains("maxItems") && instance.size() > schema["maxItems"].get<std::size_t>()) return false;
        if (schema.contains("items") && schema["items"].is_object()) {
            for (const auto& elem : instance) {
                if (!validate_against_schema(root, schema["items"], elem)) {
                    return false;
                }
            }
        }
    }

    if (instance.is_object()) {
        if (schema.contains("required") && schema["required"].is_array()) {
            for (const auto& req : schema["required"]) {
                if (!instance.contains(req.get<std::string>())) {
                    return false;
                }
            }
        }
        const bool has_props = schema.contains("properties") && schema["properties"].is_object();
        for (const auto& [key, val] : instance.items()) {
            if (has_props && schema["properties"].contains(key)) {
                if (!validate_against_schema(root, schema["properties"][key], val)) {
                    return false;
                }
            } else if (schema.contains("additionalProperties")) {
                const auto& ap = schema["additionalProperties"];
                if (ap.is_boolean()) {
                    if (!ap.get<bool>()) {
                        return false;
                    }
                } else if (ap.is_object()) {
                    if (!validate_against_schema(root, ap, val)) {
                        return false;
                    }
                }
            }
        }
    }

    return true;
}

} // anonymous namespace

TEST_CASE("Export config SSOT: all fixtures and example configs parse and match generated schema") {
    const auto schema = nlohmann::json::parse(osmium_export::export_config_schema_json());

    const std::vector<std::string> fixtures = {
        "test/export/config-empty-empty.json",
        "test/export/config-empty-tag.json",
        "test/export/config-empty-tagx.json",
        "test/export/config-false-false.json",
        "test/export/config-null-null.json",
        "test/export/config-null-tag.json",
        "test/export/config-tag-empty.json",
        "test/export/config-tag-null.json",
        "test/export/config-tag-tag.json",
        "test/export/config-tagx-empty.json",
        "test/export/config-tagx-tagx.json",
        "test/export/config-true-true.json",
        "test/export/config-undefined.json",
        "export-example-config/default-config.json",
        "export-example-config/example-config.json",
    };

    for (const auto& path : fixtures) {
        CAPTURE(path);
        const std::string content = read_file_to_string(path);
        const auto parsed = osmium_export::read_export_config(content);
        REQUIRE(parsed.has_value());
        REQUIRE(validate_against_schema(schema, schema, nlohmann::json::parse(content)));
    }

    SECTION("Omitted linear_tags and area_tags decode to nullopt (rule_type::other)") {
        const auto undef = osmium_export::read_export_config(read_file_to_string("test/export/config-undefined.json"));
        REQUIRE(undef.has_value());
        REQUIRE_FALSE(undef->linear_tags.value().has_value());
        REQUIRE_FALSE(undef->area_tags.value().has_value());

        const auto tag_null = osmium_export::read_export_config(read_file_to_string("test/export/config-tag-null.json"));
        REQUIRE(tag_null.has_value());
        REQUIRE(tag_null->linear_tags.value().has_value());
        REQUIRE(std::holds_alternative<std::vector<std::string>>(*tag_null->linear_tags.value()));
        REQUIRE_FALSE(tag_null->area_tags.value().has_value());
    }

    SECTION("Default config JSON round-trips cleanly and validates against schema") {
        const std::string default_json = osmium_export::default_export_config_json();
        REQUIRE(validate_against_schema(schema, schema, nlohmann::json::parse(default_json)));
        const auto parsed = osmium_export::read_export_config(default_json);
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->linear_tags.value().has_value());
        REQUIRE(std::get<bool>(*parsed->linear_tags.value()) == true);
        REQUIRE(parsed->area_tags.value().has_value());
        REQUIRE(std::get<bool>(*parsed->area_tags.value()) == true);
        REQUIRE(parsed->format_options.value().empty());
        REQUIRE(parsed->include_tags.value().empty());
        REQUIRE(parsed->exclude_tags.value().empty());
        REQUIRE(std::get<bool>(parsed->attributes.value().type.value()) == false);
        REQUIRE(std::get<bool>(parsed->attributes.value().id.value()) == false);
        REQUIRE(std::get<bool>(parsed->attributes.value().way_nodes.value()) == false);
    }

    SECTION("Format options preserve uint64, int64, double, bool, string, and null") {
        const std::string json = R"({
            "format_options": {
                "flag_true": true,
                "flag_null": null,
                "u64_big": 18446744073709551615,
                "i64_neg": -42,
                "flt": 3.5,
                "str": "hstore"
            }
        })";
        REQUIRE(validate_against_schema(schema, schema, nlohmann::json::parse(json)));
        const auto parsed = osmium_export::read_export_config(json);
        REQUIRE(parsed.has_value());
        const auto& opts = parsed->format_options.value();
        REQUIRE(opts.size() == 6);
        REQUIRE(std::get<bool>(*opts.at("flag_true")) == true);
        REQUIRE_FALSE(opts.at("flag_null").has_value());
        REQUIRE(std::get<uint64_t>(*opts.at("u64_big")) == UINT64_MAX);
        REQUIRE(std::get<int64_t>(*opts.at("i64_neg")) == -42);
        REQUIRE(std::get<double>(*opts.at("flt")) == Approx(3.5));
        REQUIRE(std::get<std::string>(*opts.at("str")) == "hstore");
    }

    SECTION("Schema rejects invalid export configs") {
        REQUIRE_FALSE(validate_against_schema(schema, schema, nlohmann::json::parse(R"({"unknown_key": true})")));
        REQUIRE_FALSE(validate_against_schema(schema, schema, nlohmann::json::parse(R"({"format_options": {"nested": {}}})")));
        REQUIRE_FALSE(validate_against_schema(schema, schema, nlohmann::json::parse(R"({"attributes": {"id": 42}})")));
    }
}

TEST_CASE("Export config SSOT: generated JSON Schema structure") {
    const std::string schema_str = osmium_export::export_config_schema_json();
    const auto schema = nlohmann::json::parse(schema_str);
    REQUIRE(schema.is_object());
    REQUIRE(schema.contains("$schema"));
    REQUIRE(schema.contains("$defs"));

    const auto& root = resolve_schema_node(schema, schema);
    REQUIRE(root["type"] == "object");
    REQUIRE(root["additionalProperties"] == false);
    REQUIRE(root.contains("properties"));

    const auto& props = root["properties"];
    for (const char* key : {"attributes", "format_options", "linear_tags", "area_tags", "exclude_tags", "include_tags"}) {
        CAPTURE(key);
        REQUIRE(props.contains(key));
        REQUIRE(props[key].contains("description"));
    }

    const auto& attrs = resolve_schema_node(schema, props["attributes"]);
    REQUIRE(attrs["type"] == "object");
    REQUIRE(attrs["additionalProperties"] == false);
    for (const char* key : {"type", "id", "version", "changeset", "timestamp", "uid", "user", "way_nodes"}) {
        CAPTURE(key);
        REQUIRE(attrs["properties"].contains(key));
    }
}

TEST_CASE("Extract config SSOT: fixtures and example configs parse and match generated schema") {
    const auto schema = nlohmann::json::parse(osmium_extract::extract_config_schema_json());

    SECTION("test/extract/config.json") {
        const std::string content = read_file_to_string("test/extract/config.json");
        REQUIRE(validate_against_schema(schema, schema, nlohmann::json::parse(content)));
        const auto parsed = osmium_extract::read_extract_config(content);
        REQUIRE(parsed.has_value());
        REQUIRE_FALSE(parsed->directory.value().has_value());
        REQUIRE(parsed->extracts.value().size() == 1);
        const auto& item = parsed->extracts.value()[0];
        REQUIRE(item.output.value() == "-");
        REQUIRE(item.output_format.value() == "osm");
        REQUIRE(item.bbox.value().has_value());
        const auto& arr = std::get<std::array<double, 4>>(*item.bbox.value());
        REQUIRE(arr[0] == Approx(0.0));
        REQUIRE(arr[2] == Approx(1.5));
    }

    SECTION("extract-example-config/extracts.json") {
        const std::string content = read_file_to_string("extract-example-config/extracts.json");
        REQUIRE(validate_against_schema(schema, schema, nlohmann::json::parse(content)));
        const auto parsed = osmium_extract::read_extract_config(content);
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->directory.value() == "/tmp/foobar");
        const auto& extracts = parsed->extracts.value();
        REQUIRE(extracts.size() == 7);

        // 0: bbox object
        REQUIRE(std::holds_alternative<osmium_extract::BBoxObject>(*extracts[0].bbox.value()));
        const auto& bbox_obj = std::get<osmium_extract::BBoxObject>(*extracts[0].bbox.value());
        REQUIRE(bbox_obj.left.value().value() == Approx(13.57));
        REQUIRE(bbox_obj.top.value().value() == Approx(51.18));

        // 1: bbox array
        REQUIRE(std::holds_alternative<std::array<double, 4>>(*extracts[1].bbox.value()));

        // 2: polygon file (geojson)
        REQUIRE(std::holds_alternative<osmium_extract::PolygonFileSpec>(*extracts[2].polygon.value()));
        const auto& poly_file = std::get<osmium_extract::PolygonFileSpec>(*extracts[2].polygon.value());
        REQUIRE(poly_file.file_name.value() == "berlin.geojson");
        REQUIRE(poly_file.file_type.value() == "geojson");

        // 3: multipolygon file (auto-detected poly)
        REQUIRE(std::holds_alternative<osmium_extract::PolygonFileSpec>(*extracts[3].multipolygon.value()));
        const auto& mp_file = std::get<osmium_extract::PolygonFileSpec>(*extracts[3].multipolygon.value());
        REQUIRE(mp_file.file_name.value() == "brandenburg.poly");
        REQUIRE_FALSE(mp_file.file_type.value().has_value());

        // 5: inline polygon
        REQUIRE(std::holds_alternative<osmium_extract::InlinePolygon>(*extracts[5].polygon.value()));
        const auto& inline_poly = std::get<osmium_extract::InlinePolygon>(*extracts[5].polygon.value());
        REQUIRE(inline_poly.value().size() == 1);
        REQUIRE(inline_poly.value()[0].value().size() >= 3);

        // 6: inline multipolygon with outer + inner ring
        REQUIRE(std::holds_alternative<osmium_extract::InlineMultiPolygon>(*extracts[6].multipolygon.value()));
        const auto& inline_mp = std::get<osmium_extract::InlineMultiPolygon>(*extracts[6].multipolygon.value());
        REQUIRE(inline_mp.value().size() == 1);
        REQUIRE(inline_mp.value()[0].value().size() == 2);
    }

    SECTION("output_header supports string values and null removals") {
        const std::string json = R"({
            "extracts": [{
                "output": "out.osm.pbf",
                "bbox": [0, 0, 1, 1],
                "output_header": {
                    "osmosis_replication_base_url": "https://example.org",
                    "xml_josm_upload": null
                }
            }]
        })";
        REQUIRE(validate_against_schema(schema, schema, nlohmann::json::parse(json)));
        const auto parsed = osmium_extract::read_extract_config(json);
        REQUIRE(parsed.has_value());
        const auto& hdr = *parsed->extracts.value()[0].output_header.value();
        REQUIRE(hdr.at("osmosis_replication_base_url") == "https://example.org");
        REQUIRE_FALSE(hdr.at("xml_josm_upload").has_value());
    }

    SECTION("Schema rejects invalid extract configs") {
        REQUIRE_FALSE(validate_against_schema(schema, schema, nlohmann::json::parse(R"({"directory": "/tmp"})")));
        REQUIRE_FALSE(validate_against_schema(schema, schema, nlohmann::json::parse(R"({"extracts": [{"bbox": [0, 0, 1, 1]}]})")));
        REQUIRE_FALSE(validate_against_schema(schema, schema, nlohmann::json::parse(R"({"extracts": [{"output": "a.pbf", "bbox": {"left": 200, "right": 10, "top": 10, "bottom": 0}}]})")));
        REQUIRE_FALSE(validate_against_schema(schema, schema, nlohmann::json::parse(R"({"extracts": [{"output": "a.pbf", "polygon": [[[0, 0], [1, 1]]]}]})")));
    }
}

TEST_CASE("Extract config SSOT: generated JSON Schema preserves required fields and constraints") {
    const std::string schema_str = osmium_extract::extract_config_schema_json();
    const auto schema = nlohmann::json::parse(schema_str);
    REQUIRE(schema.is_object());
    REQUIRE(schema.contains("$defs"));

    const auto& root = resolve_schema_node(schema, schema);
    REQUIRE(root["type"] == "object");
    REQUIRE(root["additionalProperties"] == false);
    REQUIRE(contains_string(root["required"], "extracts"));
    REQUIRE_FALSE(contains_string(root["required"], "directory"));

    const auto& item_schema = resolve_schema_node(schema, root["properties"]["extracts"]["items"]);
    REQUIRE(item_schema["type"] == "object");
    REQUIRE(item_schema["additionalProperties"] == false);
    REQUIRE(contains_string(item_schema["required"], "output"));
    REQUIRE_FALSE(contains_string(item_schema["required"], "description"));

    bool found_bbox_obj = false;
    bool found_poly_file = false;
    for (const auto& [def_name, def_val] : schema["$defs"].items()) {
        if (def_val.is_object() && def_val.contains("properties")) {
            const auto& p = def_val["properties"];
            if (p.contains("left") && p.contains("right") && p.contains("top") && p.contains("bottom")) {
                found_bbox_obj = true;
                for (const char* req : {"left", "right", "top", "bottom"}) {
                    REQUIRE(contains_string(def_val["required"], req));
                }
            }
            if (p.contains("file_name") && p.contains("file_type")) {
                found_poly_file = true;
                REQUIRE(contains_string(def_val["required"], "file_name"));
                REQUIRE_FALSE(contains_string(def_val["required"], "file_type"));
            }
        }
    }
    REQUIRE(found_bbox_obj);
    REQUIRE(found_poly_file);
}

TEST_CASE("Config SSOT: structured rfl::ErrorKind and ErrorDetail::path diagnostics") {
    SECTION("Export syntax error reports syntax_error") {
        const auto res = osmium_export::read_export_config("{ invalid json");
        REQUIRE_FALSE(res.has_value());
        REQUIRE(res.error().kind() == rfl::ErrorKind::syntax_error);
        const auto formatted = osmium_config::format_rfl_error(res.error());
        REQUIRE(formatted.find("[syntax_error]") != std::string::npos);
    }

    SECTION("Export unknown top-level field reports unknown_field") {
        const auto res = osmium_export::read_export_config(R"({"not_a_real_field": true})");
        REQUIRE_FALSE(res.has_value());
        REQUIRE(res.error().kind() == rfl::ErrorKind::unknown_field);
        const auto formatted = osmium_config::format_rfl_error(res.error());
        REQUIRE(formatted.find("[unknown_field]") != std::string::npos);
    }

    SECTION("Export invalid format_option value reports structured path") {
        const auto res = osmium_export::read_export_config(R"({"format_options": {"bad_opt": {"nested": 1}}})");
        REQUIRE_FALSE(res.has_value());
        const auto formatted = osmium_config::format_rfl_error(res.error());
        REQUIRE(formatted.find("format_options.bad_opt") != std::string::npos);
    }

    SECTION("Extract missing required extracts field reports missing_field") {
        const auto res = osmium_extract::read_extract_config(R"({"directory": "/tmp"})");
        REQUIRE_FALSE(res.has_value());
        REQUIRE(res.error().kind() == rfl::ErrorKind::missing_field);
        const auto formatted = osmium_config::format_rfl_error(res.error());
        REQUIRE(formatted.find("[missing_field]") != std::string::npos);
    }

    SECTION("Extract missing required output field reports extracts[0] path") {
        const auto res = osmium_extract::read_extract_config(R"({"extracts": [{"bbox": [0, 0, 1, 1]}]})");
        REQUIRE_FALSE(res.has_value());
        REQUIRE(res.error().kind() == rfl::ErrorKind::missing_field);
        const auto formatted = osmium_config::format_rfl_error(res.error());
        REQUIRE(formatted.find("[missing_field]") != std::string::npos);
        REQUIRE(formatted.find("extracts[0]") != std::string::npos);
    }

    SECTION("Extract unknown field in item reports unknown_field at extracts[0]") {
        const auto res = osmium_extract::read_extract_config(R"({"extracts": [{"output": "a.pbf", "bogus": 1}]})");
        REQUIRE_FALSE(res.has_value());
        REQUIRE(res.error().kind() == rfl::ErrorKind::unknown_field);
        const auto formatted = osmium_config::format_rfl_error(res.error());
        REQUIRE(formatted.find("[unknown_field]") != std::string::npos);
        REQUIRE(formatted.find("extracts[0]") != std::string::npos);
    }

    SECTION("Extract out-of-range bbox longitude is rejected with structured path") {
        const auto res = osmium_extract::read_extract_config(R"({
            "extracts": [{
                "output": "a.pbf",
                "bbox": {"left": 200.0, "right": 201.0, "top": 10.0, "bottom": 0.0}
            }]
        })");
        REQUIRE_FALSE(res.has_value());
        const auto formatted = osmium_config::format_rfl_error(res.error());
        REQUIRE(formatted.find("extracts[0].bbox") != std::string::npos);
    }

    SECTION("Extract inline polygon ring with fewer than 3 coordinates is rejected") {
        const auto res = osmium_extract::read_extract_config(R"({
            "extracts": [{
                "output": "a.pbf",
                "polygon": [[[0.0, 0.0], [1.0, 1.0]]]
            }]
        })");
        REQUIRE_FALSE(res.has_value());
        const auto formatted = osmium_config::format_rfl_error(res.error());
        REQUIRE(formatted.find("extracts[0].polygon") != std::string::npos);
    }

    SECTION("Extract invalid output_header value reports path") {
        const auto res = osmium_extract::read_extract_config(R"({
            "extracts": [{
                "output": "a.pbf",
                "bbox": [0, 0, 1, 1],
                "output_header": {"bad": 123}
            }]
        })");
        REQUIRE_FALSE(res.has_value());
        const auto formatted = osmium_config::format_rfl_error(res.error());
        REQUIRE(formatted.find("extracts[0].output_header.bad") != std::string::npos);
    }
}
