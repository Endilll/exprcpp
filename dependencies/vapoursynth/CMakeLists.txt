if (CMAKE_HOST_WIN32)
    find_path(vapoursynth_DLL_PATH "vapoursynth.dll")
    set(vapoursynth_SDK_PATH "${vapoursynth_DLL_PATH}/../sdk")

    add_library(vapoursynth SHARED IMPORTED GLOBAL)
    set_target_properties(vapoursynth PROPERTIES
        IMPORTED_LOCATION ${vapoursynth_DLL_PATH}
        IMPORTED_IMPLIB "${vapoursynth_SDK_PATH}/lib64/vapoursynth.lib"
    )
    target_include_directories(vapoursynth SYSTEM INTERFACE
        "${vapoursynth_SDK_PATH}/include"
    )
elseif (CMAKE_HOST_UNIX)
    find_package(PkgConfig)
    pkg_search_module(vapoursynth REQUIRED IMPORTED_TARGET GLOBAL
        vapoursynth)
endif()