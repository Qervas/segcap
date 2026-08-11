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
# ViGEmClient -- user-mode SDK for the virtual gamepad bus driver.
# Built from source (one .cpp) rather than consuming a prebuilt lib, so it
# matches our CRT settings.
# ---------------------------------------------------------------------------
add_library(vigemclient STATIC third_party/ViGEmClient/src/ViGEmClient.cpp)
target_include_directories(vigemclient PUBLIC third_party/ViGEmClient/include)
target_compile_options(vigemclient PRIVATE /W0)   # third-party; not our warning level

# ---------------------------------------------------------------------------
# vpad -- virtual gamepad driver for unattended capture runs
# ---------------------------------------------------------------------------
add_executable(vpad src/vpad/vpad.cpp)
target_link_libraries(vpad PRIVATE vigemclient setupapi)
set_target_properties(vpad PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

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
  # hooks.cpp was 2332 lines doing hook installation, ten D3D12 detours, the
  # resource-state shadow, target election, the id-buffer probe and mask writing.
  # These remain ONE class Hooks across several translation units rather than
  # becoming separate classes, deliberately: a single non-recursive mutex spans
  # them in both directions (NoteResourceCreated and SnapshotTargets each erase
  # the other's maps inside one lock, and that atomicity IS the address-recycling
  # invariant), and the election reads state the capture path writes. Extracting
  # classes would turn that documented coupling into lock-ordering rules and
  # back-pointers rather than remove it. So hooks.h is untouched.
  src/segcap/hooks.cpp
  src/segcap/hooks_install.cpp
  src/segcap/hooks_detours.cpp
  src/segcap/hooks_election.cpp
  src/segcap/hooks_capture.cpp
  src/segcap/hooks_idprobe.cpp
  src/segcap/readback.cpp
  src/segcap/ue4.cpp
  src/segcap/ue4_processevent.cpp
  src/segcap/customdepth.cpp
  src/segcap/identity.cpp
  src/segcap/perception.cpp
)
target_link_libraries(segcap PRIVATE minhook d3d12 dxgi psapi)
set_target_properties(segcap PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ---------------------------------------------------------------------------
# tests -- run without the game, so the subtle logic is asserted rather than
# eyeballed. Identity corruption is invisible at runtime: it produces a mask
# that looks fine and mislabels an object.
# ---------------------------------------------------------------------------
add_executable(test_identity src/tests/test_identity.cpp src/segcap/identity.cpp)
set_target_properties(test_identity PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
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
