#ifndef CLI_CODEGEN_HPP
#define CLI_CODEGEN_HPP

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

#include "cli_options.hpp"
#include "cmd.hpp"

#include <string>
#include <vector>

namespace osmium_cli {

struct CliCatalog {
    std::string program;
    std::string version;
    std::vector<CommandSpec> commands;
};

CliCatalog build_cli_catalog(const CommandFactory& command_factory);

std::string generate_zsh_completion(const CliCatalog& catalog);
std::string generate_bash_completion(const CliCatalog& catalog);
std::string generate_man_options(const CliCatalog& catalog, const std::string& command_filter = {});
std::string generate_json_catalog(const CliCatalog& catalog);

std::string generate_completions(const CommandFactory& command_factory,
                                 const std::string& format,
                                 const std::string& command_filter = {});

} // namespace osmium_cli

#endif // CLI_CODEGEN_HPP
