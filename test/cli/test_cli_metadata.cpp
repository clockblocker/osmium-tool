#include "test.hpp" // IWYU pragma: keep

#include "cli_codegen.hpp"
#include "cmd.hpp"

#include <nlohmann/json.hpp>
#include <rfl/json.hpp>

#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string capture_help_output(Command& cmd) {
    std::ostringstream captured;
    auto* old_buf = std::cout.rdbuf(captured.rdbuf());
    try {
        const bool should_run = cmd.setup({"--help"});
        std::cout.rdbuf(old_buf);
        REQUIRE_FALSE(should_run);
    } catch (...) {
        std::cout.rdbuf(old_buf);
        throw;
    }
    return captured.str();
}

std::set<std::string> extract_long_options_from_help(const std::string& help_text) {
    std::set<std::string> opts;
    const std::regex re{R"((?:^|[\s,])--([a-z][a-z0-9-]*))"};
    std::istringstream in{help_text};
    std::string line;
    bool in_options = false;
    while (std::getline(in, line)) {
        if (line.find("OPTIONS:") != std::string::npos) {
            in_options = true;
            continue;
        }
        if (!in_options || line.size() < 3 || line[0] != ' ' || line[1] != ' ' || line[2] != '-') {
            continue;
        }
        const std::size_t col_start = 2;
        const std::size_t desc_gap = line.find("  ", col_start);
        const std::string spec_col = (desc_gap == std::string::npos)
            ? line.substr(col_start)
            : line.substr(col_start, desc_gap - col_start);
        std::smatch m;
        if (std::regex_search(spec_col, m, re)) {
            opts.insert(m[1].str());
        }
    }
    return opts;
}

std::string read_file(const std::string& path) {
    std::ifstream in{path};
    REQUIRE(in.is_open());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::set<std::string> extract_zsh_function_long_options(const std::string& zsh_script, const std::string& func_name) {
    const std::string header = func_name + "() {";
    const std::size_t start = zsh_script.find(header);
    REQUIRE(start != std::string::npos);
    const std::size_t end = zsh_script.find("\n}", start);
    REQUIRE(end != std::string::npos);
    const std::string body = zsh_script.substr(start, end - start);

    std::set<std::string> opts;
    // Match actual option specs like --foo[...], )--foo[...], *--foo[...], --foo=[...]
    const std::regex re{R"((?:^|['*)])--([a-z][a-z0-9-]*)=?\[)"};
    for (std::sregex_iterator it(body.begin(), body.end(), re), end_it; it != end_it; ++it) {
        opts.insert((*it)[1].str());
    }
    return opts;
}

} // anonymous namespace

TEST_CASE("CLI Metadata SSOT: all 23 commands expose complete metadata matching --help") {
    CommandFactory factory;
    register_commands(factory);

    const auto catalog = osmium_cli::build_cli_catalog(factory);
    REQUIRE(catalog.program == "osmium");
    REQUIRE_FALSE(catalog.version.empty());
    // 23 optioned commands + help
    REQUIRE(catalog.commands.size() == 24);

    int optioned_commands = 0;
    for (const auto& spec : catalog.commands) {
        CAPTURE(spec.name);
        REQUIRE_FALSE(spec.description.empty());
        REQUIRE_FALSE(spec.synopsis.empty());

        if (spec.name == "help") {
            continue;
        }
        ++optioned_commands;
        REQUIRE_FALSE(spec.options.empty());

        auto cmd = factory.create_command(spec.name);
        REQUIRE(cmd != nullptr);
        const std::string help_text = capture_help_output(*cmd);
        const auto help_opts = extract_long_options_from_help(help_text);

        std::set<std::string> meta_opts;
        bool has_help = false;
        bool has_verbose = false;
        for (const auto& opt : spec.options) {
            REQUIRE_FALSE(opt.long_name.empty());
            REQUIRE_FALSE(opt.group.empty());
            REQUIRE_FALSE(opt.description.empty());
            meta_opts.insert(opt.long_name);
            if (opt.long_name == "help") {
                has_help = true;
                REQUIRE(opt.short_name == "h");
            }
            if (opt.long_name == "verbose") {
                has_verbose = true;
                REQUIRE(opt.short_name == "v");
            }
            REQUIRE(help_text.find("--" + opt.long_name) != std::string::npos);
            if (!opt.short_name.empty()) {
                REQUIRE(help_text.find("-" + opt.short_name) != std::string::npos);
            }
        }
        REQUIRE(has_help);
        REQUIRE(has_verbose);
        REQUIRE(meta_opts == help_opts);
    }

    REQUIRE(optioned_commands == 23);
}

