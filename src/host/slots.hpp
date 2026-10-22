#pragma once

// (slots.cpp: compiled with CINTERFACE, so it shares only plain types with the rest)
#include <cstdint>
#include "hooks.hpp"

namespace hifi::host
{
	// Each hooked method's vtable index, by hooks.hpp's slot order
	void slot_indices(uint32_t out[slot_count]);
	// The command list vtable's length (ID3D12GraphicsCommandList7), for the slot index sanity check
	uint32_t list_vtable_slots();
}
