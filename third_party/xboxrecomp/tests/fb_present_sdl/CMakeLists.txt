cmake_minimum_required(VERSION 3.20)
project(fb_present_sdl_test C)
# The SDL framebuffer presenter, compiled directly into the test (the way
# tests/fb_present does the GDI one). It never opens a window, so no display
# is needed -- only the SDL2 headers and library the presenter includes.
if(WIN32)
    message(FATAL_ERROR "fb_present_sdl.c is the POSIX presenter")
endif()
set(CMAKE_C_STANDARD 11)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
find_package(Threads REQUIRED)
add_executable(fb_present_sdl_test test_main.c)
target_include_directories(fb_present_sdl_test PRIVATE
    ../../src
    ../../src/input
    ../../src/platform
    ../../src/video
)
target_link_libraries(fb_present_sdl_test PRIVATE PkgConfig::SDL2 Threads::Threads)
enable_testing()
add_test(NAME fb_present_sdl COMMAND fb_present_sdl_test)
