# RetroDeck: the launcher and the Amiga front end. The other emulators are
# built by toolchain/setup.sh; ./build.sh does both.
cmake_minimum_required(VERSION 3.16)
project(RetroDeck LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
find_package(ZLIB REQUIRED)

# Text, shapes and images, shared by both programs
add_library(rdcommon STATIC src/common/font.cpp src/common/draw.cpp)
target_include_directories(rdcommon PUBLIC src/common)
target_include_directories(rdcommon SYSTEM PUBLIC third_party/stb)
target_link_libraries(rdcommon PUBLIC PkgConfig::SDL2 m)

add_executable(retrodeck
    src/launcher/main.cpp
    src/launcher/library.cpp
    src/launcher/zip.cpp
    src/launcher/settings.cpp
    src/launcher/pads.cpp
    src/launcher/displays.cpp
    src/launcher/emulators.cpp)
target_link_libraries(retrodeck PRIVATE rdcommon ZLIB::ZLIB)
target_compile_options(retrodeck PRIVATE -Wall -Wextra)

# The Amiga front end on the vAmiga core (toolchain/setup.sh vamiga)
set(VAMIGA_CORE "${CMAKE_CURRENT_SOURCE_DIR}/toolchain/src/vAmiga/Core")
if(EXISTS "${VAMIGA_CORE}/CMakeLists.txt")
    add_subdirectory("${VAMIGA_CORE}" vamiga EXCLUDE_FROM_ALL)
    # this vAmiga commit uses std::format without including <format>
    target_compile_options(VACore PRIVATE -include format -w)
    add_executable(retrodeck-amiga src/amiga/main.cpp)
    target_link_libraries(retrodeck-amiga PRIVATE rdcommon VACore)
    target_compile_options(retrodeck-amiga PRIVATE -include format -Wall)
else()
    message(WARNING "vAmiga core missing (toolchain/setup.sh vamiga): no Amiga front end")
endif()
