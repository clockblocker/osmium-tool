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
using Opt = rfl::Group<GroupName, rfl::Description<Desc, T>>;

template <rfl::internal::StringLiteral GroupName, rfl::internal::StringLiteral ShortName,
          rfl::internal::StringLiteral Desc, class T>
using ShortOpt = rfl::Group<GroupName, rfl::Short<ShortName, rfl::Description<Desc, T>>>;

template <rfl::internal::StringLiteral Desc, class T>
using CmdOpt = Opt<"COMMAND OPTIONS", Desc, T>;

template <rfl::internal::StringLiteral ShortName, rfl::internal::StringLiteral Desc, class T>
using CmdShortOpt = ShortOpt<"COMMAND OPTIONS", ShortName, Desc, T>;

using Flag = rfl::DefaultVal<bool>;
using OptStr = std::optional<std::string>;
using OptStrVec = std::optional<std::vector<std::string>>;

struct CommonOptionsNoProgress {
    ShortOpt<"COMMON OPTIONS", "v", "Set verbose mode", Flag> verbose = false;
};

struct CommonOptions {
    ShortOpt<"COMMON OPTIONS", "v", "Set verbose mode", Flag> verbose = false;
    Opt<"COMMON OPTIONS", "Display progress bar", Flag> progress = false;
    Opt<"COMMON OPTIONS", "Suppress display of progress bar", Flag> no_progress = false;
};

struct SingleInputOptions {
    ShortOpt<"INPUT OPTIONS", "F", "Format of input file", OptStr> input_format;
};

struct MultipleInputsOptions {
    ShortOpt<"INPUT OPTIONS", "F", "Format of input files", OptStr> input_format;
};

struct OutputOptions {
    ShortOpt<"OUTPUT OPTIONS", "f", "Format of output file", OptStr> output_format;
    Opt<"OUTPUT OPTIONS", "Call fsync after writing file", Flag> fsync = false;
    Opt<"OUTPUT OPTIONS", "Generator setting for file header", OptStr> generator;
    ShortOpt<"OUTPUT OPTIONS", "o", "Output file", OptStr> output;
    ShortOpt<"OUTPUT OPTIONS", "O", "Allow existing output file to be overwritten", Flag> overwrite = false;
    Opt<"OUTPUT OPTIONS", "Add output header", OptStrVec> output_header;
};

struct SingleInputPositional {
    rfl::Hidden<rfl::Positional<OptStr>> input_filename;
};

struct MultipleInputsPositional {
    rfl::Hidden<rfl::Positional<OptStrVec>> input_filenames;
};

using SubcommandApp = rfl::cli::App<"", "", "", rfl::cli::BuiltinHelp::short_and_long>;

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

    const auto result = rfl::cli::read_app<
        Opts,
        SubcommandApp,
        rfl::NoExtraFields,
        rfl::cli::SeparateOptionValues,
        rfl::cli::ListDelimiter<"">,
        rfl::cli::PureFlags>(
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
