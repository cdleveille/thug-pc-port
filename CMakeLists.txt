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

project(thug C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── xboxrecomp toolkit ──────────────────────────────────────────

set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root (git submodule by default)")

if(NOT EXISTS "${XBOXRECOMP_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "xboxrecomp not found at ${XBOXRECOMP_DIR}. "
        "Run: git submodule update --init --recursive")
endif()

# Builds the runtime libraries and provides the 'xboxrecomp' umbrella target.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── Recompiled game executable ──────────────────────────────────

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
    ${THUG_GEN_SOURCES}
)

target_include_directories(thug PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen
    ${XBOXRECOMP_DIR}/include
)

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
