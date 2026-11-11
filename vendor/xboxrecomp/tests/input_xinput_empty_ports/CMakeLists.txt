cmake_minimum_required(VERSION 3.20)
project(input_xinput_empty_ports C)
# The XInput backend (src/input/xinput_device.c) against a fake XInput: how
# often an empty port is asked about, that a pad plugged in is found and one
# pulled out is let go, and that a connected pad is read on every call.
# Windows only: it is the Windows backend.
if(NOT WIN32)
    message(FATAL_ERROR "The XInput backend is Windows-only")
endif()
set(CMAKE_C_STANDARD 11)
enable_testing()
# The fake XInputGetState/SetState/GetCapabilities are in test_main.c, so no
# XInput library is linked.
add_executable(input_xinput_empty_ports_test test_main.c
    ../../src/input/xinput_device.c ../../src/input/keyboard.c
    ../../src/input/input_map.c ../../src/input/mkb_input.c)
target_include_directories(input_xinput_empty_ports_test PRIVATE
    ../../src/input ../../src)
target_compile_definitions(input_xinput_empty_ports_test PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX)
add_test(NAME input_xinput_empty_ports COMMAND input_xinput_empty_ports_test)
