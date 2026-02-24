cmake_minimum_required(VERSION 3.20)
project(live-whisper LANGUAGES C CXX)

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

find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

# ---------------------------------------------------------------------------
# Generate xdg-shell protocol sources (needed by layer-shell)
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
# Generate layer-shell protocol sources
# ---------------------------------------------------------------------------
set(PROTO_XML ${CMAKE_SOURCE_DIR}/protocol/wlr-layer-shell-unstable-v1.xml)
set(PROTO_HDR ${CMAKE_BINARY_DIR}/wlr-layer-shell-unstable-v1-client-protocol.h)
set(PROTO_SRC ${CMAKE_BINARY_DIR}/wlr-layer-shell-unstable-v1-protocol.c)

add_custom_command(
    OUTPUT  ${PROTO_HDR}
    COMMAND ${WAYLAND_SCANNER} client-header ${PROTO_XML} ${PROTO_HDR}
    COMMAND ${CMAKE_COMMAND} -DHEADER=${PROTO_HDR} -P ${CMAKE_SOURCE_DIR}/cmake/fix-namespace.cmake
    DEPENDS ${PROTO_XML}
    COMMENT "Generating layer-shell client header (C++ safe)"
)
add_custom_command(
    OUTPUT  ${PROTO_SRC}
    COMMAND ${WAYLAND_SCANNER} private-code ${PROTO_XML} ${PROTO_SRC}
    DEPENDS ${PROTO_XML}
    COMMENT "Generating layer-shell protocol code"
)

# ---------------------------------------------------------------------------
# Generate virtual-keyboard protocol sources
# ---------------------------------------------------------------------------
set(VKBD_XML ${CMAKE_SOURCE_DIR}/protocol/virtual-keyboard-unstable-v1.xml)
set(VKBD_HDR ${CMAKE_BINARY_DIR}/virtual-keyboard-unstable-v1-client-protocol.h)
set(VKBD_SRC ${CMAKE_BINARY_DIR}/virtual-keyboard-unstable-v1-protocol.c)

add_custom_command(
    OUTPUT  ${VKBD_HDR}
    COMMAND ${WAYLAND_SCANNER} client-header ${VKBD_XML} ${VKBD_HDR}
    DEPENDS ${VKBD_XML}
    COMMENT "Generating virtual-keyboard client header"
)
add_custom_command(
    OUTPUT  ${VKBD_SRC}
    COMMAND ${WAYLAND_SCANNER} private-code ${VKBD_XML} ${VKBD_SRC}
    DEPENDS ${VKBD_XML}
    COMMENT "Generating virtual-keyboard protocol code"
)

add_library(wlr-layer-shell-protocol STATIC
    ${PROTO_SRC} ${PROTO_HDR}
    ${XDG_SRC} ${XDG_HDR}
    ${VKBD_SRC} ${VKBD_HDR}
)
target_include_directories(wlr-layer-shell-protocol PUBLIC ${CMAKE_BINARY_DIR})
target_link_libraries(wlr-layer-shell-protocol PUBLIC PkgConfig::WAYLAND_CLIENT)

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

# miniaudio (header-only — skip its CMakeLists, we just need the header)
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
    wlr-layer-shell-protocol
    PkgConfig::WAYLAND_CLIENT
    PkgConfig::WAYLAND_EGL
    PkgConfig::EGL
    PkgConfig::GLESV2
    PkgConfig::XKBCOMMON
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
