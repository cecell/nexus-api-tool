cmake_minimum_required(VERSION 3.21)

# One self-contained exe: static CRT, so it runs without the Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")

# The one place for the version: the exe and the release workflow both read it from here.
project(nexus_api_tool VERSION 0.3.0 LANGUAGES CXX)

find_package(nlohmann_json CONFIG REQUIRED)

add_executable(nexus-api-tool src/main.cpp)
target_compile_features(nexus-api-tool PRIVATE cxx_std_17)
target_compile_definitions(nexus-api-tool PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS APP_VERSION="${PROJECT_VERSION}")
target_compile_options(nexus-api-tool PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(nexus-api-tool PRIVATE nlohmann_json::nlohmann_json winhttp advapi32 bcrypt crypt32)

# Command-line checks that need neither the network nor an API key.
enable_testing()
function(add_cli_test name expected)
    add_test(NAME ${name} COMMAND nexus-api-tool ${ARGN})
    set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "${expected}")
endfunction()
add_cli_test(version "nexus-api-tool ${PROJECT_VERSION}" --version)
add_cli_test(help "USAGE" --help)
add_cli_test(unknown_command "unknown command nope" nope 1)
add_cli_test(bad_mod_id "the mod id must be a number" files abc)
add_cli_test(missing_dest "dl-file needs --dest" dl-file 790 937)
add_cli_test(limit_too_high "--limit must be 1 to 200" dependents 266 --limit 201)
add_cli_test(limit_zero "--limit must be 1 to 200" dependents 266 --limit 0)
add_cli_test(limit_wrong_command "only apply to reqs, dependents and list-mods" files 266 --limit 5)
add_cli_test(set_desc_needs_file "set-desc needs --file" set-desc 266)
add_cli_test(save_desc_needs_dest "save-desc needs --dest" save-desc 266)
add_cli_test(upload_needs_name "upload-file needs --name and --file-version" upload-file 266 --file x.7z)
add_cli_test(option_wrong_command "--file doesn't apply to desc" desc 266 --file x)
add_cli_test(list_mods_needs_user "list-mods needs a user name" list-mods)
add_cli_test(permissions_bad_address "must be a number or a mod page" permissions https://example.com/x)
add_cli_test(permissions_saved_page "You must get permission from me" permissions 1 --html ${CMAKE_CURRENT_SOURCE_DIR}/tests/permissions_sample.html)
