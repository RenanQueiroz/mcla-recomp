# FSR's static runtime, linked and run the way the PS5's title uses it, on a
# Windows PC (rexglue-temporal-aa.patch, REXGLUE_FSR_STATIC): FidelityFX built
# by the SDK's own wrapper (cmake/fidelityfx) as static libraries from the
# prebuilt shaders, with the SDK's Vulkan headers and no Vulkan library, and
# checked to have its backend (check_static.cmake); a program that links only
# those, the GPU plugin's file of the Vulkan functions FidelityFX calls directly
# (src/graphics/vulkan/temporal_aa_static.cpp) and its own code, with no Vulkan
# loader library (it opens vulkan-1.dll at run time, as the console's driver is
# reached through vk_icdGetInstanceProcAddr), with link-time optimisation and
# --undefined=ffxCreateContext as the title's link. The program then creates FSR
# with the plugin's flags on the GPU, dispatches it and checks its output, and does
# the same with frame generation (rexglue-frame-generation.patch: manual, FP16 code
# values, the plugin's flags).
#
# Needs the SDK patched and built once with FSR (the pinned FidelityFX checkout
# in its build tree), Ninja, and the LLVM in tools\ on PATH (clang, lld,
# llvm-rc, llvm-nm). From the repository, with the GPU free:
#   set PATH=%CD%\tools\clang+llvm-20.1.8-x86_64-pc-windows-msvc\bin;%PATH%
#   cmake -S launcher/dev/fsr/static_test -B <scratch>/fsr-static-test -G Ninja ^
#         -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_RC_COMPILER=llvm-rc
#   cmake --build <scratch>/fsr-static-test
#   <scratch>/fsr-static-test/fsr_static_test.exe
cmake_minimum_required(VERSION 3.23)
# Release, as FidelityFX is built (the C++ runtimes must match).
set(CMAKE_BUILD_TYPE Release CACHE STRING "")
project(fsr_static_test C CXX)

set(REXGLUE_SDK "${CMAKE_CURRENT_LIST_DIR}/../../../../third_party/rexglue-sdk" CACHE PATH
    "The patched ReXGlue SDK")
get_filename_component(REXGLUE_SDK "${REXGLUE_SDK}" ABSOLUTE)
set(FFX_ROOT "${REXGLUE_SDK}/out/build/win-amd64/_deps/ffx" CACHE PATH
    "The pinned FidelityFX checkout (cmake/rexglue_fsr.cmake fetches it)")
# What the PS5's runtime build adds for FidelityFX that a Windows clang takes
# (-mcmodel=large is not supported for this target).
set(FSR_TEST_FLAGS "-flto=thin" CACHE STRING "Code generation flags for FidelityFX and the program")
separate_arguments(_flags NATIVE_COMMAND "${FSR_TEST_FLAGS}")

include(ExternalProject)
set(_wrapper "${CMAKE_BINARY_DIR}/wrapper")
foreach(_file CMakeLists.txt soft_step.cmake check_static.cmake prebuilt_shaders_hook.cmake
              prebuilt_shaders.cmake vulkan-headers-only/FindVulkan.cmake
              ffx_provider_framegenerationswapchain_vk_stub.cpp)
    configure_file("${REXGLUE_SDK}/cmake/fidelityfx/${_file}" "${_wrapper}/${_file}" COPYONLY)
endforeach()
set(_archives_dir "${CMAKE_BINARY_DIR}/archives")
set(_archives)
foreach(_name amd_fidelityfx_vk ffx_backend_vk_x64 ffx_fsr3upscaler_x64 ffx_fsr2_x64
              ffx_frameinterpolation_x64 ffx_opticalflow_x64)
    list(APPEND _archives "${_archives_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}${_name}${CMAKE_STATIC_LIBRARY_SUFFIX}")
endforeach()
ExternalProject_Add(fsr_static_runtime
    SOURCE_DIR "${_wrapper}"
    BINARY_DIR "${CMAKE_BINARY_DIR}/ffx"
    CMAKE_GENERATOR Ninja
    CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release
        "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
        "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DCMAKE_RC_COMPILER=${CMAKE_RC_COMPILER}"
        "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DFFX_ROOT=${FFX_ROOT}" -DFFX_API_BACKEND=VK_X64
        -DREXGLUE_FSR_STATIC=ON "-DREXGLUE_FSR_ARCHIVE_DIR=${_archives_dir}"
        "-DREXGLUE_FSR_SHADERS_DIR=${REXGLUE_SDK}/thirdparty/fidelityfx-shaders-vk"
        -DREXGLUE_FSR_SHADERS_HASH=static-test
        "-DREXGLUE_FSR_VULKAN_HEADERS=${REXGLUE_SDK}/thirdparty/vulkan-headers/include"
        -DREXGLUE_FSR_VULKAN_LIBRARY=
        "-DCMAKE_C_FLAGS=${FSR_TEST_FLAGS}" "-DCMAKE_CXX_FLAGS=${FSR_TEST_FLAGS}"
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --target rexglue_fsr_static
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${_archives})

add_executable(fsr_static_test fsr_static_test.cpp
    "${REXGLUE_SDK}/src/graphics/vulkan/temporal_aa_static.cpp")
add_dependencies(fsr_static_test fsr_static_runtime)
set_target_properties(fsr_static_test PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
target_compile_definitions(fsr_static_test PRIVATE REX_HAS_FSR=1 REX_FSR_STATIC=1)
target_include_directories(fsr_static_test PRIVATE
    "${REXGLUE_SDK}/include"
    "${REXGLUE_SDK}/src/graphics"
    "${REXGLUE_SDK}/thirdparty/vulkan-headers/include"
    "${REXGLUE_SDK}/thirdparty/fidelityfx-api/include"
    "${REXGLUE_SDK}/thirdparty/renderdoc")
target_compile_options(fsr_static_test PRIVATE ${_flags})
# No vulkan-1.lib: the only definitions of the Vulkan functions FidelityFX calls
# are the plugin's (temporal_aa_static.cpp).
target_link_options(fsr_static_test PRIVATE ${_flags} -fuse-ld=lld "LINKER:/INCLUDE:ffxCreateContext")
target_link_libraries(fsr_static_test PRIVATE ${_archives})
