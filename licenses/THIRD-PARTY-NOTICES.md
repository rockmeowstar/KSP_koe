# Third-party notices — KerbalOverEngineering

## NVIDIA Optical Flow

KOE calls the NVIDIA Optical Flow API exported by the installed graphics driver's
System32 `nvofapi64.dll`. It uses unmodified `nvOpticalFlowCommon.h` and
`nvOpticalFlowD3D11.h` from Optical Flow SDK 5.0.7. Their explicit header-only
MIT notices are preserved in `NVIDIA-Optical-Flow-Headers-MIT.txt` and in source.
The SDK's separate-component terms apply to those separately licensed headers.

No NVIDIA SDK binary, sample application, FRUC DLL, CUDA library, neural-network
model, NVIDIA App component or driver installer is included. The Optical Flow
bridge/interpolation/UI protection and capability-probe implementations are KOE
code. This product is independent and is not sponsored or endorsed by NVIDIA.
NVIDIA names identify interoperability; trademarks remain their owners' property.

The user needs a compatible NVIDIA GPU and installed driver. The mod checks the
actual adapter/API and enables the option when supported. It does not accept a
driver EULA for the user, install a driver or change driver settings.

Official references:
- https://docs.nvidia.com/video-technologies/optical-flow-sdk/license/index.html
- https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-application-note/

## MinHook and HDE

MinHook: Copyright (C) 2009-2017 Tsuda Kageyu.
HDE portions: Copyright (c) 2008-2009 Vyacheslav Patkov.
BSD-2-Clause notices/disclaimers are reproduced in `MinHook-LICENSE.txt`.
They remain applicable to the native modules containing that code.

## Deferred point-light shader adaptation

Original Deferred: Copyright (c) 2024-2025 Ghassen Lahmar.
Upstream: https://github.com/LGhassen/Deferred/tree/1.3.3.0
License: GPL version 3; exact upstream license is in `Deferred-GPL-3.0.txt`.

KOE changes constant-buffer addressing/signatures to retain individual sources
in instanced draws, preserving original lighting instruction order. The modified
shader assets and the plugin binary containing them are supplied with GPL-3.0
redistribution conditions. The KOE-authored source remains MIT-compatible and
is included in the corresponding-source archive.

`source/InstancedPointLights-corresponding-source.zip` is included with the binary
package. It contains the modified shader assembly, signature/reflection inputs,
generation/build scripts, native/managed integration source and the exact upstream
Deferred 1.3.3.0 source archive. No written-offer-only substitution is used.
Changes are dated 2026-11-04. No original Deferred mod DLL or asset bundle is
installed by this package; the original installed mod remains a dependency.

## EVE compatibility

EVE's public code attribution: Ryan Bray (2013), Warwick Allison (2018-2019),
and upstream contributors including Ghassen Lahmar.
https://github.com/LGhassen/EnvironmentalVisualEnhancements/tree/release-5
Its public code's MIT notice is reproduced in `EVE-public-code-MIT.txt`.

No complete EVE shader or paid cloud content is included. The cloud-guard module
ships two small operation recipes. It uses source bytecode supplied by the user's
installed EVE at shader creation, verifies source and reconstructed output hashes,
and constructs the accepted guarded shader in memory. The original bundle stays
unchanged. Unknown source content retains original rendering.

## Other dependencies

KSP/Unity, Harmony, Firefly/API, Parallax, Kopernicus, Scatterer and original mod
assets remain separately installed dependencies under their own terms. This
package does not relicense or redistribute their original DLLs, game content or
asset bundles. See `docs/Options-and-dependencies.md` for the per-feature list.

The MIT grant covers KOE-owned material only. Videos/game screenshots retain
underlying game/mod asset rights; source/tool archives under third_party/tools
retain their original notices.
