#pragma once
#include <d3d11.h>
#include <wrl/client.h>

class BlendFactorUsage
{
  public:
    bool Used(ID3D11BlendState *state)
    {
        if (known && cached.Get() == state) return used;
        cached = state;
        known = true;
        used = false;
        if (!state) return false;
        D3D11_BLEND_DESC description{};
        state->GetDesc(&description);
        unsigned targets = description.IndependentBlendEnable ? D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT : 1;
        for (unsigned i = 0; i < targets; ++i)
        {
            const auto &target = description.RenderTarget[i];
            if (target.BlendEnable && (Factor(target.SrcBlend) || Factor(target.DestBlend) ||
                Factor(target.SrcBlendAlpha) || Factor(target.DestBlendAlpha)))
            {
                used = true;
                break;
            }
        }
        return used;
    }

  private:
    static bool Factor(D3D11_BLEND value)
    {
        return value == D3D11_BLEND_BLEND_FACTOR || value == D3D11_BLEND_INV_BLEND_FACTOR;
    }
    Microsoft::WRL::ComPtr<ID3D11BlendState> cached;
    bool known = false, used = false;
};
