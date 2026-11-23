cmake_minimum_required(VERSION 3.20)
project(albw-port C)

# The published version, here alone: the launcher shows it, WorldAcross.exe carries it in its version
# resource, and the release workflow checks the tag against it.
set(ALBW_VERSION 0.1.5)
string(REPLACE "." ";" ALBW_VERSION_PARTS "${ALBW_VERSION}")
list(GET ALBW_VERSION_PARTS 0 ALBW_VERSION_MAJOR)
list(GET ALBW_VERSION_PARTS 1 ALBW_VERSION_MINOR)
list(GET ALBW_VERSION_PARTS 2 ALBW_VERSION_PATCH)
add_compile_definitions(ALBW_VERSION="${ALBW_VERSION}")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Paths compiled into the programs -- assert messages, __FILE__ -- relative to the checkout and the
# build folder, not to this machine: a published program names no one's directories. (The build
# folder last, since it may lie inside the checkout and the last map that fits is the one used.)
add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}/=" "-ffile-prefix-map=${CMAKE_BINARY_DIR}/=")

# The generated translation unit is one huge file of straight-line code. -O1 keeps build
# times sane; the optimiser has little to work with at -O2 here anyway.
# The recompiler decides how many translation units to emit, so glob them. CONFIGURE_DEPENDS
# makes the build re-glob when that count changes.
file(GLOB ALBW_GENERATED CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/generated/*.c)
# The ARM11 rounds a multiply and the add after it separately (VMLA is not fused), so the compiler
# must not fuse them either -- which it would by default on a host with FMA, such as aarch64, or in
# a build for a newer x86 level.
set_source_files_properties(${ALBW_GENERATED} PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")

set(ALBW_RUNTIME_SOURCES
    src/runtime/albw_runtime.c
    src/runtime/albw_kernel.c
    src/runtime/albw_romfs.c
    src/runtime/albw_savedata.c
    src/runtime/albw_config.c
    src/runtime/albw_dsp.c
    src/runtime/albw_screen.c
    src/runtime/albw_pica.c
    src/runtime/albw_lighting.c
    src/runtime/albw_raster.c
    src/runtime/albw_shader.c
    src/runtime/albw_thread.c
    src/runtime/albw_platform.c
    src/runtime/albw_card.c
)

# Two programs play the game. albw-boot has the translation linked in: it is what the tools and the
# regression checks run, and it needs a translated card to build. world-across is the launcher
# that is published: it has none of the game, makes it on the player's machine from their own
# card the first time it starts (albw_builder.c), and loads it as a module (albw_game.h). It builds
# without a card. Everything else -- window, renderers, upscalers, Windows resources -- they share
# through albw_player.
add_library(albw_player INTERFACE)
add_executable(albw-boot ${ALBW_RUNTIME_SOURCES} ${ALBW_GENERATED})
add_executable(world-across ${ALBW_RUNTIME_SOURCES} src/runtime/albw_builder.c)
target_compile_definitions(world-across PRIVATE ALBW_GAME_MODULE)
target_link_libraries(world-across PRIVATE ${CMAKE_DL_LIBS})
# Further sources for the launcher alone: the AppImage's stand-ins for what older C libraries lack
# (tools/make_appimage.sh).
set(ALBW_LAUNCHER_EXTRA_SOURCES "" CACHE STRING "Further sources linked into world-across")
target_sources(world-across PRIVATE ${ALBW_LAUNCHER_EXTRA_SOURCES})
if(WIN32)
    configure_file(src/runtime/world_across.rc.in ${CMAKE_BINARY_DIR}/world_across.rc @ONLY)
    target_sources(world-across PRIVATE ${CMAKE_BINARY_DIR}/world_across.rc)
endif()
if(WIN32)
    set_target_properties(world-across PROPERTIES OUTPUT_NAME WorldAcross)
endif()
foreach(player albw-boot world-across)
    target_link_libraries(${player} PRIVATE albw_player)
endforeach()
target_include_directories(albw_player INTERFACE src/runtime src/generated)
target_compile_options(albw_player INTERFACE -O1 -g -fno-strict-aliasing -Wall
    -Wno-unused-but-set-variable -Wno-unused-variable)
target_link_libraries(albw_player INTERFACE m)

# Windows (MinGW-w64, natively in MSYS2 or across from Linux with cmake/mingw-w64-x86_64.cmake):
# Windows 10's API (WaitOnAddress), C99's printf, the C and C++ runtimes linked in so that the
# program needs no DLL but SDL's, and a manifest that makes paths UTF-8 as on Linux.
if(WIN32)
    enable_language(RC)
    target_compile_definitions(albw_player INTERFACE _WIN32_WINNT=0x0A00 __USE_MINGW_ANSI_STDIO=1)
    target_sources(albw_player INTERFACE src/windows/albw-boot.rc)
    target_link_libraries(albw_player INTERFACE synchronization)
    target_link_options(albw_player INTERFACE -static -static-libgcc -static-libstdc++)
    # stb is headers alone: a cross build takes the host's, copied where only they are found.
    if(CMAKE_CROSSCOMPILING AND EXISTS /usr/include/stb/stb_truetype.h)
        file(COPY /usr/include/stb/stb_truetype.h /usr/include/stb/stb_image.h
                  /usr/include/stb/stb_image_write.h
             DESTINATION ${CMAKE_BINARY_DIR}/host-include/stb)
        target_include_directories(albw_player SYSTEM INTERFACE ${CMAKE_BINARY_DIR}/host-include)
    endif()
    # SDL3's MinGW development files, unpacked into external/, unless SDL3_DIR says where.
    # (A failed search leaves SDL3_DIR-NOTFOUND in the cache, which a later unpacking replaces.)
    if(NOT SDL3_DIR)
        file(GLOB ALBW_SDL3_MINGW "${CMAKE_SOURCE_DIR}/external/SDL3-*/x86_64-w64-mingw32/lib/cmake/SDL3")
        if(ALBW_SDL3_MINGW)
            list(GET ALBW_SDL3_MINGW -1 ALBW_SDL3_MINGW)
            set(SDL3_DIR "${ALBW_SDL3_MINGW}" CACHE PATH "SDL3's CMake files for MinGW" FORCE)
        endif()
    endif()
endif()

# The renderer runs per pixel and the shader per vertex, so they are worth the optimiser the
# generated code is not; -O2 rather than anything tuned to this machine, so the build still runs on
# another one.
set_source_files_properties(
    src/runtime/albw_raster.c src/runtime/albw_lighting.c src/runtime/albw_shader.c
    src/runtime/albw_screen.c src/runtime/albw_pica.c src/runtime/albw_dsp.c
    PROPERTIES COMPILE_OPTIONS "-O2")

# The window, if SDL3 is installed. Without it the port still builds and runs, headless, which is how
# the tools use it anyway -- so a missing library costs the window and nothing else.
find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
    # The temporal upscalers' module is C++, as AMD's and NVIDIA's interfaces are; without either
    # SDK it is only the answer that neither is there.
    enable_language(CXX)
    target_sources(albw_player INTERFACE src/runtime/albw_window.c src/runtime/albw_launcher.c
                                     src/runtime/albw_upscale.c src/runtime/albw_vkup.cpp)
    set_source_files_properties(src/runtime/albw_upscale.c PROPERTIES COMPILE_OPTIONS "-O2")
    set_source_files_properties(src/runtime/albw_vkup.cpp PROPERTIES COMPILE_OPTIONS "-O2;-std=c++17")
    target_compile_definitions(albw_player INTERFACE ALBW_HAVE_WINDOW)
    target_link_libraries(albw_player INTERFACE SDL3::SDL3)

    # AMD FSR 1 (third_party/fsr1, MIT), as two fragment shaders for SDL's GPU renderer, compiled to
    # SPIR-V here and built into the binary. Without glslc the window simply offers no FSR.
    find_program(GLSLC glslc)
    if(GLSLC)
        set(ALBW_SHADER_OUT ${CMAKE_BINARY_DIR}/shaders)
        set(ALBW_SHADER_HEADERS)
        foreach(shader fsr_easu fsr_rcas)
            add_custom_command(
                OUTPUT ${ALBW_SHADER_OUT}/${shader}.h
                COMMAND ${CMAKE_COMMAND} -E make_directory ${ALBW_SHADER_OUT}
                COMMAND ${GLSLC} -fshader-stage=frag -O -I ${CMAKE_SOURCE_DIR}/third_party/fsr1
                        ${CMAKE_SOURCE_DIR}/src/shaders/${shader}.frag -o ${ALBW_SHADER_OUT}/${shader}.spv
                COMMAND ${CMAKE_COMMAND} -DIN=${ALBW_SHADER_OUT}/${shader}.spv
                        -DOUT=${ALBW_SHADER_OUT}/${shader}.h -DNAME=${shader}_spv
                        -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
                DEPENDS ${CMAKE_SOURCE_DIR}/src/shaders/${shader}.frag
                        ${CMAKE_SOURCE_DIR}/third_party/fsr1/ffx_a.h
                        ${CMAKE_SOURCE_DIR}/third_party/fsr1/ffx_fsr1.h
                        ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
                VERBATIM)
            list(APPEND ALBW_SHADER_HEADERS ${ALBW_SHADER_OUT}/${shader}.h)
        endforeach()
        # The GPU renderer (src/runtime/albw_gpu.c): the recorded frames drawn on the card, at a
        # multiple of the console's resolution. Its shaders share pica_state.h with the C side.
        set(ALBW_GPU_SHADERS pica.vert pica.frag blit.vert blit.frag)
        set(ALBW_GPU_HEADERS)
        foreach(shader ${ALBW_GPU_SHADERS})
            string(REPLACE "." "_" name ${shader})
            add_custom_command(
                OUTPUT ${ALBW_SHADER_OUT}/${name}.h
                COMMAND ${CMAKE_COMMAND} -E make_directory ${ALBW_SHADER_OUT}
                COMMAND ${GLSLC} -O -I ${CMAKE_SOURCE_DIR}/src/shaders
                        ${CMAKE_SOURCE_DIR}/src/shaders/${shader} -o ${ALBW_SHADER_OUT}/${name}.spv
                COMMAND ${CMAKE_COMMAND} -DIN=${ALBW_SHADER_OUT}/${name}.spv
                        -DOUT=${ALBW_SHADER_OUT}/${name}.h -DNAME=${name}_spv
                        -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
                DEPENDS ${CMAKE_SOURCE_DIR}/src/shaders/${shader}
                        ${CMAKE_SOURCE_DIR}/src/shaders/pica_state.h
                        ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
                VERBATIM)
            list(APPEND ALBW_GPU_HEADERS ${ALBW_SHADER_OUT}/${name}.h)
        endforeach()
        target_sources(albw_player INTERFACE ${ALBW_SHADER_HEADERS} ${ALBW_GPU_HEADERS}
                                         src/runtime/albw_gpu.c)
        set_source_files_properties(src/runtime/albw_gpu.c PROPERTIES COMPILE_OPTIONS "-O2")
        target_include_directories(albw_player INTERFACE ${ALBW_SHADER_OUT})
        target_compile_definitions(albw_player INTERFACE ALBW_HAVE_FSR ALBW_HAVE_GPU)
        message(STATUS "glslc found: the window offers AMD FSR 1 and the GPU renderer")
    else()
        message(STATUS "glslc not found: no FSR")
    endif()

    # AMD FSR 3 (FidelityFX SDK 1.1.4, MIT), built from the SDK's own sources when its sdk/ folder
    # has been put in external/ -- it is not part of the repository -- and glslc, Vulkan and Python
    # are there. Its shader permutations are made by tools/ffx_permutations.py, in place of AMD's
    # Windows-only shader compiler.
    set(ALBW_FFX_SDK "${CMAKE_SOURCE_DIR}/external/fidelityfx-sdk-v1.1.4/sdk"
        CACHE PATH "The sdk/ folder of AMD's FidelityFX SDK 1.1.4")
    if(NOT WIN32)
        find_package(Vulkan QUIET)
    endif()
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(GLSLC AND Vulkan_FOUND AND Python3_FOUND
            AND EXISTS "${ALBW_FFX_SDK}/include/FidelityFX/host/ffx_fsr3upscaler.h")
        enable_language(CXX)
        set(FFX_GEN ${CMAKE_BINARY_DIR}/ffx-gen)
        # The SDK sizes its contexts for Windows' two-byte wchar_t; with Linux' four-byte one, FSR
        # 3's private context outgrows the public one. A copy of ffx_types.h with room to spare
        # goes in front of the SDK's.
        file(READ "${ALBW_FFX_SDK}/include/FidelityFX/host/ffx_types.h" FFX_TYPES)
        string(REPLACE "#define FFX_SDK_DEFAULT_CONTEXT_SIZE (1024 * 128)"
                       "#define FFX_SDK_DEFAULT_CONTEXT_SIZE (1024 * 256)" FFX_TYPES_WIDE "${FFX_TYPES}")
        if(FFX_TYPES_WIDE STREQUAL FFX_TYPES)
            message(FATAL_ERROR "ffx_types.h has no FFX_SDK_DEFAULT_CONTEXT_SIZE to widen")
        endif()
        file(GENERATE OUTPUT ${FFX_GEN}/include/FidelityFX/host/ffx_types.h CONTENT "${FFX_TYPES_WIDE}")
        # The Vulkan backend lays its tables out one after another in the scratch memory it is
        # given, rounded to four bytes; GCC, optimising, stores into them with instructions that
        # need sixteen, and the backend crashed where MSVC's would not. A copy rounding to 64 is
        # what is built.
        file(READ "${ALBW_FFX_SDK}/src/backends/vk/ffx_vk.cpp" FFX_VK)
        string(REPLACE ", sizeof(uint32_t));" ", 64);" FFX_VK_ALIGNED "${FFX_VK}")
        # Twelve of them, each fourteen characters shorter: six sizes, worked out twice.
        string(LENGTH "${FFX_VK}" FFX_VK_LENGTH)
        string(LENGTH "${FFX_VK_ALIGNED}" FFX_VK_ALIGNED_LENGTH)
        math(EXPR FFX_VK_HIT_COUNT "(${FFX_VK_LENGTH} - ${FFX_VK_ALIGNED_LENGTH}) / 14")
        if(NOT FFX_VK_HIT_COUNT EQUAL 12)
            message(FATAL_ERROR "ffx_vk.cpp: expected 12 table sizes to align, found ${FFX_VK_HIT_COUNT}")
        endif()
        # And the first of them starts on a 64-byte boundary too: the effect contexts are
        # alignas(32), and the structure in front of them is not a multiple of that.
        string(REPLACE "uint8_t* pMem = (uint8_t*)((BackendContext_VK*)(backendContext + 1));"
                       "uint8_t* pMem = (uint8_t*)FFX_ALIGN_UP((uintptr_t)(backendContext + 1), 64);"
                       FFX_VK_ALIGNED "${FFX_VK_ALIGNED}")
        string(REPLACE "return FFX_ALIGN_UP(sizeof(BackendContext_VK) +"
                       "return 64 + FFX_ALIGN_UP(sizeof(BackendContext_VK) +"
                       FFX_VK_ALIGNED "${FFX_VK_ALIGNED}")
        string(FIND "${FFX_VK_ALIGNED}" "FFX_ALIGN_UP((uintptr_t)(backendContext + 1), 64)" FFX_VK_START)
        string(FIND "${FFX_VK_ALIGNED}" "return 64 + FFX_ALIGN_UP(sizeof(BackendContext_VK)" FFX_VK_TOTAL)
        if(FFX_VK_START EQUAL -1 OR FFX_VK_TOTAL EQUAL -1)
            message(FATAL_ERROR "ffx_vk.cpp: the scratch layout is not where it was expected")
        endif()
        file(GENERATE OUTPUT ${FFX_GEN}/src/ffx_vk.cpp CONTENT "${FFX_VK_ALIGNED}")

        set(FFX_PASSES autogen_reactive accumulate luma_pyramid prepare_reactivity prepare_inputs
                       shading_change rcas shading_change_pyramid luma_instability debug_view)
        set(FFX_PERMUTATION_HEADERS)
        foreach(pass ${FFX_PASSES})
            list(APPEND FFX_PERMUTATION_HEADERS ${FFX_GEN}/ffx_fsr3upscaler_${pass}_pass_permutations.h)
        endforeach()
        file(GLOB FFX_FSR3_SHADER_SOURCES
             ${ALBW_FFX_SDK}/src/backends/vk/shaders/fsr3upscaler/*.glsl
             ${ALBW_FFX_SDK}/include/FidelityFX/gpu/fsr3upscaler/*.h
             ${ALBW_FFX_SDK}/include/FidelityFX/gpu/*.h)
        add_custom_command(
            OUTPUT ${FFX_PERMUTATION_HEADERS}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ffx_permutations.py
                    --sdk ${ALBW_FFX_SDK} --out ${FFX_GEN} --component fsr3upscaler --glslc ${GLSLC}
            DEPENDS ${CMAKE_SOURCE_DIR}/tools/ffx_permutations.py ${FFX_FSR3_SHADER_SOURCES}
            VERBATIM)

        add_library(albw_ffx STATIC
            ${ALBW_FFX_SDK}/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp
            ${FFX_GEN}/src/ffx_vk.cpp
            ${ALBW_FFX_SDK}/src/backends/shared/ffx_shader_blobs.cpp
            ${ALBW_FFX_SDK}/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp
            ${ALBW_FFX_SDK}/src/shared/ffx_assert.cpp
            ${ALBW_FFX_SDK}/src/shared/ffx_message.cpp
            ${ALBW_FFX_SDK}/src/shared/ffx_object_management.cpp
            ${ALBW_FFX_SDK}/src/shared/ffx_breadcrumbs_list.cpp
            ${FFX_PERMUTATION_HEADERS})
        set_target_properties(albw_ffx PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
        target_include_directories(albw_ffx BEFORE PUBLIC ${FFX_GEN}/include)
        target_include_directories(albw_ffx PUBLIC ${ALBW_FFX_SDK}/include
            PRIVATE ${ALBW_FFX_SDK}/src/backends/shared ${ALBW_FFX_SDK}/src/components
                    ${ALBW_FFX_SDK}/src/shared ${ALBW_FFX_SDK}/src ${FFX_GEN})
        target_compile_definitions(albw_ffx PUBLIC FFX_GCC PRIVATE FFX_FSR3UPSCALER)
        target_compile_options(albw_ffx PRIVATE
            -include ${CMAKE_SOURCE_DIR}/src/ffx/ffx_port_compat.h -O2 -w)
        target_link_libraries(albw_ffx PUBLIC Vulkan::Vulkan)

        target_link_libraries(albw_player INTERFACE albw_ffx)
        target_compile_definitions(albw_player INTERFACE ALBW_HAVE_FSR3)
        message(STATUS "FidelityFX SDK found: the window offers AMD FSR 3")
    else()
        message(STATUS "FidelityFX SDK 1.1.4 not in external/ (or no Vulkan, glslc or Python): no FSR 3")
    endif()

    # NVIDIA DLSS, through NGX, when NVIDIA's DLSS SDK (under NVIDIA's licence, which whoever puts
    # it there accepts) is in external/. Only its headers and libnvsdk_ngx.a are linked; the DLSS
    # library itself is found at run time, beside the program or in the SDK.
    set(ALBW_DLSS_SDK "${CMAKE_SOURCE_DIR}/external/dlss-sdk-v310.9.16"
        CACHE PATH "NVIDIA's DLSS SDK (include/ and lib/Linux_x86_64/)")
    if(Vulkan_FOUND AND EXISTS "${ALBW_DLSS_SDK}/include/nvsdk_ngx_vk.h"
            AND EXISTS "${ALBW_DLSS_SDK}/lib/Linux_x86_64/libnvsdk_ngx.a")
        target_include_directories(albw_player INTERFACE ${ALBW_DLSS_SDK}/include)
        target_link_libraries(albw_player INTERFACE ${ALBW_DLSS_SDK}/lib/Linux_x86_64/libnvsdk_ngx.a
                                                ${CMAKE_DL_LIBS} Vulkan::Vulkan)
        target_compile_definitions(albw_player INTERFACE ALBW_HAVE_DLSS
            ALBW_DLSS_LIBRARY_DIR="${ALBW_DLSS_SDK}/lib/Linux_x86_64/rel")
        message(STATUS "DLSS SDK found: the window offers NVIDIA DLSS on NVIDIA's RTX cards")
    else()
        message(STATUS "DLSS SDK not in external/: no DLSS")
    endif()
    if(WIN32)
        # A program with a window, not a console's: started from a terminal it still writes there,
        # and otherwise into last-run.log beside its settings (albw_platform_init).
        target_link_options(albw_player INTERFACE -mwindows)
        foreach(player albw-boot world-across)
            add_custom_command(TARGET ${player} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3>
                        $<TARGET_FILE_DIR:${player}>
                COMMENT "SDL3.dll beside ${player}")
        endforeach()
    endif()
    message(STATUS "SDL3 found: albw-boot opens a window")
else()
    message(STATUS "SDL3 not found: albw-boot is headless")
endif()

# The diagnostic builds and the kernel library for the Unicorn reference run are Linux tools.
if(NOT WIN32)
# Same sources with per-instruction tracing, for diffing against the Unicorn reference.
add_executable(albw-boot-trace
    src/runtime/albw_runtime.c
    src/runtime/albw_kernel.c
    src/runtime/albw_romfs.c
    src/runtime/albw_savedata.c
    src/runtime/albw_config.c
    src/runtime/albw_dsp.c
    src/runtime/albw_screen.c
    src/runtime/albw_pica.c
    src/runtime/albw_lighting.c
    src/runtime/albw_raster.c
    src/runtime/albw_shader.c
    src/runtime/albw_thread.c
    src/runtime/albw_platform.c
    src/runtime/albw_card.c
    ${ALBW_GENERATED}
)
target_include_directories(albw-boot-trace PRIVATE src/runtime src/generated)
target_compile_definitions(albw-boot-trace PRIVATE ALBW_TRACE)
target_compile_options(albw-boot-trace PRIVATE -O1 -g -fno-strict-aliasing -Wall
    -Wno-unused-but-set-variable -Wno-unused-variable)
target_link_libraries(albw-boot-trace PRIVATE m)

# Same sources recording where the guest writes, to find the buffers the GPU reads. A call per
# store, so this is a diagnostic build and not the one to run.
add_executable(albw-boot-writes
    src/runtime/albw_runtime.c
    src/runtime/albw_kernel.c
    src/runtime/albw_romfs.c
    src/runtime/albw_savedata.c
    src/runtime/albw_config.c
    src/runtime/albw_dsp.c
    src/runtime/albw_screen.c
    src/runtime/albw_pica.c
    src/runtime/albw_lighting.c
    src/runtime/albw_raster.c
    src/runtime/albw_shader.c
    src/runtime/albw_thread.c
    src/runtime/albw_platform.c
    src/runtime/albw_card.c
    ${ALBW_GENERATED}
)
target_include_directories(albw-boot-writes PRIVATE src/runtime src/generated)
target_compile_definitions(albw-boot-writes PRIVATE ALBW_TRACK_WRITES)
target_compile_options(albw-boot-writes PRIVATE -O1 -g -fno-strict-aliasing -Wall
    -Wno-unused-but-set-variable -Wno-unused-variable)
target_link_libraries(albw-boot-writes PRIVATE m)

# The same again, watching for the first store of a not-a-number. -rdynamic so that the backtrace
# can name the generated functions, which are named after their guest addresses.
add_executable(albw-boot-nan
    src/runtime/albw_runtime.c
    src/runtime/albw_kernel.c
    src/runtime/albw_romfs.c
    src/runtime/albw_savedata.c
    src/runtime/albw_config.c
    src/runtime/albw_dsp.c
    src/runtime/albw_screen.c
    src/runtime/albw_pica.c
    src/runtime/albw_lighting.c
    src/runtime/albw_raster.c
    src/runtime/albw_shader.c
    src/runtime/albw_thread.c
    src/runtime/albw_platform.c
    src/runtime/albw_card.c
    ${ALBW_GENERATED}
)
target_include_directories(albw-boot-nan PRIVATE src/runtime src/generated)
target_compile_definitions(albw-boot-nan PRIVATE ALBW_WATCH_NAN)
target_compile_options(albw-boot-nan PRIVATE -O1 -g -fno-strict-aliasing -Wall
    -Wno-unused-but-set-variable -Wno-unused-variable)
target_link_libraries(albw-boot-nan PRIVATE m)
target_link_options(albw-boot-nan PRIVATE -rdynamic)


# The kernel as a shared library, so tools/oracle.py runs the same Horizon implementation
# the port does instead of a second one that could drift from it.
add_library(albw-kernel SHARED
    src/runtime/albw_kernel.c
    src/runtime/albw_romfs.c
    src/runtime/albw_savedata.c
    src/runtime/albw_config.c
    src/runtime/albw_dsp.c
    src/runtime/albw_screen.c
    src/runtime/albw_pica.c
    src/runtime/albw_lighting.c
    src/runtime/albw_raster.c
    src/runtime/albw_shader.c
    src/runtime/albw_thread.c
    src/runtime/albw_platform.c
    src/runtime/albw_card.c
    src/runtime/albw_kernel_shim.c
)
target_include_directories(albw-kernel PRIVATE src/runtime)
target_compile_options(albw-kernel PRIVATE -O1 -g -fno-strict-aliasing -Wall)
# Shared libraries link with undefined symbols by default, so a symbol the kernel gains but
# the shim does not provide only fails when Python dlopens it - twice now, after a run had
# already been paid for. Make it a link error instead.
target_link_options(albw-kernel PRIVATE -Wl,--no-undefined)
target_link_libraries(albw-kernel PRIVATE m)

endif()

# The rasteriser draws with a pool of threads.
find_package(Threads REQUIRED)
foreach(target albw-boot world-across albw-boot-trace albw-boot-writes albw-boot-nan albw-kernel)
    if(TARGET ${target})
        target_link_libraries(${target} PRIVATE Threads::Threads)
    endif()
endforeach()
