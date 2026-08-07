cmake_minimum_required(VERSION 3.20)
project(segcap LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
  message(FATAL_ERROR "This project targets MSVC on Windows only.")
endif()

# The injected DLL shares a process with the game. Static-link the CRT so we
# never depend on a runtime DLL the host process has not loaded, and never
# fight the host over a shared CRT's state.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  UNICODE
  _UNICODE
)

add_compile_options(/W4 /permissive- /utf-8)

# ---------------------------------------------------------------------------
# testapp -- D3D12 ground-truth fixture
#
# Renders a grid of quads writing known stencil values. The capture layer is
# developed against this before it ever touches a shipped game.
# ---------------------------------------------------------------------------
add_executable(d3d12_testapp src/testapp/d3d12_testapp.cpp)
target_link_libraries(d3d12_testapp PRIVATE d3d12 dxgi d3dcompiler)
set_target_properties(d3d12_testapp PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
