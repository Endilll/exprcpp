add_library(exprcpp SHARED)
target_sources(exprcpp PRIVATE
    ast_action.cpp
    expr.cpp
    jit_src_builder.cpp
)
target_include_directories(exprcpp PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(exprcpp PRIVATE
    GSL
)

if (CMAKE_HOST_UNIX)
    find_package(LLVM REQUIRED CONFIG)
    message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
    message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
    find_package(Clang REQUIRED CONFIG)

    target_include_directories(exprcpp SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
    target_compile_definitions(exprcpp PRIVATE ${LLVM_DEFINITIONS})

    target_link_libraries(exprcpp PRIVATE
        clang-cpp
        LLVM
        PkgConfig::vapoursynth
    )
    target_compile_options(exprcpp PRIVATE -O3 -march=native)
elseif (CMAKE_HOST_WIN32)
    get_target_property(clang_include_dirs clangBasic INCLUDE_DIRECTORIES)
    target_include_directories(exprcpp SYSTEM PRIVATE ${clang_include_dirs})

    target_compile_definitions(exprcpp PRIVATE ${LLVM_DEFINITIONS})
    target_link_libraries(exprcpp PRIVATE
        clangAST
        clangBasic
        clangCodeGen
        clangDriver
        clangFrontend
        LLVMOrcJIT
        LLVMX86CodeGen
        vapoursynth
    )

    target_compile_options(exprcpp PRIVATE /EHsc /GR)
endif()