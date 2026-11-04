#pragma once

// upscale.py (research\ffxi-re\tools\upscale) in C++ for hifixi-texpack make: each texture's colour wrapped by kPad
// texels (textures tile, so no seam at the edges), Real-ESRGAN x4plus run on a folder of them as a child process, the
// result cropped back; for 2x brought down with Lanczos, and the alpha enlarged with Lanczos (Pillow's Image.resize
// LANCZOS, in the same fixed point, so a picture comes out as the script's).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hifi::texpack::upscale
{
	constexpr uint32_t kPad = 8;   // (upscale.py's PAD)
	constexpr uint32_t kScale = 4; // (the x4plus model's factor)

	// The RGB of an RGBA8 picture with pad texels of wrap around it: (w + 2 pad) x (h + 2 pad) x 3 (numpy.pad 'wrap')
	std::vector<uint8_t> wrap_pad_rgb(const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t pad = kPad);
	// w x h texels of a W-wide picture of c channels from (x0, y0)
	std::vector<uint8_t> crop(const uint8_t *px, uint32_t W, uint32_t c, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h);
	// Pillow's resize with Image.LANCZOS (a = 3, the support widened when shrinking; horizontal then vertical, each
	// pass to 8 bits in its 22-bit fixed point; the edges clamped) on c channels
	std::vector<uint8_t> lanczos(const uint8_t *px, uint32_t w, uint32_t h, uint32_t c, uint32_t W, uint32_t H);
	// One texture after the upscaler: orig RGBA8 w x h, esrgan its padded picture as the upscaler wrote it (RGBA8,
	// ((w + 2 pad) * 4) x ((h + 2 pad) * 4)), factor 2 or 4 -> RGBA8 (w * factor) x (h * factor): the colour cropped
	// (and for 2x Lanczos'd down), the alpha the original's Lanczos'd up. false (why) when the sizes don't fit
	bool finish(const uint8_t *orig, uint32_t w, uint32_t h, const uint8_t *esrgan, uint32_t ew, uint32_t eh, uint32_t factor, std::vector<uint8_t> &out,
		std::string &why);

	// realesrgan-ncnn-vulkan.exe, its vcomp140.dll and its x4plus model in a folder: "" when they're all there, else
	// what's missing
	std::string missing_files(const std::wstring &folder);

	// How a run of the upscaler ended
	enum class end
	{
		done,    // exit 0
		failed,  // another exit code, or it couldn't be started (why)
		ended,   // ended by the caller (should_end said why)
		stalled, // no new picture for stall_s: ended
	};

	struct run_options
	{
		std::wstring folder;  // the upscaler's (missing_files "")
		std::wstring in, out; // the batch's folders
		std::wstring log;     // its console output, appended ("": none)
		double poll_s = 3.0, stall_s = 900.0;
		// asked every poll_s while it runs: "" to go on, else why to end it now
		std::function<std::string()> should_end;
	};

	// The upscaler run on a folder (realesrgan-ncnn-vulkan -i in -o out -n realesrgan-x4plus -m models -f png, as
	// upscale.py), at below-normal priority, in a job object that ends it when the job's handle closes: when this
	// returns, and when the builder itself ends, however it ends. why: the exit code or what ended it
	end run(const run_options &o, std::string &why);
}
