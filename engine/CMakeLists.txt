cmake_minimum_required(VERSION 3.21)
project(ChaosRings LANGUAGES C CXX)
if(WIN32)
    enable_language(RC) # (the .exe's icon: cmake/app-icon.cmake)
endif()
if(APPLE)
    enable_language(OBJCXX) # (MetalFX's upscaler: gfx/vk_metalfx.mm)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# SDL3: use the system package when present (Linux, Homebrew, vcpkg), otherwise
# fetch and build it from source (Windows/macOS without a package manager, Android).
find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
    include(FetchContent)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.2.0  # first stable SDL3; has every API we use. Safe to bump.
        GIT_SHALLOW TRUE)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(SDL3)
endif()

# Core library: everything that is not the app entry point, so tools can reuse it.
add_library(crcore STATIC
    src/core/lzw.cpp
    src/core/file.cpp
    src/core/text.cpp
    src/assets/texture.cpp
    src/assets/model.cpp
    src/assets/animation.cpp
    src/assets/collision.cpp
    src/assets/event.cpp
    src/game/stage_tables.cpp
    src/game/field_walker.cpp
    src/game/event_vm.cpp
    src/game/treasure.cpp
    src/game/encounter.cpp
    src/game/save.cpp
    src/game/shop.cpp
    src/game/party.cpp
    src/game/battle_rules.cpp
    src/game/genes.cpp
    src/game/puzzle.cpp
    src/scene/pose.cpp
    src/scene/smooth.cpp
    src/audio/sound.cpp
    src/audio/music.cpp
    src/audio/at9.cpp
    src/audio/scream.cpp
    src/assets/psarc.cpp
    src/assets/obb.cpp
    src/assets/gxt.cpp
    src/assets/battle_data.cpp
    src/assets/hd_texture.cpp
    src/assets/gltf_model.cpp
)
target_include_directories(crcore PUBLIC src)
target_include_directories(crcore PRIVATE third_party/hevag) # the HE-VAG filter table (vgmstream's, ISC)
target_link_libraries(crcore PUBLIC SDL3::SDL3)
# Music: AAC decoding through FAAD2 - the system's when pkg-config finds it, else the copy in
# third_party/faad2 (FAAD2 2.11.1's libfaad, built here: macOS, Windows). CR_BUNDLED_FAAD2=ON forces
# the bundled one.
option(CR_BUNDLED_FAAD2 "Build the bundled FAAD2 even if the system has one" OFF)
if(NOT CR_BUNDLED_FAAD2)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FAAD2 QUIET IMPORTED_TARGET faad2)
    endif()
