#include <array>
#include <cstdint>
#include <ctime>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

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
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <llvm/ADT/IntrusiveRefCntPtr.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Option/Option.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/VirtualFileSystem.h>

#include <vapoursynth/VapourSynth.h>

using namespace std::literals;

static constexpr auto predefined_includes{"#include <cstdint>\n\n"sv};

struct Exprcpp_data {
    VSNodeRef* src_a_node;
    VSNodeRef* src_b_node;
    std::string source_code{predefined_includes};
    const VSVideoInfo* src_a_info;
    const VSVideoInfo* src_b_info;
    std::function<int(int, int)> user_func;
    std::unique_ptr<llvm::orc::LLJIT> jit;
};

static void VS_CC exprcpp_init(VSMap* in, VSMap* out, void** instance_data,
                               VSNode* node, VSCore* core, const VSAPI* vsapi)
{
    const auto data{static_cast<Exprcpp_data*>(*instance_data)};
    vsapi->setVideoInfo(data->src_a_info, 1, node);
}

static const VSFrameRef *VS_CC exprcpp_get_frame(int n, int activationReason,
                                                 void** instance_data,
                                                 void** frame_data,
                                                 VSFrameContext* frame_ctx,
                                                 VSCore* core,
                                                 const VSAPI* vsapi)
{
    const auto data{static_cast<Exprcpp_data*>(*instance_data)};

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, data->src_a_node, frame_ctx);
        vsapi->requestFrameFilter(n, data->src_b_node, frame_ctx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef* const src_a{vsapi->getFrameFilter(
            n, data->src_a_node, frame_ctx)};
        const VSFrameRef* const src_b{vsapi->getFrameFilter(
            n, data->src_b_node, frame_ctx)};

        const VSFormat* const dst_format{data->src_a_info->format};
        const int height{vsapi->getFrameHeight(src_a, 0)};
        const int width{vsapi->getFrameWidth(src_a, 0)};
        constexpr std::array<int, 3> planes{0, 1, 2};
        std::array<const VSFrameRef*, 3> srcf{nullptr, nullptr, nullptr};
        VSFrameRef* const dst{vsapi->newVideoFrame2(
            dst_format, width, height, srcf.data(), planes.data(), src_a, core)};

        for (int i{0}; i < dst_format->numPlanes; ++i) {
            const uint8_t* const src_a_ptr{vsapi->getReadPtr(src_a, i)};
            const uint8_t* const src_b_ptr{vsapi->getReadPtr(src_b, i)};
            uint8_t* const dst_ptr{vsapi->getWritePtr(dst, i)};
            int h{vsapi->getFrameHeight(dst, i)};
            int w{vsapi->getFrameWidth(dst, i)};

            for (long j{0}; j < w*h; ++j) {
                dst_ptr[j] = static_cast<uint8_t>(
                    data->user_func(src_a_ptr[j], src_b_ptr[j]));
            }
        }

        return dst;
    }

    return 0;
}

static void VS_CC exprcpp_free(void* instance_data, VSCore* core,
                               const VSAPI* vsapi)
{
    const auto data{static_cast<Exprcpp_data*>(instance_data)};
    vsapi->freeNode(data->src_a_node);
    vsapi->freeNode(data->src_b_node);
    delete data;
    llvm::llvm_shutdown();
}

