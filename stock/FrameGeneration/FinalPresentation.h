#pragma once
#include <dxgi.h>

namespace FinalPresentation {
// The image already contains composition and UI. Enter the public Present
// dispatch here so external OSD hooks run on that final image. A saved DXGI
// trampoline can bypass hooks installed after KOE. With ReShade, dispatch on
// its exposed native chain to avoid applying its effects/menu a second time.
inline HRESULT Present(IDXGISwapChain* chain,IDXGISwapChain* nativeChain,UINT interval,UINT flags){
    IDXGISwapChain* target=nativeChain?nativeChain:chain;
    return target?target->Present(interval,flags):E_POINTER;
}
}
