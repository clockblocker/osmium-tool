#ifndef CLI_OPTIONS_HPP
#define CLI_OPTIONS_HPP

#include "exception.hpp"

#include <rfl.hpp>
#include <rfl/cli.hpp>

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

} // namespace osmium_cli

#endif // CLI_OPTIONS_HPP
