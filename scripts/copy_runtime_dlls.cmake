# Copy the DLLs an executable needs from the toolchain (MSYS2 CLANG64: SDL2,
# libepoxy, libc++, winpthread) next to it, so it runs from Explorer or any
# shell, not only one with the toolchain on PATH. Windows' own DLLs stay
# where they are.
#
#   cmake -DEXE=<path to .exe> -DSEARCH=<dir;dir...> -P copy_runtime_dlls.cmake
#
# Run by CMakeLists.txt after each link of the game.

# Paths are matched with forward slashes (CMake 4.1+); the regex below takes
# either kind.
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()

if(NOT EXE OR NOT EXISTS "${EXE}")
    message(FATAL_ERROR "copy_runtime_dlls: EXE not set or missing (${EXE})")
endif()
get_filename_component(dest "${EXE}" DIRECTORY)

# The resolver looks in the executable's own folder first, so a copy left
# there by an earlier build would be found again and kept, however the
# toolchain has changed since (MSYS2 replacing SDL2 with sdl2-compat left the
# old SDL2.dll, and SDL3.dll, which the new one needs, never came). So first
# remove every DLL here that the toolchain also has; the ones still needed
# are copied again below from the toolchain.
file(GLOB present "${dest}/*.dll")
foreach(p IN LISTS present)
    get_filename_component(name "${p}" NAME)
    foreach(dir IN LISTS SEARCH)
        if(EXISTS "${dir}/${name}")
            file(REMOVE "${p}")
            break()
        endif()
    endforeach()
endforeach()

file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${EXE}"
    DIRECTORIES ${SEARCH}
    RESOLVED_DEPENDENCIES_VAR found
    UNRESOLVED_DEPENDENCIES_VAR missing
    PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
    POST_EXCLUDE_REGEXES "[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\]")

# DLLs loaded with LoadLibrary rather than imported, which the resolver above
# cannot see: sdl2-compat (MSYS2's SDL2 since 2026) loads SDL3.dll at start-up.
# Each pair is <DLL found above>:<DLL it names and loads>, taken when the
# first one's own text mentions the second.
set(companions "SDL2.dll:SDL3.dll")
set(extra "")
foreach(pair IN LISTS companions)
    string(REPLACE ":" ";" pair "${pair}")
    list(GET pair 0 loader)
    list(GET pair 1 loaded)
    foreach(dll IN LISTS found)
        get_filename_component(name "${dll}" NAME)
        if(name STREQUAL loader)
            string(REPLACE "." "\\." pattern "${loaded}")
            file(STRINGS "${dll}" hits REGEX "${pattern}" LIMIT_COUNT 1)
            foreach(dir IN LISTS SEARCH)
                if(hits AND EXISTS "${dir}/${loaded}")
                    list(APPEND extra "${dir}/${loaded}")
                    break()
                endif()
            endforeach()
        endif()
    endforeach()
endforeach()
if(extra)
    # And whatever those import in turn.
    file(GET_RUNTIME_DEPENDENCIES
        LIBRARIES ${extra}
        DIRECTORIES ${SEARCH}
        RESOLVED_DEPENDENCIES_VAR extra_found
        UNRESOLVED_DEPENDENCIES_VAR extra_missing
        PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
        POST_EXCLUDE_REGEXES "[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\]")
    list(APPEND found ${extra} ${extra_found})
    list(APPEND missing ${extra_missing})
    list(REMOVE_DUPLICATES found)
endif()

foreach(dll IN LISTS found)
    file(COPY "${dll}" DESTINATION "${dest}")
endforeach()
if(missing)
    message(WARNING "copy_runtime_dlls: not found: ${missing}")
endif()