TEST_CASE("CLI Metadata SSOT: codegen produces valid zsh, bash, man-options, and JSON catalog") {
    CommandFactory factory;
    register_commands(factory);

    const auto catalog = osmium_cli::build_cli_catalog(factory);

    SECTION("JSON catalog round-trips through reflect-cpp") {
        const std::string json_str = osmium_cli::generate_completions(factory, "json");
        const auto parsed_json = nlohmann::json::parse(json_str);
        REQUIRE(parsed_json["program"] == "osmium");
        REQUIRE(parsed_json["commands"].is_array());
        REQUIRE(parsed_json["commands"].size() == 24);

        const auto roundtrip = rfl::json::read<osmium_cli::CliCatalog>(json_str);
        REQUIRE(roundtrip.has_value());
        REQUIRE(roundtrip->commands.size() == catalog.commands.size());
    }

    SECTION("ZSH completion contains all 23 command functions and options") {
        const std::string zsh = osmium_cli::generate_completions(factory, "zsh");
        REQUIRE(zsh.rfind("#compdef osmium", 0) == 0);
        REQUIRE(zsh.find("_osmium()") != std::string::npos);
        for (const auto& cmd : catalog.commands) {
            CAPTURE(cmd.name);
            REQUIRE(zsh.find("_osmium-" + cmd.name + "()") != std::string::npos);
            if (!cmd.options.empty()) {
                const auto gen_opts = extract_zsh_function_long_options(zsh, "_osmium-" + cmd.name);
                for (const auto& opt : cmd.options) {
                    CAPTURE(opt.long_name);
                    REQUIRE(gen_opts.count(opt.long_name) == 1);
                }
            }
        }
    }

    SECTION("Bash completion contains all commands and options") {
        const std::string bash = osmium_cli::generate_completions(factory, "bash");
        REQUIRE(bash.find("_osmium_commands=") != std::string::npos);
        REQUIRE(bash.find("complete -F _osmium osmium") != std::string::npos);
        for (const auto& cmd : catalog.commands) {
            CAPTURE(cmd.name);
            REQUIRE(bash.find(cmd.name + ")") != std::string::npos);
            for (const auto& opt : cmd.options) {
                REQUIRE(bash.find("--" + opt.long_name) != std::string::npos);
            }
        }
    }

    SECTION("Man-options Pandoc Markdown covers all commands and supports command filtering") {
        const std::string all_man = osmium_cli::generate_completions(factory, "man-options");
        for (const auto& cmd : catalog.commands) {
            if (cmd.options.empty()) {
                continue;
            }
            CAPTURE(cmd.name);
            REQUIRE(all_man.find("# osmium " + cmd.name) != std::string::npos);
        }

        const std::string export_man = osmium_cli::generate_completions(factory, "man-options", "export");
        REQUIRE(export_man.find("# COMMAND OPTIONS") != std::string::npos);
        REQUIRE(export_man.find("\\--print-config-schema") != std::string::npos);
        REQUIRE(export_man.find("-C, \\--print-default-config") != std::string::npos);
        REQUIRE(export_man.find("# osmium extract") == std::string::npos);
    }

    SECTION("Unknown format or command throws argument_error") {
        REQUIRE_THROWS_AS(osmium_cli::generate_completions(factory, "fish"), argument_error);
        REQUIRE_THROWS_AS(osmium_cli::generate_completions(factory, "man-options", "no-such-cmd"), argument_error);
    }
}

TEST_CASE("CLI Metadata SSOT: catches real drift in legacy hand-maintained zsh_completion/_osmium") {
    CommandFactory factory;
    register_commands(factory);

    const std::string legacy_zsh = read_file("zsh_completion/_osmium");
    const std::string generated_zsh = osmium_cli::generate_completions(factory, "zsh");

    SECTION("export command drift: legacy has removed --omit-rs and misses live options") {
        const auto legacy_export = extract_zsh_function_long_options(legacy_zsh, "_osmium-export");
        const auto generated_export = extract_zsh_function_long_options(generated_zsh, "_osmium-export");

        // Legacy completion advertises removed --omit-rs; live SSOT does not
        REQUIRE(legacy_export.count("omit-rs") == 1);
        REQUIRE(generated_export.count("omit-rs") == 0);

        // Legacy completion misses live options; SSOT includes all of them
        for (const char* missing_in_legacy : {"format-option", "geometry-types", "print-config-schema"}) {
            CAPTURE(missing_in_legacy);
            REQUIRE(legacy_export.count(missing_in_legacy) == 0);
            REQUIRE(generated_export.count(missing_in_legacy) == 1);
        }
    }

    SECTION("extract command drift: legacy misses --set-bounds, --progress, --no-progress, and --print-config-schema") {
        const auto legacy_extract = extract_zsh_function_long_options(legacy_zsh, "_osmium-extract");
        const auto generated_extract = extract_zsh_function_long_options(generated_zsh, "_osmium-extract");

        for (const char* missing_in_legacy : {"set-bounds", "progress", "no-progress", "print-config-schema"}) {
            CAPTURE(missing_in_legacy);
            REQUIRE(legacy_extract.count(missing_in_legacy) == 0);
            REQUIRE(generated_extract.count(missing_in_legacy) == 1);
        }
    }
}