endif()
if(FAAD2_FOUND)
    target_link_libraries(crcore PUBLIC PkgConfig::FAAD2)
    target_compile_definitions(crcore PUBLIC CR_HAVE_FAAD2=1)
    message(STATUS "Music: FAAD2 ${FAAD2_VERSION} (system)")
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/faad2/libfaad/decoder.c)
    enable_language(C)
    file(GLOB CR_FAAD_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/faad2/libfaad/*.c)
    add_library(cr_faad2 STATIC ${CR_FAAD_SOURCES})
    target_include_directories(cr_faad2 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/faad2/include
                               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/faad2/libfaad)
    # FAAD2's own CMakeLists' fixed configuration (floating point, DRC, no DRM)
    target_compile_definitions(cr_faad2 PRIVATE APPLY_DRC HAVE_INTTYPES_H=1 HAVE_MEMCPY=1 HAVE_STRING_H=1
                               STDC_HEADERS=1 PACKAGE_VERSION="2.11.1")
    if(NOT MSVC)
        target_compile_definitions(cr_faad2 PRIVATE HAVE_STRINGS_H=1 HAVE_SYS_STAT_H=1 HAVE_SYS_TYPES_H=1)
    endif()
    include(CheckSymbolExists)
    if(NOT MSVC)
        set(CMAKE_REQUIRED_LIBRARIES m)
    endif()
    check_symbol_exists(lrintf "math.h" CR_HAVE_LRINTF)
    unset(CMAKE_REQUIRED_LIBRARIES)
    if(CR_HAVE_LRINTF)
        target_compile_definitions(cr_faad2 PRIVATE HAVE_LRINTF=1)
    endif()
    if(MSVC)
        target_compile_options(cr_faad2 PRIVATE /w)
    else()
        target_compile_options(cr_faad2 PRIVATE -w)
        find_library(CR_MATH_LIBRARY m)
        if(CR_MATH_LIBRARY)
            target_link_libraries(cr_faad2 PUBLIC ${CR_MATH_LIBRARY})
        endif()
    endif()
    set_target_properties(cr_faad2 PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(crcore PUBLIC cr_faad2)
    target_compile_definitions(crcore PUBLIC CR_HAVE_FAAD2=1)
    message(STATUS "Music: FAAD2 2.11.1 (bundled)")
else()
    message(STATUS "Music: FAAD2 not found, music disabled")
endif()

# The Vita's voices and music (.at9): LibAtrac9's C decoder, always built from third_party/libatrac9
# (MIT, see its README)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libatrac9/src/libatrac9.c)
    enable_language(C)
    file(GLOB CR_ATRAC9_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libatrac9/src/*.c)
    add_library(cr_atrac9 STATIC ${CR_ATRAC9_SOURCES})
    target_include_directories(cr_atrac9 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libatrac9/src)
    # (its header marks the functions dllimport under MSVC unless COMPILING_DLL: a static library here)
    target_compile_definitions(cr_atrac9 PUBLIC COMPILING_DLL)
    if(MSVC)
        target_compile_options(cr_atrac9 PRIVATE /w)
    else()
        target_compile_options(cr_atrac9 PRIVATE -w)
        find_library(CR_ATRAC9_MATH m)
        if(CR_ATRAC9_MATH)
            target_link_libraries(cr_atrac9 PUBLIC ${CR_ATRAC9_MATH})
        endif()
    endif()
    set_target_properties(cr_atrac9 PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(crcore PUBLIC cr_atrac9)
    target_compile_definitions(crcore PUBLIC CR_HAVE_ATRAC9=1)
    message(STATUS "Vita audio: LibAtrac9 (bundled)")
endif()

# Vita data (cr1.psarc): zlib when available
# zlib and the like: the system's, else (Windows, macOS, Android - CR_FETCH_DEPS) built here from
# their sources, static
option(CR_FETCH_DEPS "Build zlib (and FreeType) from source where the system has none" ON)
find_package(ZLIB QUIET)
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
    set(CR_FETCH_EXCLUDE EXCLUDE_FROM_ALL)
endif()
if(NOT ZLIB_FOUND AND CR_FETCH_DEPS)
    include(FetchContent)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5) # (zlib's asks for CMake 2.4, which CMake 4 refuses)
    FetchContent_Declare(zlib GIT_REPOSITORY https://github.com/madler/zlib.git GIT_TAG v1.3.1 GIT_SHALLOW TRUE ${CR_FETCH_EXCLUDE})
    # The excluded dependency's example is not built or part of the port's tests.
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "Build zlib example programs" FORCE)
    FetchContent_MakeAvailable(zlib)
    set_target_properties(zlibstatic PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}")
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
    set(ZLIB_FOUND TRUE)
    set(ZLIB_LIBRARY zlibstatic) # (the find module's own names too: FreeType's build reads them)
    set(ZLIB_LIBRARIES zlibstatic)
    set(ZLIB_INCLUDE_DIR "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}")
    set(ZLIB_INCLUDE_DIRS "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}")
    set(ZLIB_VERSION_STRING "1.3.1 (built here)")
endif()
if(NOT ZLIB_FOUND)
    message(FATAL_ERROR "The Vita save importer requires zlib. Install zlib or enable CR_FETCH_DEPS.")
endif()
if(ZLIB_FOUND)
    target_link_libraries(crcore PUBLIC ZLIB::ZLIB)
    target_compile_definitions(crcore PUBLIC CR_HAVE_ZLIB=1)
    message(STATUS "Vita data: zlib ${ZLIB_VERSION_STRING}")
else()
    message(STATUS "Vita data: zlib not found, --vita disabled")
endif()

if(MSVC)
    target_compile_options(crcore PUBLIC /W4)
else()
    target_compile_options(crcore PUBLIC -Wall -Wextra)
endif()

set(GAME_SOURCES
    src/main.cpp
    src/platform/paths.cpp
    src/platform/setup.cpp
    src/gfx/gl.cpp
    src/gfx/renderer.cpp
    src/gfx/gl_device.cpp
    src/gfx/vk_device.cpp
    src/gfx/hud.cpp
    src/gfx/font.cpp
    src/gfx/menu_painter.cpp
)

# stb_truetype (public domain, vendored): dialogue in every language with a system font
set(THIRD_PARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
# stb_vorbis (public domain, vendored): the Android release's music (Ogg Vorbis)
target_sources(crcore PRIVATE ${THIRD_PARTY_DIR}/stb/stb_vorbis.c)
target_include_directories(crcore SYSTEM PRIVATE ${THIRD_PARTY_DIR})
set_source_files_properties(${THIRD_PARTY_DIR}/stb/stb_vorbis.c PROPERTIES COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/w,-w>")
# cgltf 1.13 (MIT), stb_image 2.30 and stb_image_write 1.16 (public domain / MIT), vendored: the
# model bridge's glTF reading, its pictures' decoding and the exports' PNGs (assets/gltf_model.cpp)
target_sources(crcore PRIVATE ${THIRD_PARTY_DIR}/gltf_libs.c)
set_source_files_properties(${THIRD_PARTY_DIR}/gltf_libs.c PROPERTIES COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/w,-w>")

# Vulkan back end: the loader + headers and glslc (shaders are compiled to SPIR-V at build time
# and embedded). Without them the game builds with OpenGL only.
find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND AND MINGW AND CMAKE_CROSSCOMPILING)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/mingw-vulkan.cmake) # (llvm-mingw: no Vulkan SDK)
endif()
find_program(GLSLC glslc HINTS ${Vulkan_GLSLC_EXECUTABLE})
set(CR_VULKAN OFF)
if(Vulkan_FOUND AND GLSLC AND NOT ANDROID) # (Android: GLES 3)
    set(CR_VULKAN ON)
    set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/shaders)
    set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
    file(MAKE_DIRECTORY ${SHADER_OUT})
    set(SHADER_INCS)
    foreach(s quad.vert quad.frag mesh.vert mesh.frag)
        add_custom_command(OUTPUT ${SHADER_OUT}/${s}.inc
            COMMAND ${GLSLC} -O -mfmt=c -I ${SHADER_DIR} ${SHADER_DIR}/${s} -o ${SHADER_OUT}/${s}.inc
            DEPENDS ${SHADER_DIR}/${s} ${SHADER_DIR}/common.glsl
            COMMENT "glslc ${s}")
        list(APPEND SHADER_INCS ${SHADER_OUT}/${s}.inc)
    endforeach()
    # Scene variants (temporal upscaling): + motion vectors and a reactive mask
    foreach(s quad.vert quad.frag mesh.vert mesh.frag)
        add_custom_command(OUTPUT ${SHADER_OUT}/scene_${s}.inc
            COMMAND ${GLSLC} -O -mfmt=c -DSCENE=1 -I ${SHADER_DIR} ${SHADER_DIR}/${s} -o ${SHADER_OUT}/scene_${s}.inc
            DEPENDS ${SHADER_DIR}/${s} ${SHADER_DIR}/common.glsl
            COMMENT "glslc ${s} (scene)")
        list(APPEND SHADER_INCS ${SHADER_OUT}/scene_${s}.inc)
    endforeach()
    # AMD FidelityFX FSR 3.1 upscaler passes (third_party/fidelityfx, MIT), as AMD's Vulkan
    # back end builds them: 32-bit maths, Lanczos approximation, render-size motion vectors
    set(FFX_DIR ${THIRD_PARTY_DIR}/fidelityfx)
    file(GLOB FFX_HEADERS ${FFX_DIR}/gpu/*.h ${FFX_DIR}/gpu/*/*.h)
    set(FFX_DEFS -DFFX_GPU=1 -DFFX_GLSL=1 -DFFX_HALF=0
        -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0
        -DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0
        -DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=0
        -DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0
        -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2
        -DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE=1
        -DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT=0
        -DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS=1
        -DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS=0
        -DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH=0)
    foreach(p prepare_inputs luma_pyramid shading_change_pyramid shading_change prepare_reactivity
              luma_instability accumulate accumulate_sharpen rcas)
        set(src ${p})
        set(sharpen 0)
        if(p STREQUAL "accumulate_sharpen")
            set(src accumulate)
            set(sharpen 1)
        endif()
        add_custom_command(OUTPUT ${SHADER_OUT}/fsr3_${p}.inc
            COMMAND ${GLSLC} -fshader-stage=comp --target-env=vulkan1.2 -O -mfmt=c ${FFX_DEFS}
                    -DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING=${sharpen}
                    -I ${FFX_DIR}/gpu -I ${FFX_DIR}/gpu/fsr3upscaler
                    ${FFX_DIR}/passes/ffx_fsr3upscaler_${src}_pass.glsl -o ${SHADER_OUT}/fsr3_${p}.inc
            DEPENDS ${FFX_DIR}/passes/ffx_fsr3upscaler_${src}_pass.glsl ${FFX_HEADERS}
            COMMENT "glslc FSR 3.1 ${p}")
        list(APPEND SHADER_INCS ${SHADER_OUT}/fsr3_${p}.inc)
    endforeach()
    add_custom_target(cr_shaders DEPENDS ${SHADER_INCS})
    list(APPEND GAME_SOURCES src/gfx/vk_fsr3.cpp src/gfx/vk_dlss.cpp)
    if(APPLE) # (MetalFX's upscaler: Objective-C++)
        list(APPEND GAME_SOURCES src/gfx/vk_metalfx.mm)
        set_source_files_properties(src/gfx/vk_metalfx.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/apple-vulkan.cmake)
    endif()
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/dlss.cmake) # (NVIDIA DLSS: CR_DLSS, cr_link_dlss)
    # VkXxx info{sType} value-initialises the other members, as intended
    if(NOT MSVC)
        set_source_files_properties(src/gfx/vk_device.cpp src/gfx/vk_fsr3.cpp src/gfx/vk_dlss.cpp
                                    PROPERTIES COMPILE_OPTIONS -Wno-missing-field-initializers)
    endif()
    message(STATUS "Renderer: Vulkan ${Vulkan_VERSION} and OpenGL")
