cmake_minimum_required(VERSION 3.20)

# ============================================================================
# THUG PC Port: native Windows/Linux build of Tony Hawk's Underground (Xbox,
# NTSC-U, 2003) by static recompilation.
#
# The game's x86 code is lifted to C by the xboxrecomp toolkit
# (vendor/xboxrecomp). scripts/regen.sh writes the generated C into
# src/recomp/gen/, and this file compiles it together with the toolkit's
# runtime libraries into one native executable.
# ============================================================================

project(thug C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── xboxrecomp toolkit ──────────────────────────────────────────

set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root (vendor/xboxrecomp by default)")

if(NOT EXISTS "${XBOXRECOMP_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "xboxrecomp not found at ${XBOXRECOMP_DIR}. It is part of this "
        "repository (vendor/xboxrecomp); is the checkout complete?")
endif()

# ── SDL ───────────────────────────────────────────────────────────

# Linux: SDL 2.32 (vendor/SDL) linked into the executable, so a player needs
# no SDL package. SDL still loads X11, Wayland and the sound libraries at run
# time. Windows keeps the toolchain's DLLs (docs/NOTES.md, "Decisions").
if(NOT WIN32)
    option(THUG_BUNDLED_SDL "Link SDL2 statically from vendor/SDL" ON)
endif()
if(THUG_BUNDLED_SDL)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_STATIC_PIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)
    add_subdirectory(vendor/SDL ${CMAKE_BINARY_DIR}/SDL EXCLUDE_FROM_ALL)
    # The toolkit's find_package(SDL2) calls land here, on SDL2::SDL2 (the
    # static library), instead of on a system SDL.
    file(WRITE "${CMAKE_BINARY_DIR}/sdl2-bundled/SDL2Config.cmake" "")
    set(SDL2_DIR "${CMAKE_BINARY_DIR}/sdl2-bundled" CACHE PATH "" FORCE)
endif()

# THUG draws through the pushbuffer executor, never the toolkit's D3D8 layer,
# whose POSIX back end needs libepoxy: only its headers, which other toolkit
# code includes.
if(NOT WIN32)
    add_library(xbox_d3d8 INTERFACE)
    target_include_directories(xbox_d3d8 INTERFACE "${XBOXRECOMP_DIR}/src/d3d")
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

# ── The window icon ─────────────────────────────────────────────

# thug.png, embedded as RGBA (SDL2 reads only BMP by itself, and a PNG
# library is not worth a dependency for one image). Python is already needed
# for lifting; replacing thug.png is all it takes to change the icon.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(THUG_ICON_SOURCE "${CMAKE_BINARY_DIR}/generated/thug_icon.c")
add_custom_command(
    OUTPUT "${THUG_ICON_SOURCE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND Python3::Interpreter -I "${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_icon.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/thug.png" 256 "${THUG_ICON_SOURCE}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/thug.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_icon.py"
    COMMENT "Embedding thug.png as the window icon"
    VERBATIM)

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
    src/gpu/xbr.c
    src/host/controls.c
    src/host/game_data.c
    src/host/host_sleep.c
    src/host/host_window.c
    src/host/launch.c
    src/host/prof_sampler.c
    src/host/restart.c
    src/host/window_icon.c
    src/ui/settings.c
    ${THUG_ICON_SOURCE}
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

# The keyboard bindings use the toolkit's input headers (src/input, which
# reach src/platform): for that one file, so they shadow nothing else.
set_source_files_properties(src/host/controls.c PROPERTIES
    INCLUDE_DIRECTORIES "${XBOXRECOMP_DIR}/src")

# GPU renderer: OpenGL 3.3 through SDL2 and glad (vendor/glad, generated for
# GL 3.3 core and the extensions the renderer checks for), on Linux and
# Windows (docs/WINDOWS.md). The toolkit's own D3D11 path is not THUG's: the
# game renders through the pushbuffer executor.
if(NOT THUG_BUNDLED_SDL)
    find_package(SDL2 REQUIRED)
endif()
find_package(Threads REQUIRED)
add_library(thug_glad STATIC vendor/glad/src/gl.c)
target_include_directories(thug_glad PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/vendor/glad/include")
target_link_libraries(thug PRIVATE SDL2::SDL2 thug_glad Threads::Threads)
# SDL is not handed main(): main.c is an ordinary entry point everywhere.
target_compile_definitions(thug PRIVATE SDL_MAIN_HANDLED)

# The in-game settings overlay (F1): Dear ImGui (vendored), drawn
# by the GPU renderer through glad.
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui")
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
    else()
        # Every thread's stack reserve, as a Linux thread's default: lifted
        # code recurses on the native stack, and the toolkit starts the
        # title's threads with CreateThread's default (MinGW's is 2 MB). The
        # guest's main thread asks for 64 MB itself (host_window.c).
        target_link_options(thug PRIVATE "-Wl,--stack,8388608")
    endif()
    if(MINGW)
        # A windowed program: started from Explorer, no console window opens
        # beside the game. The log goes to %LOCALAPPDATA%\thug-pc-port\thug.log
        # (src/host/launch.c); a terminal it was started from still gets the
        # player's messages, and redirecting stderr still works.
        option(THUG_WINDOWS_CONSOLE
               "Build thug.exe as a console program (a console window opens with it)" OFF)
        if(NOT THUG_WINDOWS_CONSOLE)
            target_link_options(thug PRIVATE -mwindows)
        endif()
        # The toolchain's DLLs (SDL2, libc++, winpthread) beside the
        # game, so build/bin/thug.exe starts from Explorer too.
        get_filename_component(THUG_TOOLCHAIN_BIN "${CMAKE_C_COMPILER}" DIRECTORY)
        add_custom_command(TARGET thug POST_BUILD
            COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:thug>
                    "-DSEARCH=${THUG_TOOLCHAIN_BIN}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/scripts/copy_runtime_dlls.cmake"
            COMMENT "Copying the runtime DLLs next to thug.exe"
            VERBATIM)
    endif()
else()
    # The toolkit's platform target already links Threads and libm.
    # -rdynamic lets backtrace_symbols() in the crash reporter name the
    # lifted function a fault happened in (sub_XXXXXXXX).
    target_link_options(thug PRIVATE -rdynamic)
    # The C++ runtime (ImGui) inside the executable too: only glibc and the
    # GL driver come from the system.
    target_link_options(thug PRIVATE -static-libstdc++ -static-libgcc)
endif()

set_target_properties(thug PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ── Tests (ctest --test-dir build) ──────────────────────────────

enable_testing()

# The xBR magnifier behind "Smooth text and icons" (src/gpu/xbr.c).
add_executable(thug-xbr-test tests/xbr_test.c src/gpu/xbr.c)
target_include_directories(thug-xbr-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu)
if(NOT MSVC)
    target_link_libraries(thug-xbr-test PRIVATE m)
endif()
add_test(NAME xbr COMMAND thug-xbr-test)
