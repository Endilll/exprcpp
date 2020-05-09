cmake_minimum_required(VERSION 3.16.3)
project(ExprCpp CXX)

set(CMAKE_CXX_STANDARD_REQUIRED 17)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(exprcpp SHARED)
target_sources(exprcpp PRIVATE expr.cpp)

if (CMAKE_HOST_WIN32)
    add_library(vapoursynth SHARED IMPORTED)
    set_target_properties(vapoursynth PROPERTIES
        IMPORTED_LOCATION "C:\\Program Files\\VapourSynth\\core\\vapoursynth.dll"
        IMPORTED_IMPLIB   "C:\\Program Files\\VapourSynth\\sdk\\lib64\\vapoursynth.lib"
    )
    target_include_directories(vapoursynth SYSTEM INTERFACE
        "C:\\Program Files\\VapourSynth\\sdk\\include"
    )
elseif (CMAKE_HOST_UNIX)
    add_library(vapoursynth SHARED IMPORTED)
    set_target_properties(vapoursynth PROPERTIES
        IMPORTED_LOCATION "/usr/lib/libvapoursynth.so"
    )

    find_package(LLVM REQUIRED CONFIG)
    message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
    message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
    target_include_directories(exprcpp PRIVATE ${LLVM_INCLUDE_DIRS})
    target_compile_definitions(exprcpp PRIVATE ${LLVM_DEFINITIONS})
    find_package(Clang REQUIRED CONFIG)
endif()

target_link_libraries(exprcpp PRIVATE
    vapoursynth
    LLVM
    clang-cpp
)