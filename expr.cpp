#include <array>
#include <cstdint>
#include <ctime>
#include <functional>
#include <iostream>
#include <memory>
#include <string>

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
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/Triple.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Option/Option.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/VirtualFileSystem.h>

#include <vapoursynth/VapourSynth.h>

struct Exprcpp_data {
    VSNodeRef* src_a_node;
    VSNodeRef* src_b_node;
    std::string source_code;
    const VSVideoInfo* src_a_info;
    const VSVideoInfo* src_b_info;
    std::function<int(int, int)> user_func;
    std::unique_ptr<llvm::orc::LLJIT> jit;
};

std::string get_executable_path(const char* argv_0, void* main_addr) {
  return llvm::sys::fs::getMainExecutable(argv_0, main_addr);
}

llvm::ExitOnError ExitOnErr;

static void VS_CC exprcpp_init(VSMap* in, VSMap* out, void** instance_data,
                               VSNode* node, VSCore* core, const VSAPI* vsapi) {
    const auto data{static_cast<Exprcpp_data*>(*instance_data)};
    vsapi->setVideoInfo(data->src_a_info, 1, node);
}

static const VSFrameRef *VS_CC exprcpp_get_frame(int n, int activationReason,
                                                 void** instance_data,
                                                 void** frame_data,
                                                 VSFrameContext* frame_ctx,
                                                 VSCore* core,
                                                 const VSAPI* vsapi) {
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
                               const VSAPI* vsapi) {
    const auto data{static_cast<Exprcpp_data*>(instance_data)};
    vsapi->freeNode(data->src_a_node);
    vsapi->freeNode(data->src_b_node);
    delete data;
    llvm::llvm_shutdown();
}

