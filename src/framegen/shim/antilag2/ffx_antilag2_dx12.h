#pragma once
// HiFi XI: the FidelityFX SDK's frame interpolation swap chain tells AMD's Anti-Lag 2 driver extension which frames are
// interpolated, when the game has handed it an Anti-Lag 2 context (private data on the swap chain). HiFi XI never does,
// and AMD's header checks its driver structures' sizes for 64 bits: what the swap chain names, doing nothing.
namespace AMD::AntiLag2DX12
{
	struct Context
	{
	};
	inline long SetFrameGenFrameType(Context *, bool) { return 0; }
}
