#pragma once
#include <windows.h>
#include <psapi.h>
#include <wrl/client.h>
#include <dxgi.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <stdexcept>
#include "../../third_party/reshade-api/reshade_events.hpp"

// AI-generated comment: Official ReShade API 20 (SDK v6.8.0). No module names, addresses or vtable
// offsets identify ReShade; its exported API and version handshake do.
namespace ReShadeIntegration
{
using AfterPresent = void (*)(IDXGISwapChain *, HWND);
inline std::atomic<bool> active{};
inline std::atomic<HWND> ownWindow{};
inline std::mutex gate;
inline Microsoft::WRL::ComPtr<IDXGISwapChain> ownNativeSwap;
inline AfterPresent after{};
inline void Init(reshade::api::effect_runtime *runtime)
{
    if (runtime->get_device()->get_api() != reshade::api::device_api::d3d11)
        return;
    if (runtime->get_hwnd() != ownWindow.load())
        return;
    std::lock_guard<std::mutex> lock(gate);
    ownNativeSwap = reinterpret_cast<IDXGISwapChain *>(runtime->get_native());
}
inline void Destroy(reshade::api::effect_runtime *runtime)
{
    if (runtime->get_hwnd() != ownWindow.load())
        return;
    std::lock_guard<std::mutex> lock(gate);
    ownNativeSwap.Reset();
}
inline void Present(reshade::api::effect_runtime *runtime)
{
    if (runtime->get_device()->get_api() != reshade::api::device_api::d3d11)
        return;
    HWND window = static_cast<HWND>(runtime->get_hwnd());
    if (window != ownWindow.load() && after)
        after(reinterpret_cast<IDXGISwapChain *>(runtime->get_native()), window);
}
inline bool Start(HMODULE self, AfterPresent callback)
{
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), nullptr, 0, &needed))
        throw std::runtime_error("ReShade module enumeration failed");
    std::vector<HMODULE> modules(needed / sizeof(HMODULE));
    if (!K32EnumProcessModules(GetCurrentProcess(), modules.data(), needed, &needed))
        throw std::runtime_error("ReShade module enumeration failed");
    if (needed > modules.size() * sizeof(HMODULE))
        throw std::runtime_error("Module inventory changed during ReShade detection");
    for (HMODULE module : modules)
    {
        auto add = reinterpret_cast<bool (*)(void *, unsigned)>(GetProcAddress(module, "ReShadeRegisterAddon"));
        auto event =
            reinterpret_cast<void (*)(reshade::addon_event, void *)>(GetProcAddress(module, "ReShadeRegisterEvent"));
        if (!add || !event)
            continue;
        if (!add(self, 20))
            throw std::runtime_error(
                "ReShade API 20 integration unavailable; frame generation disabled to preserve its output");
        after = callback;
        event(reshade::addon_event::init_effect_runtime, reinterpret_cast<void *>(&Init));
        event(reshade::addon_event::destroy_effect_runtime, reinterpret_cast<void *>(&Destroy));
        event(reshade::addon_event::reshade_present, reinterpret_cast<void *>(&Present));
        active = true;
        return true;
    }
    return false;
}
inline Microsoft::WRL::ComPtr<IDXGISwapChain> NativeSwap()
{
    std::lock_guard<std::mutex> lock(gate);
    return ownNativeSwap;
}
}
