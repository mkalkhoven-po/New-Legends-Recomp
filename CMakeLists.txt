cmake_minimum_required(VERSION 3.20)
project(NewLegendsRecomp CXX RC)

# The generated code relies on clang builtins (__builtin_parity etc.):
# configure with clang-cl, see build.bat.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "Build with clang-cl (see build.bat)")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(GENERATED_DIR ${CMAKE_SOURCE_DIR}/generated)

if(NOT EXISTS ${GENERATED_DIR}/sources.cmake)
    message(FATAL_ERROR "Run `python -m recompiler generate game/default.xbe generated` first")
endif()
include(${GENERATED_DIR}/sources.cmake)

# Recompiled game code: huge goto-based functions; warnings are noise.
add_library(game_code STATIC ${GENERATED_SOURCES})
target_include_directories(game_code PUBLIC runtime/include ${GENERATED_DIR})
target_compile_options(game_code PRIVATE -w -O2 -fno-strict-aliasing /bigobj)

add_executable(newlegends
    runtime/src/main.cpp
    runtime/src/memory.cpp
    runtime/src/loader.cpp
    runtime/src/dispatch.cpp
    runtime/src/cpu.cpp
    runtime/src/kernel/kernel.cpp
    runtime/src/kernel/kthread.cpp
    runtime/src/kernel/kmem.cpp
    runtime/src/kernel/kfile.cpp
    runtime/src/kernel/kmisc.cpp
    runtime/src/kernel/krtl.cpp
    runtime/src/hle/unreal.cpp
    runtime/src/hle/input.cpp
    runtime/src/hle/dsound.cpp
    runtime/src/hle/audio_out.cpp
    runtime/src/crash.cpp
    runtime/src/settings.cpp
    runtime/src/gpu/nv2a.cpp
    runtime/src/gpu/pfifo.cpp
    runtime/src/gpu/pgraph.cpp
    runtime/src/gpu/video.cpp
    runtime/src/gpu/vk_backend.cpp
    runtime/src/gpu/texture.cpp
    runtime/src/gpu/shaders.cpp
    res/newlegends.rc
)
# Vulkan and the runtime GLSL compiler from the LunarG SDK.
set(VULKAN_SDK $ENV{VULKAN_SDK})
if(NOT VULKAN_SDK)
    message(FATAL_ERROR "VULKAN_SDK is not set (install the LunarG Vulkan SDK)")
endif()
target_include_directories(newlegends PRIVATE ${VULKAN_SDK}/Include)
target_link_libraries(newlegends PRIVATE game_code winmm xinput dbghelp onecore ole32 bcrypt
    ${VULKAN_SDK}/Lib/vulkan-1.lib ${VULKAN_SDK}/Lib/shaderc_combined.lib)
target_compile_options(newlegends PRIVATE -Wno-unused-parameter)
# Guest stacks are in guest memory, but recompiled calls nest on the host stack.
target_link_options(newlegends PRIVATE /STACK:67108864)
