#include "exprcpp/jit_src_builder.h"

#include "exprcpp/support.h"

#include <gsl/gsl>

#include <fstream>
#include <iterator>
#include <numeric>

using namespace std::literals;

namespace exprcpp {

std::string Jit_src_builder::create_includes()
{
    return std::accumulate(includes_.cbegin(), includes_.cend(), ""s,
        [](const std::string& acc, const std::string& elem) {
            return acc + "#include <"s + elem + ">\n"s;
        }
    ) + "\n";
}

std::string Jit_src_builder::create_loop_func()
{
    Expects(!this->user_func_name.empty());

    includes_.emplace("algorithm"s);
    includes_.emplace("limits"s);
    includes_.emplace("type_traits"s);
    includes_.emplace("utility"s);
    static const auto user_func_placeholder{"USER_FUNC_NAME"s};
    std::string loop_func{
R"EOS(

namespace exprcpp {
template<typename Dst_ptr, typename... Src_ptrs,
         typename Dst_t = std::remove_pointer_t<Dst_ptr>>
void run_loop(long pixel_count, std::pair<Dst_t, Dst_t> value_range,
              Dst_ptr dst, Src_ptrs... srcs)
{
    using User_t = decltype(USER_FUNC_NAME(srcs[0]...));

    for (long i{0}; i < pixel_count; ++i) {
        if constexpr (!std::is_same_v<Dst_t, User_t>
                      && std::is_integral_v<Dst_t>
                      && std::is_integral_v<User_t>
                      && std::numeric_limits<Dst_t>::max()
                         < std::numeric_limits<User_t>::max()) {
            dst[i] = std::clamp<User_t>(USER_FUNC_NAME(srcs[i]...),
                                        value_range.first, value_range.second);
        } else {
            dst[i] = USER_FUNC_NAME(srcs[i]...);
        }
    }
}
} // namespace exprcpp

)EOS"s};

    auto pos{loop_func.find(user_func_placeholder)};
    while (pos != std::string::npos) {
        loop_func.replace(pos, ssize(user_func_placeholder),
                          this->user_func_name);
        pos = loop_func.find(user_func_placeholder, pos + 1);
    }
    return loop_func;
}

std::string Jit_src_builder::create_entry_func()
{
    Expects(this->dst_fmt);

    includes_.emplace("cstdint"s);
    static auto to_string{[](const VSFormat& fmt) -> std::string {
        switch (fmt.sampleType) {
        case stInteger:
            return "uint"s + std::to_string(fmt.bytesPerSample * 8) + "_t"s;
        case stFloat:
            switch (fmt.bytesPerSample) {
            case 2:
                throw std::runtime_error{"FP16 is not supported yet"s};
            case 4:
                return "float"s;
            case 8:
                return "double"s;
            case 10:
                return "long double"s;
            }
            break;
        }
        throw std::runtime_error{"Unsupported sample type"s};
    }};

    static auto to_ptr_string{[&](const VSFormat& fmt,
                                  bool immutable = true) {
        if (!immutable) {
            return to_string(fmt) + "* const"s;
        }
        return "const "s + to_string(fmt) + "* const"s;
    }};

    std::vector<std::pair<std::string, std::string>> ptrs{
        {"dst"s, to_ptr_string(*this->dst_fmt, /* immutable */ false)}};
    for (gsl::index i{0}; i != ssize(this->src_fmts); ++i) {
        ptrs.emplace_back("src"s + std::to_string(i),
                          to_ptr_string(*this->src_fmts[i]));
    }

    std::string entry_func;
    entry_func +=
"\nnamespace "s + entry_func_ns + " {\n"s;
    entry_func +=
"void "s + entry_func_name + "(long pixel_count, void** data_ptrs)\n"s
"{\n"s;
    for (gsl::index i{0}; i != ssize(ptrs); ++i) {
        const std::string name{ptrs[i].first};
        const std::string type{ptrs[i].second};
        const std::string index{std::to_string(i)};
        entry_func +=
"    auto* const __restrict "s + name + "{static_cast<"s + type + ">("s
                                              "data_ptrs["s + index + "])};\n"s;
    }
    entry_func +=
"    exprcpp::run_loop(pixel_count, {0, "s
               + std::to_string((1 << this->dst_fmt->bitsPerSample) - 1) + "}"s;
    for (const auto& [name, _]: ptrs) {
        entry_func += ", "s + name;
    }
    entry_func += ");\n"s
"}\n"s;
    entry_func +=
"} // namespace "s + entry_func_ns + "\n\n"s;
    return entry_func;
}

Jit_src_builder::Jit_src_builder(const std::vector<const VSFormat*>& src_fmts)
    : src_fmts{src_fmts} {}

const std::string& Jit_src_builder::user_code() { return user_code_; }

void Jit_src_builder::user_code(const std::string& user_code)
{
    user_code_ = builtin_includes + user_code;
}

void Jit_src_builder::user_code(const std::filesystem::path& path)
{
    std::ifstream ifs{path};
    user_code_ = builtin_includes
                 + std::string{std::istreambuf_iterator<char>{ifs}, {}};
}

std::string Jit_src_builder::full_source()
{
    const std::string loop_func{create_loop_func()};
    const std::string entry_func{create_entry_func()};
    // create_includes() needs to be invoked the last
    return create_includes() + user_code_ + loop_func + entry_func;
}
} // namespace exprcpp