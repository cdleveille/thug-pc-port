/**
 * imgui_gl_backend.cpp -- Dear ImGui's OpenGL 3 renderer, built against
 * glad (the loader the GPU renderer already uses) instead of ImGui's own
 * embedded GL loader, so both draw through the same function pointers on the
 * same context.
 */
#include <glad/gl.h>
#define IMGUI_IMPL_OPENGL_LOADER_CUSTOM
#include "backends/imgui_impl_opengl3.cpp"
