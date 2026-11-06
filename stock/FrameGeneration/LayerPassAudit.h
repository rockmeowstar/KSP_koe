#pragma once
#ifdef KOE_CAPTURE_AUDIT
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <vector>

namespace LayerPassAudit
{
    using SetDepthFn = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11DepthStencilState *, UINT);
    using ClearDepthFn = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11DepthStencilView *, UINT, FLOAT, UINT8);
    inline SetDepthFn originalSet{};
    inline ClearDepthFn originalClear{};
    inline std::atomic<ID3D11DeviceContext *> observed{};
    inline std::mutex mutex;
    inline std::ofstream output;
    inline std::set<std::pair<std::uintptr_t, UINT>> states;
    inline std::set<std::tuple<unsigned, unsigned, unsigned>> clears;
    inline std::vector<Microsoft::WRL::ComPtr<ID3D11DepthStencilState>> retained;
    inline bool hooksReady{};

    inline void Face(const D3D11_DEPTH_STENCILOP_DESC &face)
    {
        output << "{\"fail\":" << unsigned(face.StencilFailOp)
               << ",\"depthFail\":" << unsigned(face.StencilDepthFailOp)
               << ",\"pass\":" << unsigned(face.StencilPassOp)
               << ",\"function\":" << unsigned(face.StencilFunc) << "}";
    }

    inline void STDMETHODCALLTYPE SetDepth(ID3D11DeviceContext *context, ID3D11DepthStencilState *state, UINT reference)
    {
        originalSet(context, state, reference);
        if (context != observed.load(std::memory_order_relaxed) || !state) return;
        try
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!output || states.size() >= 4096 || !states.emplace(reinterpret_cast<std::uintptr_t>(state), reference).second) return;
            retained.emplace_back(state);
            D3D11_DEPTH_STENCIL_DESC desc{};
            state->GetDesc(&desc);
            output << "{\"event\":\"depthState\",\"depthEnabled\":" << bool(desc.DepthEnable)
                   << ",\"depthWrite\":" << unsigned(desc.DepthWriteMask)
                   << ",\"depthFunction\":" << unsigned(desc.DepthFunc)
                   << ",\"stencilEnabled\":" << bool(desc.StencilEnable)
                   << ",\"readMask\":" << unsigned(desc.StencilReadMask)
                   << ",\"writeMask\":" << unsigned(desc.StencilWriteMask)
                   << ",\"reference\":" << reference << ",\"front\":";
            Face(desc.FrontFace);output << ",\"back\":";Face(desc.BackFace);output << "}\n";output.flush();
        }
        catch (...) { }
    }

    inline void STDMETHODCALLTYPE ClearDepth(ID3D11DeviceContext *context, ID3D11DepthStencilView *view,
                                             UINT flags, FLOAT depth, UINT8 stencil)
    {
        originalClear(context, view, flags, depth, stencil);
        if (context != observed.load(std::memory_order_relaxed) || !view) return;
        try
        {
            D3D11_DEPTH_STENCIL_VIEW_DESC desc{};view->GetDesc(&desc);
            std::lock_guard<std::mutex> lock(mutex);
            if (!output || !clears.emplace(unsigned(desc.Format), flags, unsigned(stencil)).second) return;
            output << "{\"event\":\"clearDepth\",\"format\":" << unsigned(desc.Format)
                   << ",\"flags\":" << flags << ",\"depth\":" << depth
                   << ",\"stencil\":" << unsigned(stencil) << "}\n";output.flush();
        }
        catch (...) { }
    }

    inline void Start(ID3D11DeviceContext *context, const std::filesystem::path &root, bool force = false)
    {
        if ((!force && !GetEnvironmentVariableA("KOE_LAYER_PASS_AUDIT", nullptr, 0)) || root.empty()) return;
        std::lock_guard<std::mutex> lock(mutex);
        if (originalSet) { if (hooksReady) observed.store(context, std::memory_order_relaxed);return; }
        std::error_code directoryError;
        std::filesystem::create_directories(root,directoryError);
        if (directoryError) return;
        output.open(root / "layer-pass-states.jsonl");
        if (!output) return;
        auto table = *reinterpret_cast<void ***>(context);
        void *setTarget = table[36], *clearTarget = table[53];
        auto createdSet = MH_CreateHook(setTarget, reinterpret_cast<void *>(SetDepth), reinterpret_cast<void **>(&originalSet));
        auto createdClear = MH_CreateHook(clearTarget, reinterpret_cast<void *>(ClearDepth), reinterpret_cast<void **>(&originalClear));
        if (createdSet != MH_OK || createdClear != MH_OK)
        {
            if (createdSet == MH_OK) MH_RemoveHook(setTarget);
            if (createdClear == MH_OK) MH_RemoveHook(clearTarget);
            originalSet = nullptr;originalClear = nullptr;
            output << "{\"event\":\"unavailable\",\"setStatus\":" << int(createdSet)
                   << ",\"clearStatus\":" << int(createdClear) << "}\n";output.flush();return;
        }
        auto enabledSet = MH_EnableHook(setTarget), enabledClear = MH_EnableHook(clearTarget);
        output << "{\"event\":\"start\",\"setStatus\":" << int(enabledSet)
               << ",\"clearStatus\":" << int(enabledClear) << "}\n";output.flush();
        hooksReady = enabledSet == MH_OK && enabledClear == MH_OK;
        if (hooksReady) observed.store(context, std::memory_order_relaxed);
    }

    inline void Stop()
    {
        observed.store(nullptr, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(mutex);
        if (output) output.flush();
    }
}
#endif
