#pragma once

#include <vapoursynth/VapourSynth.h>

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace exprcpp {

class Jit_src_builder {
    std::set<std::string> includes_;
    std::string user_code_;

    std::string create_includes();
    std::string create_loop_func();
    std::string create_entry_func();

public:
    static constexpr auto entry_func_ns{"exprcpp"};
    static constexpr auto entry_func_name{"run"};
    using entry_func_ptr = void (*)(long, int, int, void**);
    using entry_func_type =
        std::function<std::remove_pointer_t<entry_func_ptr>>;

    static constexpr auto builtin_includes{"#include <cstdint>\n\n"};

    std::string user_func_name;
    const VSFormat* dst_fmt;
    const std::vector<const VSFormat*>& src_fmts;

    Jit_src_builder(const std::vector<const VSFormat*>& src_fmts);

    const std::string& user_code();
    void user_code(const std::string& user_code);

    std::string full_source();
};
} // namespace exprcpp