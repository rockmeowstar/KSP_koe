#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

class GpuQueueLimit
{
    Microsoft::WRL::ComPtr<IDXGIDevice1> device;
    UINT previous{};
    bool armed{}, changed{};

  public:
    GpuQueueLimit() = default;
    GpuQueueLimit(const GpuQueueLimit &) = delete;
    GpuQueueLimit &operator=(const GpuQueueLimit &) = delete;
    ~GpuQueueLimit() { Disable(); }

    void Enable(ID3D11Device *source)
    {
        if (armed || !source) return;
        armed = true;
        if (SUCCEEDED(source->QueryInterface(IID_PPV_ARGS(&device))) &&
            SUCCEEDED(device->GetMaximumFrameLatency(&previous)) && previous != 1)
            changed = SUCCEEDED(device->SetMaximumFrameLatency(1));
    }

    void Disable()
    {
        UINT current{};
        // AI-generated comment: Restore only our own value; preserve a later change by another mod.
        if (changed && SUCCEEDED(device->GetMaximumFrameLatency(&current)) && current == 1)
            device->SetMaximumFrameLatency(previous);
        device.Reset();
        armed = changed = false;
    }
};
