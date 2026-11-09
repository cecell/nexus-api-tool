cmake_minimum_required(VERSION 3.21)

# One self-contained exe: static CRT, so it runs without the Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")

project(nexus_api_tool VERSION 0.1.0 LANGUAGES CXX)

find_package(nlohmann_json CONFIG REQUIRED)

add_executable(nexus-api-tool src/main.cpp)
target_compile_features(nexus-api-tool PRIVATE cxx_std_17)
target_compile_definitions(nexus-api-tool PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(nexus-api-tool PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(nexus-api-tool PRIVATE nlohmann_json::nlohmann_json winhttp advapi32)
