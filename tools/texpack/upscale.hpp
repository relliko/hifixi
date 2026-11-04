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

	// The models make can run (--upscaler-model), all 4x: hdcube (4x_HDCube_200k, Venomalia, CC0-1.0: ships with the
	// tool in models-hdcube\, get-upscaler copies it into the upscaler's models\; the default since M8) and x4plus
	// (Real-ESRGAN's, from the official zip)
	struct model
	{
		const char *id;     // --upscaler-model's word
		const wchar_t *name; // -n: models\<name>.param and .bin
		uint8_t number;     // (in make's maker version: 0 x4plus, 1 hdcube)
	};
	constexpr model kModels[] = { { "hdcube", L"realesrgan-hdcube-x4", 1 }, { "x4plus", L"realesrgan-x4plus", 0 } };
	// The model with this id; nullptr when there's none
	const model *find_model(const std::string &id);

	// realesrgan-ncnn-vulkan.exe, its vcomp140.dll and a model (by its -n name) in a folder: "" when they're all there,
	// else what's missing
	std::string missing_files(const std::wstring &folder, const wchar_t *model_name = L"realesrgan-x4plus");

	// How a run of the upscaler ended
	enum class end
	{
		done,    // exit 0
		failed,  // another exit code, or it couldn't be started (why)
		ended,   // ended by the caller (should_end said why)
		stalled, // no new picture for stall_s: ended
	};

	// A child program run to its end: what run_program watches
	struct program_options
	{
		std::wstring log;            // its console output, appended ("": none)
		double poll_s = 3.0, stall_s = 900.0;
		std::function<std::string()> should_end; // as run_options'
		std::wstring watch, pattern; // the folder its output files appear in, and their pattern (the stall check)
	};
	// exe run with cmd in folder cwd, at below-normal priority, in a job object that ends it when the job's handle
	// closes (when this returns, and when the builder itself ends, however it ends); polled every poll_s: should_end,
	// and no new file matching pattern in watch for stall_s counts as stalled. why: the exit code or what ended it
	end run_program(const std::wstring &exe, std::wstring cmd, const std::wstring &cwd, const program_options &o, std::string &why);

	struct run_options
	{
		std::wstring folder;  // the upscaler's (missing_files "")
		const wchar_t *model = L"realesrgan-x4plus"; // -n (kModels' name)
		std::wstring in, out; // the batch's folders
		std::wstring log;     // its console output, appended ("": none)
		double poll_s = 3.0, stall_s = 900.0;
		// asked every poll_s while it runs: "" to go on, else why to end it now
		std::function<std::string()> should_end;
	};

	// The upscaler run on a folder (realesrgan-ncnn-vulkan -i in -o out -n <model> -m models -s 4 -f png, as
	// upscale.py) by run_program; the stall check counts its PNGs
	end run(const run_options &o, std::string &why);
}
