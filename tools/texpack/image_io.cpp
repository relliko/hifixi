// PNG through WIC (image_io.hpp)

#include "image_io.hpp"
#include <windows.h>
#include <wincodec.h>
#include <cstdio>

namespace hifi::texpack::image
{
	namespace
	{
		template <class T> struct ref
		{
			T *p = nullptr;
			~ref()
			{
				if (p != nullptr)
					p->Release();
			}
			T **put() { return &p; }
			T *operator->() const { return p; }
		};

		std::string hr_text(const char *what, HRESULT hr)
		{
			char text[128];
			std::snprintf(text, sizeof(text), "%s (0x%08lX)", what, static_cast<unsigned long>(hr));
			return text;
		}

		bool factory(ref<IWICImagingFactory> &f, std::string &why)
		{
			const HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(f.put()));
			if (FAILED(hr))
				why = hr_text("WIC isn't there", hr);
			return SUCCEEDED(hr);
		}
	}

	com_scope::com_scope()
	{
		const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		ok = SUCCEEDED(hr);
	}

	com_scope::~com_scope()
	{
		if (ok)
			CoUninitialize();
	}

	bool read(const std::wstring &path, picture &out, std::string &why)
	{
		ref<IWICImagingFactory> f;
		if (!factory(f, why))
			return false;
		ref<IWICBitmapDecoder> d;
		HRESULT hr = f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, d.put());
		if (FAILED(hr))
		{
			why = hr_text("it can't be opened as a picture", hr);
			return false;
		}
		ref<IWICBitmapFrameDecode> frame;
		ref<IWICFormatConverter> conv;
		UINT w = 0, h = 0;
		if (FAILED(hr = d->GetFrame(0, frame.put())) || FAILED(hr = frame->GetSize(&w, &h)) || FAILED(hr = f->CreateFormatConverter(conv.put())) ||
			FAILED(hr = conv->Initialize(frame.p, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
		{
			why = hr_text("its picture can't be read", hr);
			return false;
		}
		out.w = w, out.h = h;
		out.px.assign(size_t(w) * h * 4, 0);
		if (FAILED(hr = conv->CopyPixels(nullptr, w * 4, UINT(out.px.size()), out.px.data())))
		{
			why = hr_text("its texels can't be read", hr);
			return false;
		}
		return true;
	}

	bool write_png(const std::wstring &path, const uint8_t *px, uint32_t w, uint32_t h, uint32_t channels, std::string &why)
	{
		ref<IWICImagingFactory> f;
		if (!factory(f, why))
			return false;
		// (WIC's PNG encoder takes BGR and BGRA: red and blue swapped here)
		const uint32_t c = channels == 3 ? 3 : 4;
		std::vector<uint8_t> bgr(size_t(w) * h * c);
		for (size_t i = 0; i < size_t(w) * h; ++i)
		{
			const uint8_t *s = px + i * c;
			uint8_t *d = bgr.data() + i * c;
			d[0] = s[2], d[1] = s[1], d[2] = s[0];
			if (c == 4)
				d[3] = s[3];
		}
		ref<IWICStream> stream;
		ref<IWICBitmapEncoder> enc;
		ref<IWICBitmapFrameEncode> frame;
		ref<IPropertyBag2> props;
		HRESULT hr;
		if (FAILED(hr = f->CreateStream(stream.put())) || FAILED(hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)))
		{
			why = hr_text("the file can't be made", hr);
			return false;
		}
		WICPixelFormatGUID format = c == 3 ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
		if (FAILED(hr = f->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.put())) || FAILED(hr = enc->Initialize(stream.p, WICBitmapEncoderNoCache)) ||
			FAILED(hr = enc->CreateNewFrame(frame.put(), props.put())) || FAILED(hr = frame->Initialize(props.p)) || FAILED(hr = frame->SetSize(w, h)) ||
			FAILED(hr = frame->SetPixelFormat(&format)))
		{
			why = hr_text("the PNG encoder can't start", hr);
			return false;
		}
		if (format != (c == 3 ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA))
		{
			why = "the PNG encoder wants another texel format";
			return false;
		}
		if (FAILED(hr = frame->WritePixels(h, w * c, UINT(bgr.size()), bgr.data())) || FAILED(hr = frame->Commit()) || FAILED(hr = enc->Commit()))
		{
			why = hr_text("the PNG can't be written", hr);
			return false;
		}
		return true;
	}
}
