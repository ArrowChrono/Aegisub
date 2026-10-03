#include <dlfcn.h>

#include "scenechange_provider.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

static_assert(sizeof(void *) == 8);
static_assert(sizeof(scenechange_provider_plane) == 24);
static_assert(offsetof(scenechange_provider_plane, stride) == 8);
static_assert(offsetof(scenechange_provider_plane, height) == 16);
static_assert(sizeof(scenechange_provider_frame_buffer) == 128);
static_assert(offsetof(scenechange_provider_frame_buffer, data) == 16);
static_assert(offsetof(scenechange_provider_frame_buffer, planes) == 32);
static_assert(SCENECHANGE_PROVIDER_FRAME_BUFFER_V1_SIZE == 128);
static_assert(sizeof(scenechange_provider_result) == 32);
static_assert(offsetof(scenechange_provider_result, frame_index) == 8);
static_assert(offsetof(scenechange_provider_result, confidence) == 24);
static_assert(SCENECHANGE_PROVIDER_RESULT_V1_SIZE == 32);
static_assert(sizeof(scenechange_provider_api) == 72);
static_assert(offsetof(scenechange_provider_api, create) == 24);
static_assert(offsetof(scenechange_provider_api, commit_written_frame) == 64);
static_assert(SCENECHANGE_PROVIDER_API_V1_SIZE == 72);

namespace {

template<typename T>
struct Extended final {
    T value;
    unsigned char tail[16];
};

template<typename T>
bool HasUntouchedTail(const Extended<T> &extended) {
    for (const unsigned char value : extended.tail) {
        if (value != 0xA5u)
            return false;
    }
    return true;
}

int Fail(const char *message, const char *detail = nullptr) {
    if (detail != nullptr && detail[0] != '\0')
        std::fprintf(stderr, "%s: %s\n", message, detail);
    else
        std::fprintf(stderr, "%s\n", message);
    return 1;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2)
        return Fail("usage: provider_abi_smoke <scenechange_wwxd.so>");

    void *module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (module == nullptr)
        return Fail("dlopen failed");

    const auto get_api = reinterpret_cast<scenechange_provider_get_api_fn>(
        dlsym(module, "scenechange_provider_get_api"));
    if (get_api == nullptr) {
        dlclose(module);
        return Fail("scenechange_provider_get_api export is missing");
    }

    char error[512]{};
    Extended<scenechange_provider_api> api_storage;
    std::memset(&api_storage, 0xA5, sizeof(api_storage));
    api_storage.value.struct_size = static_cast<uint32_t>(sizeof(api_storage));
    int code = get_api(SCENECHANGE_PROVIDER_API_VERSION, &api_storage.value, error, sizeof(error));
    const scenechange_provider_api &api = api_storage.value;
    if (code != SCENECHANGE_PROVIDER_OK ||
        api.struct_size != SCENECHANGE_PROVIDER_API_V1_SIZE ||
        !HasUntouchedTail(api_storage) ||
        api.api_version != SCENECHANGE_PROVIDER_API_VERSION ||
        api.backend != SCENECHANGE_PROVIDER_BACKEND_WWXD ||
        api.input_pixel_format != SCENECHANGE_PROVIDER_PIXEL_FORMAT_GRAY8_PADDED16 ||
        api.create == nullptr || api.destroy == nullptr || api.reset == nullptr ||
        api.set_progress_callback == nullptr || api.get_write_frame == nullptr ||
        api.commit_written_frame == nullptr) {
        dlclose(module);
        return Fail("provider API table validation failed", error);
    }

    void *context = api.create(65, 49, nullptr, 0, error, sizeof(error));
    if (context == nullptr) {
        dlclose(module);
        return Fail("provider create failed", error);
    }

    Extended<scenechange_provider_frame_buffer> frame_storage;
    std::memset(&frame_storage, 0xA5, sizeof(frame_storage));
    frame_storage.value.struct_size = static_cast<uint32_t>(sizeof(frame_storage));
    code = api.get_write_frame(context, &frame_storage.value, error, sizeof(error));
    const scenechange_provider_frame_buffer &frame = frame_storage.value;
    if (code != SCENECHANGE_PROVIDER_OK ||
        frame.struct_size != SCENECHANGE_PROVIDER_FRAME_BUFFER_V1_SIZE ||
        !HasUntouchedTail(frame_storage) ||
        frame.pixel_format != SCENECHANGE_PROVIDER_PIXEL_FORMAT_GRAY8_PADDED16 ||
        frame.plane_count != 1 || frame.data == nullptr ||
        frame.planes[0].data != frame.data || frame.planes[0].width != 80 ||
        frame.planes[0].height != 64 || frame.planes[0].stride < 80 ||
        frame.data_size != static_cast<size_t>(frame.planes[0].stride) * 64u) {
        api.destroy(context);
        dlclose(module);
        return Fail("owned frame layout validation failed", error);
    }

    scenechange_provider_frame_buffer duplicate{};
    duplicate.struct_size = static_cast<uint32_t>(sizeof(duplicate));
    code = api.get_write_frame(context, &duplicate, error, sizeof(error));
    if (code != SCENECHANGE_PROVIDER_INVALID_ARGUMENT) {
        api.destroy(context);
        dlclose(module);
        return Fail("duplicate write-frame acquisition was not rejected", error);
    }

    std::memset(frame.data, 0, frame.data_size);
    Extended<scenechange_provider_result> result_storage;
    std::memset(&result_storage, 0xA5, sizeof(result_storage));
    result_storage.value.struct_size = static_cast<uint32_t>(sizeof(result_storage));
    code = api.commit_written_frame(context, &result_storage.value, error, sizeof(error));
    const scenechange_provider_result &result = result_storage.value;
    if (code != SCENECHANGE_PROVIDER_OK ||
        result.struct_size != SCENECHANGE_PROVIDER_RESULT_V1_SIZE ||
        !HasUntouchedTail(result_storage) ||
        result.frame_index != 0) {
        api.destroy(context);
        dlclose(module);
        return Fail("frame commit validation failed", error);
    }

    code = api.reset(context, error, sizeof(error));
    if (code != SCENECHANGE_PROVIDER_OK) {
        api.destroy(context);
        dlclose(module);
        return Fail("provider reset failed", error);
    }

    api.destroy(context);
    dlclose(module);
    return 0;
}
