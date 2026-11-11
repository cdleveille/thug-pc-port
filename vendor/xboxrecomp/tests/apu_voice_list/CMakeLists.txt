cmake_minimum_required(VERSION 3.20)
project(apu_voice_list_test C)
# test_main.c compiles apu_vp.c itself, to run a frame on a hand-built voice
# list, and stands in for the little it uses from the rest of the APU (as
# tests/apu_vp does). AddressSanitizer turns an out-of-range voice handle's
# write into a hard failure. POSIX only: it links the platform layer for the
# Interlocked* calls; tests/apu_vp covers Windows.
if(WIN32)
    message(FATAL_ERROR "POSIX test; tests/apu_vp covers Windows")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(apu_voice_list_test test_main.c)
target_include_directories(apu_voice_list_test PRIVATE
    ../../src ../../src/apu ../../src/platform)
target_compile_options(apu_voice_list_test PRIVATE
    -fsanitize=address -fno-omit-frame-pointer)
target_link_options(apu_voice_list_test PRIVATE -fsanitize=address)
target_link_libraries(apu_voice_list_test PRIVATE platform m)
enable_testing()
add_test(NAME apu_voice_list COMMAND apu_voice_list_test)
