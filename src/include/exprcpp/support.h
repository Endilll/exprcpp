#pragma once

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <llvm/Support/Error.h>
#pragma clang diagnostic pop

#include <vapoursynth/VapourSynth.h>

#include <cstddef>
#include <stdexcept>
#include <string>

namespace exprcpp {

template<typename T>
auto ssize(const T& container) {
    return static_cast<ptrdiff_t>(std::size(container));
}

template<typename T>
T check_result(llvm::Expected<T>&& result, const std::string& message)
{
    if (!result) { throw std::runtime_error{message}; }
    return std::move(*result);
}

template<typename T>
T& check_result(llvm::Expected<T&>&& result, const std::string& message)
{
    if (!result) { throw std::runtime_error{message}; }
    return *result;
}

template<template<typename...> typename T>
void clean_frames(const VSAPI* vsapi, const T<const VSFrameRef*>& frames)
{
    for (const auto frame: frames) {
        vsapi->freeFrame(frame);
    }
}

} // namespace exprcpp