else()
    message(STATUS "Renderer: OpenGL only (Vulkan loader or glslc not found)")
endif()

if(ANDROID)
    # SDL's Android Java glue loads libmain.so
    add_library(main SHARED ${GAME_SOURCES})
    set(GAME_TARGET main)
    target_link_libraries(main PRIVATE crcore GLESv3 log android)
    # (CR_RELEASE always: Gradle builds a release APK as RelWithDebInfo, and a phone has no F3 to
    # take the keyboard's key help off the screen)
    target_compile_definitions(main PRIVATE CR_GLES=1 CR_RELEASE=1)
    target_include_directories(main SYSTEM PRIVATE ${THIRD_PARTY_DIR})
else()
    add_executable(chaosrings WIN32 MACOSX_BUNDLE ${GAME_SOURCES})
    set(GAME_TARGET chaosrings)
    target_link_libraries(chaosrings PRIVATE crcore)
    # The first start's setup (platform/setup.cpp), on a desktop: a Vita NoNpDrm dump's files decrypted
    # (third_party/psvpfsparser - Vita3K's, GPL-2.0) and a dump's archive - .rar, .7z, .zip, .vpk -
    # unpacked (libarchive: the system's, else built here - CR_FETCH_DEPS; without it, a folder only)
    if(NOT IOS)
        file(GLOB CR_PSVPFS_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/psvpfsparser/*.cpp)
        add_library(psvpfs STATIC ${CR_PSVPFS_SOURCES})
        target_include_directories(psvpfs SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/psvpfsparser)
        target_compile_features(psvpfs PUBLIC cxx_std_17)
        if(NOT MSVC)
            target_compile_options(psvpfs PRIVATE -w)
        endif()
        target_link_libraries(chaosrings PRIVATE psvpfs)
        target_compile_definitions(chaosrings PRIVATE CR_HAVE_PSVPFS=1)
        find_package(LibArchive QUIET)
        if(NOT LibArchive_FOUND AND CR_FETCH_DEPS)
            include(FetchContent)
            set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
            foreach(off ENABLE_OPENSSL ENABLE_MBEDTLS ENABLE_NETTLE ENABLE_LIBB2 ENABLE_LZ4 ENABLE_LZO ENABLE_ZSTD ENABLE_BZip2 ENABLE_LIBXML2
                        ENABLE_EXPAT ENABLE_PCREPOSIX ENABLE_PCRE2POSIX ENABLE_LIBGCC ENABLE_CNG ENABLE_TAR ENABLE_CPIO ENABLE_CAT ENABLE_UNZIP
                        ENABLE_XATTR ENABLE_ACL ENABLE_ICONV ENABLE_TEST ENABLE_INSTALL ENABLE_WERROR)
                set(${off} OFF CACHE BOOL "" FORCE)
            endforeach()
            set(POSIX_REGEX_LIB NONE CACHE STRING "" FORCE) # (no regular expressions: only its readers are used)
            if(MSVC) # (its checks miss the C runtime's wide-string functions under MSVC, then it defines its own: C2169)
                foreach(have HAVE_WCSCPY HAVE_WCSLEN HAVE_WMEMCMP HAVE_WMEMCPY HAVE_WMEMMOVE)
                    set(${have} 1 CACHE INTERNAL "")
                endforeach()
            endif()
            FetchContent_Declare(libarchive GIT_REPOSITORY https://github.com/libarchive/libarchive.git GIT_TAG v3.7.7 GIT_SHALLOW TRUE ${CR_FETCH_EXCLUDE})
            FetchContent_MakeAvailable(libarchive)
            target_include_directories(archive_static INTERFACE ${libarchive_SOURCE_DIR}/libarchive)
            target_compile_definitions(archive_static INTERFACE LIBARCHIVE_STATIC)
            add_library(LibArchive::LibArchive ALIAS archive_static)
            set(LibArchive_FOUND TRUE)
            set(LibArchive_VERSION "3.7.7 (built here)")
        endif()
        if(LibArchive_FOUND)
            target_link_libraries(chaosrings PRIVATE LibArchive::LibArchive)
            target_compile_definitions(chaosrings PRIVATE CR_HAVE_LIBARCHIVE=1)
            message(STATUS "Setup: Vita dumps decrypted; archives through libarchive ${LibArchive_VERSION}")
        else()
            message(STATUS "Setup: Vita dumps decrypted; no libarchive - a dump's archive unpacked first")
        endif()
    endif()
    # The GL device on OpenGL ES 3.0 as Android has it (Mesa's GLES): a test of that path
    option(CR_GLES "OpenGL ES 3.0 for the GL device" OFF)
    if(CR_GLES)
        find_library(CR_GLES_LIBRARY NAMES GLESv3 GLESv2 REQUIRED)
        target_link_libraries(chaosrings PRIVATE ${CR_GLES_LIBRARY})
        target_compile_definitions(chaosrings PRIVATE CR_GLES=1)
        message(STATUS "GL device: OpenGL ES 3.0 (${CR_GLES_LIBRARY})")
    endif()
    target_include_directories(chaosrings SYSTEM PRIVATE ${THIRD_PARTY_DIR})
    if(CR_VULKAN)
        add_dependencies(chaosrings cr_shaders)
        target_include_directories(chaosrings PRIVATE ${SHADER_OUT})
        target_link_libraries(chaosrings PRIVATE Vulkan::Vulkan)
        if(APPLE)
            cr_apple_vulkan(chaosrings)
        endif()
        target_compile_definitions(chaosrings PRIVATE CR_HAVE_VULKAN=1)
        if(CR_DLSS)
            cr_link_dlss(chaosrings)
        else()
            message(STATUS "Upscalers: FSR 3.1 (no DLSS SDK: CR_DLSS_SDK)")
        endif()
    endif()
    # A Release build leaves out the viewer's status / key help at the top (F3 shows it)
    target_compile_definitions(chaosrings PRIVATE $<$<CONFIG:Release,MinSizeRel>:CR_RELEASE=1>)
    set_target_properties(chaosrings PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "Chaos Rings"
        MACOSX_BUNDLE_GUI_IDENTIFIER "local.preservation.chaosrings")
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/app-icon.cmake)
    cr_app_icon(chaosrings chaosrings)

    # Headless asset tool (decode/verify), desktop only
    add_executable(crtool tools/crtool.cpp)
    target_link_libraries(crtool PRIVATE crcore)
    target_include_directories(crtool SYSTEM PRIVATE ${THIRD_PARTY_DIR})
endif()

# In-process save importer tests, with synthetic exports and no game assets.
if(NOT ANDROID AND NOT IOS)
    enable_testing()
    add_executable(vita-import-tests tools/vita_import_test.cpp)
    target_link_libraries(vita-import-tests PRIVATE crcore SDL3::SDL3)
    add_test(NAME vita-import COMMAND vita-import-tests)
    if(TARGET cr_atrac9)
        add_executable(atrac9-bits-tests tools/atrac9_bits_test.cpp)
        target_link_libraries(atrac9-bits-tests PRIVATE cr_atrac9)
        add_test(NAME atrac9-bits COMMAND atrac9-bits-tests)
    endif()
endif()
