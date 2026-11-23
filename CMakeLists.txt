cmake_minimum_required(VERSION 3.20)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" BZCONV_VERSION_RAW)
string(STRIP "${BZCONV_VERSION_RAW}" BZCONV_VERSION)
project(bzconverter VERSION ${BZCONV_VERSION} LANGUAGES CXX)
add_compile_definitions(BZCONV_VERSION="${BZCONV_VERSION}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find ZLIB (with MinGW sysroot hints)
find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
    find_path(ZLIB_INCLUDE_DIR NAMES zlib.h
        HINTS
            "${CMAKE_CXX_COMPILER}/../../x86_64-w64-mingw32/include"
            "C:/ProgramData/mingw64/mingw64/x86_64-w64-mingw32/include"
    )
    find_library(ZLIB_LIBRARY NAMES z zlib zdll
        HINTS
            "${CMAKE_CXX_COMPILER}/../../x86_64-w64-mingw32/lib"
            "C:/ProgramData/mingw64/mingw64/x86_64-w64-mingw32/lib"
    )
    include(FindPackageHandleStandardArgs)
    find_package_handle_standard_args(ZLIB DEFAULT_MSG ZLIB_LIBRARY ZLIB_INCLUDE_DIR)
    if(ZLIB_FOUND AND NOT TARGET ZLIB::ZLIB)
        add_library(ZLIB::ZLIB UNKNOWN IMPORTED)
        set_target_properties(ZLIB::ZLIB PROPERTIES
            IMPORTED_LOCATION "${ZLIB_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${ZLIB_INCLUDE_DIR}"
        )
    endif()
endif()

# Core library sources
set(BZCONVERTER_CORE_SOURCES
    src/bzconverter/core/types.hpp
    src/bzconverter/core/json.hpp
    src/bzconverter/core/sha256.hpp
    src/bzconverter/core/output_bundle.hpp
    src/bzconverter/formats/probe.hpp
    src/bzconverter/formats/probe.cpp
    src/bzconverter/formats/msh/msh_parser.hpp
    src/bzconverter/formats/msh/msh_parser.cpp
    src/bzconverter/formats/xsi/xsi_parser.hpp
    src/bzconverter/formats/xsi/xsi_parser.cpp
    src/bzconverter/materials/material_resolver.hpp
    src/bzconverter/materials/material_resolver.cpp
    src/bzconverter/materials/texture_codec.hpp
    src/bzconverter/materials/texture_codec.cpp
    src/bzconverter/scene/scene.hpp
    src/bzconverter/scene/scene.cpp
    src/bzconverter/exporters/exporter.hpp
    src/bzconverter/exporters/fbx_exporter.hpp
    src/bzconverter/exporters/fbx_exporter.cpp
    src/bzconverter/exporters/glb_exporter.hpp
    src/bzconverter/exporters/glb_exporter.cpp
    src/bzconverter/exporters/obj_exporter.hpp
    src/bzconverter/exporters/obj_exporter.cpp
    src/bzconverter/validation/validator.hpp
    src/bzconverter/validation/validator.cpp
)

add_library(bzconverter_core STATIC ${BZCONVERTER_CORE_SOURCES})
target_include_directories(bzconverter_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(bzconverter_core PUBLIC ZLIB::ZLIB)

if(MSVC)
    target_compile_options(bzconverter_core PRIVATE /W4 /utf-8)
else()
    target_compile_options(bzconverter_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# CLI executable
add_executable(bzconv src/cli/main.cpp)
target_link_libraries(bzconv PRIVATE bzconverter_core)

# Copy python bridge script next to executable and to scripts/ folder
add_custom_command(TARGET bzconv POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:bzconv>/scripts
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bzconverter/exporters/blender_scripts/bz_fbx_bridge.py
        $<TARGET_FILE_DIR:bzconv>/scripts/bz_fbx_bridge.py
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bzconverter/exporters/blender_scripts/bz_fbx_bridge.py
        $<TARGET_FILE_DIR:bzconv>/bz_fbx_bridge.py
    COMMENT "Copying Blender FBX bridge script"
)

# Test executable
if(WIN32)
    add_executable(bzconv_gui WIN32 src/gui/gui_main.cpp)
    target_link_libraries(bzconv_gui PRIVATE bzconverter_core comdlg32 comctl32 gdi32 user32 shell32 ole32 uxtheme urlmon)
    if(MINGW)
        target_link_options(bzconv_gui PRIVATE -municode)
    endif()
endif()

# Test executable
enable_testing()
add_executable(bzconv_tests tests/test_main.cpp)
target_link_libraries(bzconv_tests PRIVATE bzconverter_core)
add_test(NAME bzconv_suite COMMAND bzconv_tests WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# Stage the exact MinGW runtime the compiler links against. A different
# MinGW on PATH supplies older libstdc++/libgcc DLLs, which makes both
# executables fail at load with 0xc0000139 (entry point not found) even
# though the build and the tests look fine. A DLL beside the EXE wins over
# any copy found on PATH, so the package is self-contained and reproducible.
if(MINGW AND NOT MSVC)
    get_filename_component(_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_bz_runtime_dlls libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
    foreach(_dll IN LISTS _bz_runtime_dlls)
        if(EXISTS "${_mingw_bin}/${_dll}")
            add_custom_command(TARGET bzconv POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_mingw_bin}/${_dll}" "$<TARGET_FILE_DIR:bzconv>/${_dll}"
                VERBATIM)
            add_custom_command(TARGET bzconv_tests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_mingw_bin}/${_dll}" "$<TARGET_FILE_DIR:bzconv_tests>/${_dll}"
                VERBATIM)
        endif()
    endforeach()
endif()
