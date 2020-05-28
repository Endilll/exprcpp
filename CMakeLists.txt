cmake_minimum_required(VERSION 3.16.3)
project(ExprCpp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(COMPILER_WARNINGS
    "-Wall"
    "-Wextra"
    "-Wpedantic"
    "-Wshadow"
)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include(GNUInstallDirs)
    if (NOT CMAKE_LIBRARY_OUTPUT_DIRECTORY)
        set(CMAKE_LIBRARY_OUTPUT_DIRECTORY
            ${CMAKE_CURRENT_BINARY_DIR}/stage/${CMAKE_INSTALL_LIBDIR})
    endif()

    add_compile_options(${COMPILER_WARNINGS})
    add_compile_options("-fcolor-diagnostics")
endif()

add_subdirectory(dependencies)

add_subdirectory(src)