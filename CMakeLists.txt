cmake_minimum_required(VERSION 3.20)

# ============================================================================
# THUG PC Port: native Windows/Linux build of Tony Hawk's Underground (Xbox,
# NTSC-U, 2003) by static recompilation.
#
# The game's x86 code is lifted to C by the xboxrecomp toolkit
# (third_party/xboxrecomp). scripts/regen.sh writes the generated C into
# src/recomp/gen/, and this file compiles it together with the toolkit's
# runtime libraries into one native executable.
# ============================================================================

project(thug C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── xboxrecomp toolkit ──────────────────────────────────────────

set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root (vendored in third_party/ by default)")

if(NOT EXISTS "${XBOXRECOMP_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "xboxrecomp not found at ${XBOXRECOMP_DIR}. It is part of this "
        "repository (third_party/xboxrecomp); is the checkout complete?")
endif()

# Builds the runtime libraries and provides the 'xboxrecomp' umbrella target.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── Recompiled game executable ──────────────────────────────────

# ── Game files: the start-up check and disc unpacking ─────────

# The same code the game runs at start-up (src/host/game_data.c), as a
# command: lifting needs game/default.xbe before the game itself can be
# built, so scripts/build.sh builds and runs this first.
add_executable(thug-extract src/tools/thug_extract.c src/host/game_data.c)
target_include_directories(thug-extract PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(thug-extract PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Empty until scripts/regen.sh has run. CONFIGURE_DEPENDS re-globs when the
# pipeline adds or removes chunks, so a regen never leaves a file out.
file(GLOB THUG_GEN_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen/*.c")

if(NOT THUG_GEN_SOURCES)
    message(WARNING
        "No generated sources in src/recomp/gen/. Run scripts/regen.sh first; "
        "the executable will not link without them.")
endif()

add_executable(thug
    src/main.c
    src/recomp_manual.c
    src/gpu/nv2a_pb_gl.c
    src/host/game_data.c
    src/host/host_window.c
    src/host/launch.c
    src/ui/settings.c
    ${THUG_GEN_SOURCES}
)

target_include_directories(thug PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen
    ${XBOXRECOMP_DIR}/include
    # nv2a_backend.h / nv2a_combiner.h: the executor's back-end interface the
    # GPU renderer (src/gpu) implements.
    ${XBOXRECOMP_DIR}/src/kernel
)

# GPU renderer: OpenGL 3.3 through SDL2 + libepoxy, as the toolkit's POSIX
# graphics already use. On Windows the file compiles to a stub for now.
if(NOT WIN32)
    find_package(SDL2 REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(THUG_EPOXY REQUIRED epoxy)
    target_link_libraries(thug PRIVATE SDL2::SDL2 ${THUG_EPOXY_LINK_LIBRARIES})
    target_include_directories(thug PRIVATE ${THUG_EPOXY_INCLUDE_DIRS})

    # The in-game settings overlay (F1): Dear ImGui (vendored), drawn
    # by the GPU renderer through epoxy.
    set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
    if(NOT EXISTS "${IMGUI_DIR}/imgui.cpp")
        message(FATAL_ERROR "Dear ImGui not found at ${IMGUI_DIR}. It is part "
                            "of this repository; is the checkout complete?")
    endif()
    target_sources(thug PRIVATE
        src/ui/overlay.cpp
        src/ui/imgui_gl_backend.cpp
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp)
    target_include_directories(thug PRIVATE ${IMGUI_DIR})
endif()

target_link_libraries(thug PRIVATE xboxrecomp)

# The lifted code models x86 exactly: integer arithmetic wraps, and guest
# memory is one byte array read through casts at every width. Tell the C
# compiler so, rather than letting -O2 reason about overflow or aliasing that
# the original program never had.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
    set_source_files_properties(${THUG_GEN_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-fwrapv;-fno-strict-aliasing;-w")
endif()

# ── Platform specifics ──────────────────────────────────────────

if(WIN32)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(thug PRIVATE
        d3d11
        dxgi
        dxguid
        winmm
        dbghelp
        $<IF:$<BOOL:${MINGW}>,xinput1_4,xinput>
    )
    if(MSVC)
        target_link_options(thug PRIVATE /LARGEADDRESSAWARE)
        # Generated chunks are large; raise the object section limit.
        target_compile_options(thug PRIVATE /bigobj)
    endif()
else()
    # The toolkit's platform target already links Threads and libm.
    # -rdynamic lets backtrace_symbols() in the crash reporter name the
    # lifted function a fault happened in (sub_XXXXXXXX).
    target_link_options(thug PRIVATE -rdynamic)
endif()

set_target_properties(thug PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
