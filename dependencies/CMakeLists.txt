include(FetchContent)

FetchContent_Declare(gsl
    GIT_REPOSITORY https://github.com/microsoft/GSL.git
    GIT_TAG 1999b48a519196711f0d03af3b7eedd49fcc6db3 # 3.0.1
)

FetchContent_MakeAvailable(gsl)

add_subdirectory(vapoursynth)