#include "exprcpp/ast_action.h"
#include "exprcpp/jit_src_builder.h"
#include "exprcpp/support.h"
#include <iterator>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/DiagnosticFrontend.h>
#include <clang/Basic/DiagnosticIDs.h>
#include <clang/Basic/DiagnosticOptions.h>
#include <clang/CodeGen/CodeGenAction.h>
#include <clang/Driver/Compilation.h>
#include <clang/Driver/Driver.h>
#include <clang/Driver/Job.h>
#include <clang/Driver/Tool.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/CompilerInvocation.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <llvm/ADT/IntrusiveRefCntPtr.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/ExecutionEngine/JITSymbol.h>
#include <llvm/ExecutionEngine/Orc/DebugUtils.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Option/Option.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/raw_os_ostream.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/VirtualFileSystem.h>

// #include <clang/AST/ASTContext.h>
// #include <clang/AST/Decl.h>
// #include <clang/AST/DeclBase.h>
// #include <clang/AST/DeclGroup.h>
// #include <clang/AST/Mangle.h>
// #include <llvm/Support/Casting.h>
#pragma clang diagnostic pop

#include <gsl/gsl>
#include <vapoursynth/VapourSynth.h>

#include <bitset>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace std::literals;

namespace exprcpp {

class Dump_info {
public:
    std::filesystem::path dump_path;

    Dump_info(const VSAPI& vsapi, const VSMap& vsmap)
    {
        int err{0};
        auto get_bool_value{[&](const std::string& name, bool fallback) {
            if (bool value{static_cast<bool>(
                    vsapi.propGetInt(&vsmap, name.c_str(), 0, &err))}; !err) {
                return value;
            }
            return fallback;
        }};

        dump_source( get_bool_value("dump_source"s,  false));
        dump_bitcode(get_bool_value("dump_bitcode"s, false));
        dump_binary( get_bool_value("dump_binary"s,  false));

        this->dump_path = [&]() -> std::filesystem::path {
            const char* path_c{vsapi.propGetData(&vsmap, "dump_path", 0, &err)};
            if (err) {
                auto path{std::filesystem::current_path()};
                if (flags_.any()) {
                    vsapi.logMessage(
                        mtWarning, ("expr_cpp: using CWD for dumping: "s
                                    + path.string()).c_str());
                }
                return path;
            }
            return {path_c};
        }();

    }

    bool dump_source() const  { return flags_[0]; }
    bool dump_bitcode() const { return flags_[1]; }
    bool dump_binary() const  { return flags_[2]; }

