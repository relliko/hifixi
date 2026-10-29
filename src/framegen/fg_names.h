#pragma once
// Force-included (/FI) in the FidelityFX SDK's frame generation code (src/framegen/ffx) and in HiFi XI's own frame
// generation code: the SDK's functions that FSR 2's own copy (src/upscale/fsr2, an older FidelityFX) also defines, under
// other names here, so both link into one plugin. (Found with dumpbin: the C ones by name, the C++ ones by their
// signatures, which differ only in structs of the same name.)
#define ffxAssertReport ffxfg_AssertReport
#define ffxAssertSetPrintingCallback ffxfg_AssertSetPrintingCallback
#define ffxGetCommandListDX12 ffxfg_GetCommandListDX12
#define ffxGetDeviceDX12 ffxfg_GetDeviceDX12
#define ffxGetResourceDX12 ffxfg_GetResourceDX12
#define addBarrier ffxfg_addBarrier
#define ffxGetDX12ResourceFlags ffxfg_GetDX12ResourceFlags
#define ffxGetDX12StateFromResourceState ffxfg_GetDX12StateFromResourceState
#define flushBarriers ffxfg_flushBarriers
#define getDX12ResourcePtr ffxfg_getDX12ResourcePtr
#define TIF ffxfg_TIF
#define BackendContext_DX12 ffxfg_BackendContext_DX12