static void VS_CC exprcpp_create(const VSMap* in, VSMap* out, void* userData,
                                 VSCore* core, const VSAPI* vsapi)
{
    const auto data{new Exprcpp_data{vsapi->propGetNode(in, "clip_a", 0, 0),
                                     vsapi->propGetNode(in, "clip_b", 0, 0)}};
    data->source_code += vsapi->propGetData(in, "code", 0, 0);
    data->src_a_info = vsapi->getVideoInfo(data->src_a_node);
    data->src_b_info = vsapi->getVideoInfo(data->src_b_node);


    // LLVM stuff

    // Overlay real FS with in-memory one, and load user code there.
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> real_fs{
        llvm::vfs::getRealFileSystem()};
    llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> mem_vfs{
        new llvm::vfs::InMemoryFileSystem{}};
    llvm::IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> vfs{
        new llvm::vfs::OverlayFileSystem{llvm::vfs::getRealFileSystem()}};
    vfs->pushOverlay(mem_vfs);
    mem_vfs->addFile("expr.cpp", std::time(nullptr),
                 llvm::MemoryBuffer::getMemBuffer(data->source_code));

    // Create diagnostics engine for driver.
    llvm::IntrusiveRefCntPtr<clang::DiagnosticOptions> diag_opts{
        new clang::DiagnosticOptions()};
    clang::TextDiagnosticPrinter* diag_client{
        new clang::TextDiagnosticPrinter{llvm::errs(), &*diag_opts}};
    llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs> diag_id{
        new clang::DiagnosticIDs()};
    clang::DiagnosticsEngine diags{diag_id, &*diag_opts, diag_client};

    // Create driver.
    clang::driver::Driver driver{"clang", llvm::sys::getProcessTriple(), diags,
                                 vfs};

    // Create compilation using driver.
    llvm::SmallVector<const char*, 3> args{"clang", "expr.cpp", "-fsyntax-only"};
    std::unique_ptr<clang::driver::Compilation> compilation{
        driver.BuildCompilation(args)};
    if (!compilation) {
        vsapi->setError(out, "Failed to create compilation");
        return;
    }

    // Extract arguments from compilation.
    const clang::driver::JobList& jobs{compilation->getJobs()};
    if (jobs.size() != 1 || !clang::isa<clang::driver::Command>(*jobs.begin())) {
        vsapi->setError(out, "Failed to create job list");
        return;
    }
    const clang::driver::Command& cmd{
        llvm::cast<clang::driver::Command>(*jobs.begin())};
    const llvm::opt::ArgStringList& cc_args{cmd.getArguments()};
    auto ci{std::make_unique<clang::CompilerInvocation>()};
    clang::CompilerInvocation::CreateFromArgs(*ci, cc_args, diags);

    // Create compiler instance to do the work.
    clang::CompilerInstance clang;
    clang.setInvocation(std::move(ci));
    clang.createFileManager(vfs);
    if (!clang.hasFileManager()) {
        vsapi->setError(out, "Failed to create file manager from virtual FS");
        return;
    }
    clang.createDiagnostics();
    if (!clang.hasDiagnostics()) {
        vsapi->setError(out, "Failed to create diagnostics engine");
        return;
    }

    // Emit LLVM IR and extract mangled function name
    std::unique_ptr<clang::CodeGenAction> act{new clang::EmitLLVMOnlyAction{}};
    if (!clang.ExecuteAction(*act)) {
        vsapi->setError(out, "Failed to emit LLVM bitcode");
        return;
    }
    std::unique_ptr<llvm::LLVMContext> ctx{act->takeLLVMContext()};
    std::unique_ptr<llvm::Module> module{act->takeModule()};
    if (!module) {
        vsapi->setError(out, "Failed to create module");
        return;
    }
    std::string func_name{[&module]{
        for (auto& v: *module) {
            if (v.hasName()) {
                return v.getName().data();
            }
        }
        // throw
    }()};

    // Initialize backend
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    auto jit{[]() {
        if (auto expected{llvm::orc::LLJITBuilder().create()}; expected) {
            return std::move(*expected);
        } else {
            // throw
        }
    }()};

    // Add generator to JIT to resolve symbols from C and C++ runtimes
    // referenced in user code (i.e. printf), and prevent any loaded libraries
    // from unloading.
    auto psg{llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
        jit->getDataLayout().getGlobalPrefix())};
    if (!psg) {
        vsapi->setError(out, "Unable to get symbols generator");
        return;
    }
    jit->getMainJITDylib().addGenerator(std::move(*psg));
    llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);

    // Add IR module to JIT and get the symbol of user function.
    auto ts_module{llvm::orc::ThreadSafeModule{std::move(module),
                                               std::move(ctx)}};
    if (jit->addIRModule(std::move(ts_module))) {
        vsapi->setError(out, "Failed to add IR module to JIT");
        return;
    }
    const auto symbol{[&jit, &func_name]() {
        if (auto expected{jit->lookupLinkerMangled(jit->getMainJITDylib(), func_name)};
            expected)
        {
            return *expected;
        } else {
            // throw
        }
    }()};

    // Store address of user function and JIT itself in filter's data to keep
    // function alive. getAddress() triggers compilation into native code.
    data->user_func = reinterpret_cast<int (*)(int, int)>(symbol.getAddress());
    data->jit = std::move(jit);

    vsapi->createFilter(in, out, "expr_cpp", exprcpp_init, exprcpp_get_frame,
                        exprcpp_free, fmParallel, 0, data, core);
}

VS_EXTERNAL_API(void) VapourSynthPluginInit(VSConfigPlugin config_func,
                                            VSRegisterFunction register_func,
                                            VSPlugin* plugin)
{
  config_func("com.endill.ExprCpp", "endill", "C++-based Expr",
              VAPOURSYNTH_API_VERSION,1, plugin);
  register_func("expr_cpp", "clip_a:clip;clip_b:clip;code:data",
                exprcpp_create, nullptr, plugin);
  return;
}