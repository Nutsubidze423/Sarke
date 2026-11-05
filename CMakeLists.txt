cmake_minimum_required(VERSION 3.25)
project(kvm_demo LANGUAGES CXX RC)

if(NOT WIN32)
  message(FATAL_ERROR "kvm_demo builds on Windows only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT to match the x64-windows-static vcpkg triplet: exes run on A and B without redistributables.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
# Warnings are errors; headers included with <...> (Windows SDK, vcpkg) are external and not warned about.
add_compile_options(/W4 /WX /external:anglebrackets /external:W0 /permissive- /utf-8 /EHsc /Zc:__cplusplus)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(libjpeg-turbo CONFIG REQUIRED)
# TLS 1.3 for the channels between PCs (docs/SECURITY.md); linked statically like everything else.
find_package(OpenSSL REQUIRED)
# Opus for the shared app's sound (kvm/audio_codec.h, roadmap Phase 3).
find_package(Opus CONFIG REQUIRED)

# Module library: all <dir>/*.cpp except main.cpp, plus its include dir and link deps.
# Falls back to an INTERFACE library while the directory has no sources besides main.cpp.
function(kvm_add_module_lib name dir)
  file(GLOB sources CONFIGURE_DEPENDS ${dir}/*.cpp)
  list(REMOVE_ITEM sources ${dir}/main.cpp)
  if(sources)
    add_library(${name} STATIC ${sources})
    target_include_directories(${name} PUBLIC ${dir})
    target_link_libraries(${name} PUBLIC kvm_common ${ARGN})
  else()
    add_library(${name} INTERFACE)
    target_include_directories(${name} INTERFACE ${dir})
    target_link_libraries(${name} INTERFACE kvm_common ${ARGN})
  endif()
endfunction()

# Source files are globbed per directory, so adding a new .cpp needs no CMakeLists edit
# (fewer merge conflicts between tracks). Re-run configure is automatic via CONFIGURE_DEPENDS.
# Tests are registered in tools/CMakeLists.txt (labels unit / desktop / vdd / bench).
enable_testing()
add_subdirectory(common)
add_subdirectory(host_a)
add_subdirectory(client_b)
add_subdirectory(tools)

# libFuzzer targets (roadmap P1.12): only in the x64-fuzz preset. MSVC's /fsanitize=fuzzer,address;
# they compile header-only code only, so nothing they link is left uninstrumented.
option(SARKE_FUZZ "Build the libFuzzer targets in tools/fuzz" OFF)
if(SARKE_FUZZ)
  foreach(fuzz_target IN ITEMS fuzz_protocol fuzz_annexb)
    add_executable(${fuzz_target} tools/fuzz/${fuzz_target}.cpp)
    target_include_directories(${fuzz_target} PRIVATE common/include)
    target_link_libraries(${fuzz_target} PRIVATE nlohmann_json::nlohmann_json)
    target_compile_options(${fuzz_target} PRIVATE /fsanitize=fuzzer /fsanitize=address /Zi)
    target_link_options(${fuzz_target} PRIVATE /DEBUG /INCREMENTAL:NO)
    set_target_properties(${fuzz_target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools/fuzz)
  endforeach()
endif()

# Icon and version of Sarke's executables (resources/sarke.rc; the version is in resources/version.h).
function(sarke_resources target which)
  target_sources(${target} PRIVATE ${CMAKE_SOURCE_DIR}/resources/sarke.rc)
  target_compile_definitions(${target} PRIVATE ${which})
endfunction()
sarke_resources(kvm_host SARKE_EXE_HOST)
sarke_resources(kvm_client SARKE_EXE_CLIENT)
sarke_resources(sarke SARKE_EXE_LAUNCHER)
sarke_resources(sarke_driver SARKE_EXE_DRIVER)
