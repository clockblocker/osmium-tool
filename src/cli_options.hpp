#ifndef CLI_OPTIONS_HPP
#define CLI_OPTIONS_HPP

#include "exception.hpp"

#include <rfl.hpp>
#include <rfl/cli.hpp>

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace osmium_cli {

template <rfl::internal::StringLiteral GroupName, rfl::internal::StringLiteral Desc, class T>
using Opt = rfl::cli::Opt<T, rfl::cli::attr::Group<GroupName>, rfl::cli::attr::Desc<Desc>>;

template <rfl::internal::StringLiteral GroupName, rfl::internal::StringLiteral ShortName,
          rfl::internal::StringLiteral Desc, class T>
using ShortOpt = rfl::cli::Opt<T, rfl::cli::attr::Group<GroupName>,
                               rfl::cli::attr::Short<ShortName>,
                               rfl::cli::attr::Desc<Desc>>;

template <rfl::internal::StringLiteral Desc, class T>
using CmdOpt = Opt<"COMMAND OPTIONS", Desc, T>;

template <rfl::internal::StringLiteral ShortName, rfl::internal::StringLiteral Desc, class T>
using CmdShortOpt = ShortOpt<"COMMAND OPTIONS", ShortName, Desc, T>;

using Flag = bool;
using OptStr = std::optional<std::string>;
using OptStrVec = std::optional<std::vector<std::string>>;

struct CommonOptionsNoProgress {
    rfl::cli::Flag<rfl::cli::attr::Short<"v">, rfl::cli::attr::Desc<"Set verbose mode">> verbose = false;
};

struct CommonOptions {
    rfl::cli::Flag<rfl::cli::attr::Short<"v">, rfl::cli::attr::Desc<"Set verbose mode">> verbose = false;
    rfl::cli::Flag<rfl::cli::attr::Desc<"Display progress bar">> progress = false;
    rfl::cli::Flag<rfl::cli::attr::Desc<"Suppress display of progress bar">> no_progress = false;
};

struct SingleInputOptions {
    rfl::cli::Arg<OptStr, rfl::cli::attr::Short<"F">, rfl::cli::attr::Desc<"Format of input file">> input_format;
};

struct MultipleInputsOptions {
    rfl::cli::Arg<OptStr, rfl::cli::attr::Short<"F">, rfl::cli::attr::Desc<"Format of input files">> input_format;
};

struct OutputOptions {
    rfl::cli::Arg<OptStr, rfl::cli::attr::Short<"f">, rfl::cli::attr::Desc<"Format of output file">> output_format;
    rfl::cli::Flag<rfl::cli::attr::Desc<"Call fsync after writing file">> fsync = false;
    rfl::cli::Arg<OptStr, rfl::cli::attr::Desc<"Generator setting for file header">> generator;
    rfl::cli::Arg<OptStr, rfl::cli::attr::Short<"o">, rfl::cli::attr::Desc<"Output file">> output;
    rfl::cli::Flag<rfl::cli::attr::Short<"O">, rfl::cli::attr::Desc<"Allow existing output file to be overwritten">> overwrite = false;
    rfl::cli::Arg<OptStrVec, rfl::cli::attr::Desc<"Add output header">> output_header;
};

struct SingleInputPositional {
    rfl::cli::Arg<OptStr, rfl::cli::attr::Positional, rfl::cli::attr::Hidden> input_filename;
};

struct MultipleInputsPositional {
    rfl::cli::Arg<OptStrVec, rfl::cli::attr::Positional, rfl::cli::attr::Hidden> input_filenames;
};

using SubcommandApp = rfl::cli::App<"", "", "", rfl::cli::BuiltinHelp::short_and_long>;
using OsmiumCli = rfl::Processors<
    SubcommandApp,
    rfl::cli::GnuStyle,
    rfl::cli::ListDelimiter<"">>;

template <class Opts, class Cmd>
std::optional<Opts> parse_command_options(const Cmd& cmd,
                                          const std::string& description,
                                          const std::vector<std::string>& arguments) {
    const rfl::cli::UsageOptions usage_opts{
        .detailed = true,
        .synopsis = cmd.synopsis(),
        .description = description,
        .footer = std::string{"Use 'osmium help "} + cmd.name() + "' to display the manual page.",
        .help_group = "COMMON OPTIONS",
    };

    const auto result = rfl::cli::read_app<Opts, OsmiumCli>(
        std::string{"osmium "} + cmd.name(),
        arguments,
        usage_opts);

    if (!result) {
        throw cli_parse_error{result.error().what()};
    }

    if (const auto* info = std::get_if<rfl::cli::HelpOrVersion>(&*result)) {
        std::cout << info->text;
        return std::nullopt;
    }

    return std::move(std::get<Opts>(*result));
}

struct OptionMeta {
    std::string long_name;
    std::string short_name;
    std::string group;
    std::string description;
    std::string value_placeholder;
    std::optional<std::string> default_value;
    std::vector<std::string> choices;
    bool takes_value = false;
    bool is_repeatable = false;
    bool is_required = false;
};

struct PositionalMeta {
    std::string name;
    bool is_variadic = false;
};

struct CommandSpec {
    std::string name;
    std::string description;
    std::string synopsis;
    std::vector<OptionMeta> options;
    std::vector<PositionalMeta> positionals;
};

// NOTE (reflect-cpp gap): reflect-cpp does not yet expose a public CLI
// reflection/metadata API. We have to reach into rfl::cli::internal
// (cached_app_options, UsageWalker, builtin_usage_args) AND inspect
// options.positionals separately because UsageWalker omits rfl::Hidden fields
// (which osmium-tool uses to hide positional fields from the options list).
template <class Opts>
CommandSpec describe_options(const std::string& command_name,
                             const std::string& description,
                             const std::string& synopsis) {
    using ProcessorsType = rfl::cli::internal::cli_processors_t<OsmiumCli>;
    constexpr auto delimiter = rfl::cli::internal::list_delimiter_v<ProcessorsType>;
    const auto& [options, builtin] =
        rfl::cli::internal::cached_app_options<Opts, ProcessorsType, /*_all_paths=*/true>();

    auto walker = rfl::cli::internal::UsageWalker<ProcessorsType>(&options, delimiter);
    auto builtin_args = rfl::cli::internal::builtin_usage_args(builtin, "COMMON OPTIONS");
    auto fields = walker.template collect<Opts>();

    std::vector<rfl::cli::internal::UsageArg> merged;
    merged.reserve(builtin_args.size() + fields.size());
    const auto common_pos = std::find_if(
        fields.begin(), fields.end(),
        [](const rfl::cli::internal::UsageArg& arg) { return arg.group == "COMMON OPTIONS"; });
    if (common_pos != fields.end()) {
        merged.insert(merged.end(), fields.begin(), common_pos);
        merged.insert(merged.end(), builtin_args.begin(), builtin_args.end());
        merged.insert(merged.end(), common_pos, fields.end());
    } else {
        merged.insert(merged.end(), builtin_args.begin(), builtin_args.end());
        merged.insert(merged.end(), fields.begin(), fields.end());
    }

    CommandSpec spec;
    spec.name = command_name;
    spec.description = description;
    spec.synopsis = synopsis;

    for (const auto& arg : merged) {
        if (arg.positional) {
            continue;
        }
        const bool takes_value = !arg.bare && arg.kind != rfl::cli::internal::OptionKind::flag;
        spec.options.push_back(OptionMeta{
            .long_name = arg.name,
            .short_name = arg.short_name,
            .group = arg.group.empty() ? std::string{"COMMAND OPTIONS"} : arg.group,
            .description = arg.description,
            .value_placeholder = takes_value
                ? rfl::cli::internal::usage_value_to_string(arg, delimiter)
                : std::string{},
            .default_value = arg.default_value,
            .choices = arg.choices,
            .takes_value = takes_value,
            .is_repeatable = arg.is_list,
            .is_required = arg.required,
        });
    }

    for (const auto& pos_name : options.positionals) {
        const auto opt = options.find_long(pos_name);
        spec.positionals.push_back(PositionalMeta{
            .name = pos_name,
            .is_variadic = opt.is_list,
        });
    }

    return spec;
}

} // namespace osmium_cli

#endif // CLI_OPTIONS_HPP