    void dump_source(bool value)  { flags_[0] = value; }
    void dump_bitcode(bool value) { flags_[1] = value; }
    void dump_binary(bool value)  { flags_[2] = value; }

private:
    std::bitset<3> flags_;
};


struct Exprcpp_data {
    std::vector<VSNodeRef*> srcs;
    VSVideoInfo* dst_info;
    std::vector<std::pair<VSNodeRef*, int>> dst_init;
    std::vector<Jit_src_builder::entry_func_type> jit_funcs;
    std::unique_ptr<llvm::orc::LLJIT> jit;
};

void VS_CC init(VSMap*, VSMap*, void** instance_data, VSNode* node, VSCore*,
                const VSAPI* vsapi)
{
    const auto* data{static_cast<const Exprcpp_data*>(*instance_data)};
    vsapi->setVideoInfo(data->dst_info, 1, node);
}

const VSFrameRef *VS_CC get_frame(
    int n, int activationReason, void** instance_data, void**,
    VSFrameContext* frame_ctx, VSCore* core, const VSAPI* vsapi)
{
    const auto* data{static_cast<const Exprcpp_data*>(*instance_data)};

    if (activationReason == arInitial) {
        for (auto* src: data->srcs) {
            vsapi->requestFrameFilter(n, src, frame_ctx);
        }
    } else if (activationReason == arAllFramesReady) {
        Expects(!data->dst_init.empty());
        Expects(!data->jit_funcs.empty());

        auto src_frames{[&]() {
            std::vector<const VSFrameRef*> src_frames;
            src_frames.reserve(ssize(data->srcs));
            for (auto* node: data->srcs) {
                src_frames.push_back(vsapi->getFrameFilter(n, node, frame_ctx));
            }
            return src_frames;
        }()};
        auto src_frames_cleaner{gsl::finally(
            [&]() { clean_frames(vsapi, src_frames); })};

        VSFrameRef* const dst_frame{[&]() {
            std::vector<const VSFrameRef*> copy_src_frames;
            std::vector<int> copy_src_planes;
            for (const auto& [node, plane]: data->dst_init) {
                const VSFrameRef* frame{nullptr};
                if (node) {
                    frame = vsapi->getFrameFilter(n, node, frame_ctx);
                }
                copy_src_frames.push_back(frame);
                copy_src_planes.push_back(plane);
            }
            auto copy_src_frames_cleaner{gsl::finally(
                [&]() { clean_frames(vsapi, copy_src_frames); })};

            return vsapi->newVideoFrame2(
                data->dst_info->format, vsapi->getFrameWidth(src_frames[0], 0),
                vsapi->getFrameHeight(src_frames[0], 0), copy_src_frames.data(),
                copy_src_planes.data(), src_frames[0], core);
        }()};

        for (gsl::index plane{0}; plane != data->dst_info->format->numPlanes;
                                                                      ++plane) {
            const auto jit_func{data->jit_funcs[plane]};
            if (!jit_func) { continue; }

            const long pixel_count{vsapi->getFrameWidth(dst_frame, plane)
                                   * vsapi->getFrameHeight(dst_frame, plane)};

            for (gsl::index i{0}; i != ssize(src_frames); ++i) {
                const long src_pixel_count{
                    vsapi->getFrameWidth(src_frames[i], plane)
                    * vsapi->getFrameHeight(src_frames[i], plane)};
                if (src_pixel_count != pixel_count) {
                    throw std::runtime_error{
                        "Frame "s + std::to_string(n) + " of input clip #"s
                        + std::to_string(i) + " has different number of pixels"s
                        "than output frame"s};
                }
            }

            auto data_ptrs{[&]() {
                std::vector<void*> data_ptrs{
                    vsapi->getWritePtr(dst_frame, plane)};
                data_ptrs.reserve(ssize(data->srcs) + 1);
                for (const auto* src_frame: src_frames) {
                    data_ptrs.push_back(const_cast<uint8_t*>(
                        vsapi->getReadPtr(src_frame, plane)));
                }
                return data_ptrs;
            }()};

            jit_func(pixel_count, data_ptrs.data());
        }

        return dst_frame;
    }

    return 0;
}

void VS_CC free(void* instance_data, VSCore*, const VSAPI* vsapi)
{
    const auto* data{static_cast<Exprcpp_data*>(instance_data)};
    for (auto* src : data->srcs) {
        vsapi->freeNode(src);
    }
    delete data;
    llvm::llvm_shutdown();
}

Jit_src_builder::entry_func_type process_source(
    llvm::orc::LLJIT& jit, Jit_src_builder& src_builder,
    const Dump_info dump_info, const gsl::index plane,
    const std::vector<const char*>& cxxflags = {"-O3", "-std=c++17",
    "-march=native"})
{
    llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> mem_vfs{
        new llvm::vfs::InMemoryFileSystem{}};
    llvm::IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> vfs{
        new llvm::vfs::OverlayFileSystem{mem_vfs}};
    vfs->pushOverlay(llvm::vfs::getRealFileSystem());

    llvm::raw_os_ostream llvm_cout{std::cout};

    llvm::IntrusiveRefCntPtr<clang::DiagnosticOptions> diag_opts{
        new clang::DiagnosticOptions()};
    clang::TextDiagnosticPrinter* diag_client{
        new clang::TextDiagnosticPrinter{llvm_cout, diag_opts.get()}};
    llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs> diag_id{
        new clang::DiagnosticIDs()};
    clang::DiagnosticsEngine diags{diag_id, diag_opts.get(), diag_client};

    auto build_compiler_invocation{[&](const std::string& file_name) {
        clang::driver::Driver driver{"clang++", llvm::sys::getProcessTriple(),
                                     diags, vfs};

        llvm::SmallVector<const char*, 20> args{
            "clang++", file_name.c_str(), "-fsyntax-only"};
        args.append(cxxflags.cbegin(), cxxflags.cend());
        std::unique_ptr<clang::driver::Compilation> compilation{
            driver.BuildCompilation(args)};
        if (!compilation) {
            throw std::runtime_error{"Failed to create compilation"s};
        }

        const clang::driver::JobList& jobs{compilation->getJobs()};
        if (ssize(jobs) != 1
            || !clang::isa<clang::driver::Command>(*jobs.begin()))
        {
            throw std::runtime_error{"Failed to create job list"s};
        }
        const clang::driver::Command& cmd{
            llvm::cast<clang::driver::Command>(*jobs.begin())};
        const llvm::opt::ArgStringList& cc_args{cmd.getArguments()};
        auto ci{std::make_unique<clang::CompilerInvocation>()};
        clang::CompilerInvocation::CreateFromArgs(*ci, cc_args, diags);
        return ci;
    }};

    clang::CompilerInstance ci{};
    ci.createFileManager(vfs);
    if (!ci.hasFileManager()) {
        throw std::runtime_error{
            "Failed to create file manager from virtual FS"s};
    }

    auto execute_action{[&ci](clang::FrontendAction& action) {
        ci.createDiagnostics();
        if (!ci.hasDiagnostics()) {
            throw std::runtime_error{"Failed to create diagnostics engine"s};
        }
        ci.resetAndLeakSourceManager();
        if (!ci.ExecuteAction(action)) {
            throw std::runtime_error{"Failed to execute frontend action"s};
        }
    }};

    if (src_builder.user_func_name.empty()) {
        mem_vfs->addFile(
            "expr.cpp", std::time(nullptr),
            llvm::MemoryBuffer::getMemBuffer(src_builder.user_code()));
        ci.setInvocation(build_compiler_invocation("expr.cpp"s));

        std::string user_func_name{};
        {
            std::string _;
            Name_extractor_action name_action{user_func_name, _};
            execute_action(name_action);
        }
        if (user_func_name.empty()) {
            throw std::runtime_error{"User function not found"s};
        }
        src_builder.user_func_name = user_func_name;
    }

    std::string jit_source{src_builder.full_source()};

    if (dump_info.dump_source()) {
        std::ofstream ofs{dump_info.dump_path
                          / (src_builder.user_func_name + "_dump.cpp"s)};
        ofs << jit_source;
    }

    mem_vfs->addFile("expr_full.cpp", std::time(nullptr),
                     llvm::MemoryBuffer::getMemBuffer(jit_source));

    ci.setInvocation(build_compiler_invocation("expr_full.cpp"s));

    std::string entry_name_mangled;
    {
        std::string _;
        Name_extractor_action name_action{_, entry_name_mangled};
        execute_action(name_action);
    }

    clang::EmitLLVMOnlyAction main_action{};
    execute_action(main_action);

    std::unique_ptr<llvm::LLVMContext> ctx{main_action.takeLLVMContext()};
    std::unique_ptr<llvm::Module> module{main_action.takeModule()};
    if (!module) { throw std::runtime_error{"Failed to create module"s}; }

    if (dump_info.dump_bitcode()) {
        std::ofstream ofs{dump_info.dump_path
                          / (src_builder.user_func_name + "_dump.bc"s),
                          std::ofstream::binary};
        llvm::raw_os_ostream os{ofs};
        WriteBitcodeToFile(*module, os);
    }

    auto& jd{check_result(jit.createJITDylib(std::to_string(plane)),
                          "Failed to create JITDylib"s)};
    auto psg{check_result(
        llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            jit.getDataLayout().getGlobalPrefix()),
        "Unable to get symbols generator"s)};
    jd.addGenerator(std::move(psg));

