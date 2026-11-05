#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wrl/client.h>
#include <cstddef>

// AI-generated comment: Diagnostic renderer core. Caller guarantees identical geometry, render state,
// resources and shader variant, and consecutive original light draw order.
// Each source retains the original shader's full constants, including matrices.
class InstancedLightBatch final
{
  public:
    static constexpr unsigned VectorsPerSource = 128, MaximumSources = 32, Slot = 13;
    struct alignas(16) Record
    {
        float vectors[VectorsPerSource][4]{};
    };
    HRESULT Initialize(ID3D11Device *, const void *vs, size_t vsSize, const void *ps, size_t psSize);
    HRESULT Draw(ID3D11DeviceContext *, const Record *, unsigned count, unsigned indices, unsigned startIndex = 0,
                 int baseVertex = 0);

  private:
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants;
};