static void VS_CC exprcpp_create(const VSMap* in, VSMap* out, void* userData,
                                 VSCore* core, const VSAPI* vsapi) {
    const auto data{new Exprcpp_data{vsapi->propGetNode(in, "clip_a", 0, 0),
                                     vsapi->propGetNode(in, "clip_b", 0, 0),
                                     vsapi->propGetData(in, "code",   0, 0)}};
    data->src_a_info = vsapi->getVideoInfo(data->src_a_node);
    data->src_b_info = vsapi->getVideoInfo(data->src_b_node);


    // LLVM stuff

    llvm::DebugFlag = true;
    llvm::setCurrentDebugType("orc");
    // This just needs to be some symbol in the binary; C++ doesn't
    // allow taking the address of ::main however.
    auto main_addr{reinterpret_cast<void*>(get_executable_path)};
    std::string path{get_executable_path(nullptr, main_addr)};
    llvm::IntrusiveRefCntPtr<clang::DiagnosticOptions> diag_opts{
        new clang::DiagnosticOptions()};
    clang::TextDiagnosticPrinter* diag_client{
        new clang::TextDiagnosticPrinter{llvm::errs(), &*diag_opts}};

    llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs> diag_id{new clang::DiagnosticIDs()};
    clang::DiagnosticsEngine diags{diag_id, &*diag_opts, diag_client};

    const std::string triple_str{llvm::sys::getProcessTriple()};
    llvm::Triple triple{triple_str};

    llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> vfs{
        new llvm::vfs::InMemoryFileSystem{}};
    vfs->addFile("Test.cxx", std::time(nullptr), llvm::MemoryBuffer::getMemBuffer(data->source_code));

    clang::driver::Driver driver(path, triple.str(), diags, vfs);
    driver.setCheckInputsExist(false);

    // FIXME: This is a hack to try to force the driver to do something we can
    // recognize. We need to extend the driver library to support this use model
    // (basically, exactly one input, and the operation mode is hard wired).
    // SmallVector<const char *, 16> Args(argv, argv + argc);
    llvm::SmallVector<const char*, 3> args{"clang", "expr.cpp", "-fsyntax-only"};
    // Args.push_back("-fsyntax-only");
    std::unique_ptr<clang::driver::Compilation> compilation{
        driver.BuildCompilation(args)};
    if (!compilation)
        return;

    // We expect to get back exactly one command job, if we didn't something
    // failed. Extract that job from the compilation.
    const clang::driver::JobList& jobs{compilation->getJobs()};
    if (jobs.size() != 1 || !clang::isa<clang::driver::Command>(*jobs.begin())) {
        llvm::SmallString<256> msg;
        llvm::raw_svector_ostream os{msg};
        jobs.Print(os, "; ", true);
        diags.Report(clang::diag::err_fe_expected_compiler_job) << os.str();
        vsapi->setError(out, os.str().data());
        return;
    }

    const clang::driver::Command& cmd{
        llvm::cast<clang::driver::Command>(*jobs.begin())};
    if (llvm::StringRef(cmd.getCreator().getName()) != "clang") {
        diags.Report(clang::diag::err_fe_expected_clang_command);
        return;
    }

    // Initialize a compiler invocation object from the clang (-cc1) arguments.
    const llvm::opt::ArgStringList& cc_args{cmd.getArguments()};
    auto ci{std::make_unique<clang::CompilerInvocation>()};
    clang::CompilerInvocation::CreateFromArgs(*ci, cc_args, diags);

    // Show the invocation, with -v.
    if (ci->getHeaderSearchOpts().Verbose) {
        llvm::errs() << "clang invocation:\n";
        jobs.Print(llvm::errs(), "\n", true);
        llvm::errs() << "\n";
    }

    // Create a compiler instance to handle the actual work.
    clang::CompilerInstance clang;
    clang.setInvocation(std::move(ci));
    clang.createFileManager(vfs);

    // Create the compilers actual diagnostics engine.
    clang.createDiagnostics();
    if (!clang.hasDiagnostics())
        vsapi->setError(out, "Can't create diagnostics engine");
        return;

    // Infer the builtin include path if unspecified.
    if (clang.getHeaderSearchOpts().UseBuiltinIncludes &&
        clang.getHeaderSearchOpts().ResourceDir.empty())
        clang.getHeaderSearchOpts().ResourceDir =
        clang::CompilerInvocation::GetResourcesPath(nullptr, main_addr);

    // Create and execute the frontend to generate an LLVM bitcode module.
    std::unique_ptr<clang::CodeGenAction> act{new clang::EmitLLVMOnlyAction()};
    if (!clang.ExecuteAction(*act)) {
        vsapi->setError(out, "Can't emit LLVM bitcode");
        return;
    }

    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    std::unique_ptr<llvm::LLVMContext> ctx{act->takeLLVMContext()};
    std::unique_ptr<llvm::Module> module{act->takeModule()};

    if (!module) {
        vsapi->setError(out, "Unable to create module");
    }

    std::string func_name{[&module]{
        for (auto& v: *module) {
            if (v.hasName()) {
                return v.getName().data();
            }
        }
        throw "No user functions found";
    }()};

    // for (auto& v: *module) {
    //     if (v.hasName()) {
    //     printf("%s   ", v.getName().data());
    //     } else {
    //     printf("Unnamed function\n");
    //     }
    // }
    // printf("\n");

    auto jit{ExitOnErr(llvm::orc::LLJITBuilder().create())};

    auto psg{llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
        jit->getDataLayout().getGlobalPrefix())};
    if (!psg) {
        vsapi->setError(out, "Unable to get symbols generator");
        return;
    }
    jit->getMainJITDylib().addGenerator(std::move(*psg));
    llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);

    auto ts_module{llvm::orc::ThreadSafeModule{std::move(module), std::move(ctx)}};
    ExitOnErr(jit->addIRModule(std::move(ts_module)));

    // printf("***\nEndill main: print symbols\n");
    // for (auto& v: J->getMainJITDylib().Symbols) {
    //   printf("%s   ", (*v.first).data());
    // }
    // printf("\n***\n");

    auto s{ExitOnErr(jit->lookupLinkerMangled(jit->getMainJITDylib(), func_name))};
    // auto Main = (int (*)(...))r.getAddress();
    // auto add = static_cast<std::function<int(int, int)>>(s.getAddress());
    data->user_func = reinterpret_cast<int (*)(int, int)>(s.getAddress());
    data->jit = std::move(jit);

    vsapi->createFilter(in, out, "expr_cpp", exprcpp_init, exprcpp_get_frame, exprcpp_free, fmParallel, 0, data, core);
}

VS_EXTERNAL_API(void) VapourSynthPluginInit(VSConfigPlugin config_func, VSRegisterFunction register_func, VSPlugin* plugin)
{
  config_func("com.endill.ExprCpp", "endill", "C++-based Expr",
              VAPOURSYNTH_API_VERSION,1, plugin);
  register_func("expr_cpp", "clip_a:clip;clip_b:clip;code:data",
                exprcpp_create, nullptr, plugin);
  return;
}