    if (dump_info.dump_binary()) {
        jit.getObjTransformLayer().setTransform(
            llvm::orc::DumpObjects{dump_info.dump_path.string(),
                                   src_builder.user_func_name});
    }
    auto disable_object_dumper{gsl::finally([&]() {
        if (dump_info.dump_binary()) {
            jit.getObjTransformLayer().setTransform(nullptr);
        }
    })};

    auto ts_module{llvm::orc::ThreadSafeModule{std::move(module),
                                               std::move(ctx)}};
    if (jit.addIRModule(jd, std::move(ts_module))) {
        throw std::runtime_error{"Failed to add IR module to JIT"s};
    }
    auto symbol{check_result(
        jit.lookupLinkerMangled(jd, entry_name_mangled),
        "Failed to find user function symbol"s)};
    return llvm::jitTargetAddressToPointer<Jit_src_builder::entry_func_ptr>(
        symbol.getAddress());
}

void VS_CC create(const VSMap* in, VSMap* out, void*, VSCore* core,
                  const VSAPI* vsapi) try
{
    auto data{std::make_unique<Exprcpp_data>()};
    std::vector<const VSFormat*> src_fmts;
    int err{0};
    for (gsl::index i{0}; i != vsapi->propNumElements(in, "clips"); ++i) {
        VSNodeRef* const node{vsapi->propGetNode(in, "clips", i, &err)};
        if (err) {
            throw std::runtime_error{"Failed to get one of the inputs"s};
        }
        if (const VSFormat* fmt{vsapi->getVideoInfo(node)->format}) {
            src_fmts.push_back(fmt);
        } else {
            throw std::runtime_error{
                "Input clips with non-constant format aren't allowed"s};
        }
        data->srcs.push_back(node);
    }
    if (data->srcs.empty()) { throw std::runtime_error{"No input clips"s}; }

    data->dst_info =
        const_cast<VSVideoInfo*>(vsapi->getVideoInfo(data->srcs[0]));
    if (int64_t fmt_idx{vsapi->propGetInt(in, "format", 0, &err)}; !err) {
        // TODO: add format checks if necessary
        data->dst_info->format = vsapi->getFormatPreset(fmt_idx, core);
    }
    for (const auto& fmt: src_fmts) {
        if (fmt->numPlanes != data->dst_info->format->numPlanes) {
            throw std::runtime_error{
                "All inputs must have the same number of planes"s};
        }
        if (fmt->subSamplingH != data->dst_info->format->subSamplingH
            || fmt->subSamplingW != data->dst_info->format->subSamplingW) {
            throw std::runtime_error{
                "All inputs mush have the same subsampling"s};
        }
    }

    Jit_src_builder src_builder_common{src_fmts};
    src_builder_common.dst_fmt = data->dst_info->format;

    const auto source_path{[&]() {
        const char* path_c_str{vsapi->propGetData(in, "source_path", 0, &err)};
        if (err) { return std::filesystem::path{}; }
        const std::filesystem::path path{path_c_str};

        const auto status{std::filesystem::status(path)};
        if (!std::filesystem::exists(status)) {
            throw std::runtime_error{"Source file doesn't exist"s};
        }
        if (!std::filesystem::is_regular_file(status)
            && !std::filesystem::is_character_file(status)
            && !std::filesystem::is_fifo(status)
            && !std::filesystem::is_symlink(status))
        {
            throw std::runtime_error{"Source file is not a file"s};
        }
        return path;
    }()};
    if (!source_path.empty()) {
        src_builder_common.user_code(source_path);
    }

    Dump_info dump_info{*vsapi, *in};

    bool user_cxxflags_present{false};
    auto user_cxxflags{[&]() {
        std::vector<const char*> user_cxxflags;
        int64_t user_cxxflags_count{vsapi->propNumElements(in, "cxxflags")};
        if (user_cxxflags_count == -1) { return user_cxxflags; }
        for (gsl::index i{0}; i != user_cxxflags_count; ++i) {
            const char* cxxflag{vsapi->propGetData(in, "cxxflags", i, &err)};
            if (err) { throw std::runtime_error{"Failed to read cxxflag"s}; }
            user_cxxflags.push_back(cxxflag);
        }
        user_cxxflags_present = true;
        return user_cxxflags;
    }()};

    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    auto jit{check_result(llvm::orc::LLJITBuilder().create(),
                          "Failed to create JIT"s)};

    for (gsl::index i{0}; i != data->dst_info->format->numPlanes; ++i) {
        const char* user_code_c_str{vsapi->propGetData(in, "code", i, &err)};
        if (err) {
            data->jit_funcs.push_back(
                data->jit_funcs[ssize(data->jit_funcs) - 1]);
            data->dst_init.push_back(
                {data->dst_init[ssize(data->dst_init) - 1].first, i});
            continue;
        }
        std::string user_code{user_code_c_str};
        if (user_code.empty()) {
            data->jit_funcs.emplace_back(nullptr);
            data->dst_init.push_back({data->srcs[0], i});
            continue;
        }
        Jit_src_builder src_builder{src_builder_common};
        if (!source_path.empty()) {
            src_builder.user_func_name = user_code;
        } else {
            src_builder.user_code(user_code);
        }
        auto jit_func{[&]() {
            if (user_cxxflags_present) {
                return process_source(*jit, src_builder, dump_info, i,
                                      user_cxxflags);
            }
            return process_source(*jit, src_builder, dump_info, i);
        }()};
        data->jit_funcs.push_back(jit_func);
        data->dst_init.push_back({nullptr, i});
    }

    data->jit = std::move(jit);

    vsapi->createFilter(in, out, "expr_cpp", init, get_frame,
                        free, fmParallel, 0, data.release(), core);
} catch (const std::exception& ex) {
    vsapi->setError(out, ("expr_cpp: "s + ex.what()).c_str());
}
} // namespace exprcpp

VS_EXTERNAL_API(void) VapourSynthPluginInit(VSConfigPlugin config_func,
                                            VSRegisterFunction register_func,
                                            VSPlugin* plugin)
{
    config_func("org.endill.expr", "expr", "C++-based Expr",
                VAPOURSYNTH_API_VERSION, 1, plugin);
    register_func("expr_cpp", "clips:clip[];code:data[];format:int:opt;"
                              "source_path:data:opt;cxxflags:data[]:opt:empty;"
                              "dump_path:data:opt;dump_source:int:opt;"
                              "dump_bitcode:int:opt;dump_binary:int:opt",
                  exprcpp::create, nullptr, plugin);
    return;
}