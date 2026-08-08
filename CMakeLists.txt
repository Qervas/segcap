cmake_minimum_required(VERSION 3.20)
# C is required as well as CXX: MinHook is C, and without it CMake reports
# "can not determine linker language for target: minhook".
project(segcap LANGUAGES C CXX)

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
# minhook -- built directly rather than via its own CMakeLists, which pulls in
# DLL packaging targets and options we do not want. It is four C files.
# ---------------------------------------------------------------------------
add_library(minhook STATIC
  third_party/minhook/src/buffer.c
  third_party/minhook/src/hook.c
  third_party/minhook/src/trampoline.c
  third_party/minhook/src/hde/hde64.c
)
target_include_directories(minhook PUBLIC third_party/minhook/include)
# Third-party C; do not hold it to our warning level.
target_compile_options(minhook PRIVATE /W0)

# ---------------------------------------------------------------------------
# injector -- gets segcap.dll into a target process
# ---------------------------------------------------------------------------
add_executable(injector src/injector/injector.cpp)
set_target_properties(injector PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ---------------------------------------------------------------------------
# segcap -- the injected capture DLL
# ---------------------------------------------------------------------------
add_library(segcap SHARED
  src/segcap/dllmain.cpp
  src/segcap/hooks.cpp
  src/segcap/readback.cpp
)
target_link_libraries(segcap PRIVATE minhook d3d12 dxgi)
set_target_properties(segcap PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

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
