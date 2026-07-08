cmake_minimum_required(VERSION 3.20)
project(live-whisper VERSION 0.0.1-alpha-gnome LANGUAGES C CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")

# ---------------------------------------------------------------------------
# System dependencies
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(WAYLAND_EGL    REQUIRED IMPORTED_TARGET wayland-egl)
pkg_check_modules(EGL            REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(GLESV2         REQUIRED IMPORTED_TARGET glesv2)
pkg_check_modules(XKBCOMMON      REQUIRED IMPORTED_TARGET xkbcommon)
pkg_check_modules(DBUS1          REQUIRED IMPORTED_TARGET dbus-1)

find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

# ---------------------------------------------------------------------------
# Generate xdg-shell protocol sources
# ---------------------------------------------------------------------------
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
set(XDG_XML ${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml)
set(XDG_HDR ${CMAKE_BINARY_DIR}/xdg-shell-client-protocol.h)
set(XDG_SRC ${CMAKE_BINARY_DIR}/xdg-shell-protocol.c)

add_custom_command(
    OUTPUT  ${XDG_HDR}
    COMMAND ${WAYLAND_SCANNER} client-header ${XDG_XML} ${XDG_HDR}
    DEPENDS ${XDG_XML}
    COMMENT "Generating xdg-shell client header"
)
add_custom_command(
    OUTPUT  ${XDG_SRC}
    COMMAND ${WAYLAND_SCANNER} private-code ${XDG_XML} ${XDG_SRC}
    DEPENDS ${XDG_XML}
    COMMENT "Generating xdg-shell protocol code"
)

# ---------------------------------------------------------------------------
# Generate xdg-decoration protocol sources (to request no window decorations)
# ---------------------------------------------------------------------------
set(XDG_DECORATION_XML ${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml)
set(XDG_DECORATION_HDR ${CMAKE_BINARY_DIR}/xdg-decoration-unstable-v1-client-protocol.h)
set(XDG_DECORATION_SRC ${CMAKE_BINARY_DIR}/xdg-decoration-unstable-v1-protocol.c)

add_custom_command(
    OUTPUT  ${XDG_DECORATION_HDR}
    COMMAND ${WAYLAND_SCANNER} client-header ${XDG_DECORATION_XML} ${XDG_DECORATION_HDR}
    DEPENDS ${XDG_DECORATION_XML}
    COMMENT "Generating xdg-decoration client header"
)
add_custom_command(
    OUTPUT  ${XDG_DECORATION_SRC}
    COMMAND ${WAYLAND_SCANNER} private-code ${XDG_DECORATION_XML} ${XDG_DECORATION_SRC}
    DEPENDS ${XDG_DECORATION_XML}
    COMMENT "Generating xdg-decoration protocol code"
)

add_library(wayland-protocols STATIC
    ${XDG_SRC} ${XDG_HDR}
    ${XDG_DECORATION_SRC} ${XDG_DECORATION_HDR}
)
target_include_directories(wayland-protocols PUBLIC ${CMAKE_BINARY_DIR})
target_link_libraries(wayland-protocols PUBLIC PkgConfig::WAYLAND_CLIENT)

# ---------------------------------------------------------------------------
# FetchContent dependencies
# ---------------------------------------------------------------------------
include(FetchContent)

# whisper.cpp
FetchContent_Declare(
    whisper
    GIT_REPOSITORY https://github.com/ggerganov/whisper.cpp.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(whisper)

# miniaudio (header-only)
FetchContent_Declare(
    miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_Populate(miniaudio)
add_library(miniaudio_hdr INTERFACE)
target_include_directories(miniaudio_hdr INTERFACE ${miniaudio_SOURCE_DIR})

# Dear ImGui
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_Populate(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_ES3)
target_link_libraries(imgui PUBLIC PkgConfig::GLESV2)

# Embedded font (DroidSans from ImGui's bundled fonts)
set(GEN_DIR ${CMAKE_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN_DIR})

find_file(BINARY_TO_COMPRESSED binary_to_compressed_c.cpp
          PATHS ${imgui_SOURCE_DIR}/misc/fonts)
add_executable(binary_to_compressed_tool ${BINARY_TO_COMPRESSED})

add_custom_command(
    OUTPUT  ${GEN_DIR}/gen_font.cpp
    COMMAND binary_to_compressed_tool
            ${imgui_SOURCE_DIR}/misc/fonts/DroidSans.ttf
            gen_font > ${GEN_DIR}/gen_font.cpp
    DEPENDS binary_to_compressed_tool
    COMMENT "Generating embedded font"
)
add_custom_target(generate_font ALL DEPENDS ${GEN_DIR}/gen_font.cpp)

# ---------------------------------------------------------------------------
# Model download
# ---------------------------------------------------------------------------
set(MODEL_DIR  ${CMAKE_BINARY_DIR}/models)
set(MODEL_FILE ${MODEL_DIR}/ggml-tiny.bin)

if(NOT EXISTS ${MODEL_FILE})
    file(MAKE_DIRECTORY ${MODEL_DIR})
    add_custom_command(
        OUTPUT  ${MODEL_FILE}
        COMMAND ${CMAKE_COMMAND} -E echo "Downloading ggml-tiny.bin model..."
        COMMAND curl -L -o ${MODEL_FILE}
                "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.bin"
        COMMENT "Downloading whisper tiny model"
    )
    add_custom_target(download-model ALL DEPENDS ${MODEL_FILE})
endif()

# ---------------------------------------------------------------------------
# Main executable
# ---------------------------------------------------------------------------
add_executable(live-whisper
    src/main.cpp
    src/audio.cpp
    src/transcriber.cpp
    src/overlay.cpp
    src/imgui_impl_wayland.cpp
    src/paste.cpp
    src/font.cpp
)
add_dependencies(live-whisper generate_font)

target_include_directories(live-whisper PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}
    ${GEN_DIR}
)

target_link_libraries(live-whisper PRIVATE
    whisper
    miniaudio_hdr
    imgui
    wayland-protocols
    PkgConfig::WAYLAND_CLIENT
    PkgConfig::WAYLAND_EGL
    PkgConfig::EGL
    PkgConfig::GLESV2
    PkgConfig::XKBCOMMON
    PkgConfig::DBUS1
    pthread
    dl
    m
)

target_compile_definitions(live-whisper PRIVATE
    LIVE_WHISPER_DATADIR="${CMAKE_INSTALL_FULL_DATAROOTDIR}/live-whisper"
)

# ---------------------------------------------------------------------------
# Install rules
# ---------------------------------------------------------------------------
install(TARGETS live-whisper RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${MODEL_FILE} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/live-whisper)

# GNOME Shell extension for text injection
install(DIRECTORY extension/ DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/gnome-shell/extensions/live-whisper@local
        FILES_MATCHING PATTERN "*.json" PATTERN "*.js")
