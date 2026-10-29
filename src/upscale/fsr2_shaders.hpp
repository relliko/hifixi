#pragma once
// FSR 2's shaders compiled at run time for its DX12 backend (fsr2_shaders.cpp): why the last one couldn't be, and how
// many have been and how long they took
#include <cstdint>
#include <string>

namespace hifi::upscale::fsr2_shaders
{
	std::string last_error();
	void stats(uint32_t &compiled, double &ms);
}
