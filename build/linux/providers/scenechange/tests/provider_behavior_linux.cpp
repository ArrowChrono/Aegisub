#include "scenechange_provider.h"
#include <dlfcn.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

static_assert(sizeof(scenechange_provider_api) == 72);
static_assert(sizeof(scenechange_provider_frame_buffer) == 128);
static_assert(sizeof(scenechange_provider_result) == 32);

static char error[512]{};
static void check(bool ok, const char* label) {
    if (!ok) { std::fprintf(stderr, "FAIL %s: %s\n", label, error); std::exit(1); }
}
struct Progress { int cancel_at; std::vector<std::pair<int, int>> events; };
static int progress(void* opaque, int processed, int scene) {
    auto& state = *static_cast<Progress*>(opaque);
    state.events.emplace_back(processed, scene);
    return state.cancel_at > 0 && processed >= state.cancel_at;
}
struct Commit { int code; scenechange_provider_result result; };
static Commit solid(const scenechange_provider_api& api, void* ctx, unsigned char luma) {
    scenechange_provider_frame_buffer frame{};
    frame.struct_size = sizeof(frame);
    check(api.get_write_frame(ctx, &frame, error, sizeof(error)) == 0, "get frame");
    check(frame.data != nullptr && frame.pixel_format == 1 && frame.plane_count == 1, "GRAY8 layout");
    std::memset(frame.data, luma, frame.data_size);
    scenechange_provider_result result{};
    result.struct_size = sizeof(result);
    int code = api.commit_written_frame(ctx, &result, error, sizeof(error));
    check(code == 0 || code == 1, "commit frame");
    return {code, result};
}
int main(int argc, char** argv) {
    check(argc == 2, "usage: provider_behavior_linux /path/to/scenechange_wwxd.so");
    void* module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!module) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    auto get_api = reinterpret_cast<scenechange_provider_get_api_fn>(dlsym(module, "scenechange_provider_get_api"));
    check(get_api != nullptr, "export");
    scenechange_provider_api api{}; api.struct_size = sizeof(api);
    check(get_api(0, &api, error, sizeof(error)) == -5, "unsupported API");
    check(get_api(SCENECHANGE_PROVIDER_API_VERSION, nullptr, error, sizeof(error)) == -1, "null API");
    api.struct_size = sizeof(api)-1;
    check(get_api(SCENECHANGE_PROVIDER_API_VERSION, &api, error, sizeof(error)) == -1, "short API");
    api.struct_size = sizeof(api);
    check(get_api(SCENECHANGE_PROVIDER_API_VERSION, &api, error, sizeof(error)) == 0, "get API");
    check(api.backend == 1 && api.input_pixel_format == 1 && api.capabilities == 7, "WWXD capabilities");
    check(api.create(0, 48, nullptr, 0, error, sizeof(error)) == nullptr, "invalid dimensions");
    int options = 0;
    check(api.create(64, 48, &options, sizeof(options), error, sizeof(error)) == nullptr, "reject backend options");
    for (auto dims : std::array<std::pair<int,int>,2>{{{64,48},{65,49}}}) {
        void* ctx = api.create(dims.first, dims.second, nullptr, 0, error, sizeof(error));
        check(ctx != nullptr, "create");
        scenechange_provider_result noframe{}; noframe.struct_size = sizeof(noframe);
        check(api.commit_written_frame(ctx, &noframe, error, sizeof(error)) == -1, "commit without acquire");
        Progress state{2, {}};
        check(api.set_progress_callback(ctx, progress, &state, error, sizeof(error)) == 0, "callback registration");
        const auto first = solid(api, ctx, 32);
        check(first.code == 0 && first.result.frame_index == 0 && first.result.is_scene_change == 1, "first frame cut");
        const auto second = solid(api, ctx, 32);
        check(second.code == 1 && second.result.frame_index == 1 && second.result.is_scene_change == 0, "cancellation result remains valid");
        check(state.events == std::vector<std::pair<int,int>>{{1,1},{2,0}}, "progress sequence");
        check(api.commit_written_frame(ctx, &noframe, error, sizeof(error)) == -1, "canceled frame was consumed");
        state.cancel_at = 0;
        const auto continued = solid(api, ctx, 220);
        check(continued.code == 0 && continued.result.frame_index == 2 && continued.result.is_scene_change == 1 && continued.result.transition_kind == 1 && continued.result.confidence == 1., "real hard cut detection");
        const auto stable = solid(api, ctx, 220);
        check(stable.result.frame_index == 3 && stable.result.is_scene_change == 0 && stable.result.transition_kind == 0 && stable.result.confidence == 0., "stable scene");
        check(api.reset(ctx, error, sizeof(error)) == 0, "reset");
        const auto restarted = solid(api, ctx, 220);
        check(restarted.result.frame_index == 0 && restarted.result.is_scene_change == 1, "reset frame index");
        check(state.events.back() == std::pair<int,int>{1,1}, "reset retains callback and restarts count");
        check(api.set_progress_callback(ctx, nullptr, nullptr, error, sizeof(error)) == 0, "unregister callback");
        const auto count = state.events.size();
        solid(api, ctx, 220);
        check(state.events.size() == count, "unregistered callback stays quiet");
        api.destroy(ctx);
        std::printf("PASS %dx%d: actual cut/stable detection, cancellation consumption, reset, callback lifetime\n", dims.first, dims.second);
    }
    dlclose(module);
    std::puts("PASS ABI errors, dimensions, unsupported options; NativeAOT independent of .NET installation");
}
