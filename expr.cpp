#include <array>
#include <functional>
#include <memory>
#include <string>

#include <llvm-10/llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm-10/llvm/Support/ManagedStatic.h>

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