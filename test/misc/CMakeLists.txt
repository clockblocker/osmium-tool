#-----------------------------------------------------------------------------
#
#  CMake Config
#
#  Osmium Tool Tests - misc
#
#-----------------------------------------------------------------------------

do_test(version1 "osmium --version" "^osmium version ${PROJECT_VERSION}")
do_test(version2 "osmium version"   "^osmium version ${PROJECT_VERSION}")

do_test(unknown_command "osmium x" "^Unknown command or option 'x'. Try 'osmium help'.\n")

do_test(completions_zsh  "osmium --generate-completions zsh"         "^#compdef osmium")
do_test(completions_bash "osmium --generate-completions bash"        "complete -F _osmium osmium")
do_test(completions_man  "osmium --generate-completions man-options export" "# COMMAND OPTIONS")
do_test(completions_json "osmium --generate-completions json"        "\"program\": \"osmium\"")


#-----------------------------------------------------------------------------
