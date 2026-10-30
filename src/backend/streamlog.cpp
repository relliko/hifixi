#include <d3d12.h>
#include "streamlog.hpp"
#include "../game/slotmark.hpp"
#include "../host/hooks.hpp"
#include <share.h>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// The stream log (streamlog.hpp). While one is being made, the hooks hand over every call (on_call, on dgVoodoo's
// threads, and the game thread's at readbacks); each is written out as a line as it comes, under one lock, which slows
// the frame being logged and no other. The game's Direct3D 8 draws come in on the game thread alone (d3d8_draw).

namespace hifi::backend::streamlog
{
	namespace
	{
		constexpr uint32_t kMaxFrames = 8;
		constexpr size_t kMaxBytes = size_t(48) << 20; // (a world frame is about 0.5-2 MB of lines: past this, cut short)
		constexpr size_t kRing = 64;                   // calls kept while waiting for a world frame's start
		constexpr ULONGLONG kMaxMs = 15000;            // a log never runs longer

		enum class phase
		{
			idle,
			asked,     // the request, until the next present
			waiting,   // traced, for a world frame's start
			recording,
			done,      // the frames are in (or none came): written at the next present after the game's frames end
		};

		std::mutex mutex; // all of it, but the game's draws (the game thread's alone)
		phase now = phase::idle;
		std::atomic<bool> active { false }; // (not idle: present() looks no further without it)
		uint32_t frames_wanted = 1, frames_seen = 0;
		std::string label, context, ended;
		std::wstring folder;
		void *(*views)(uint64_t) = nullptr;
		uint32_t wait_ms = 5000;
		DWORD game_thread = 0;
		LARGE_INTEGER started = {}, frequency = {};
		ULONGLONG armed_at = 0;
		std::string text; // the Direct3D 12 calls, a line each
		uint32_t index = 0;
		// Each line's call as the totals need it, and where the line is in text
		struct record
		{
			uint32_t slot = 0, list = 0; // (list: its id, for a list's calls)
			bool quiet = false;
			uint64_t vertices = 0;           // (draws)
			std::string rt, ds;              // (OMSetRenderTargets)
			std::vector<uint32_t> submitted; // (ExecuteCommandLists: the lists' ids)
			size_t at = 0;
		};
		std::vector<record> records;
		struct kept
		{
			double us;
			std::string body;
			record r;
		};
		std::vector<kept> ring; // (while waiting: the last calls)

		// Names: lists L1.., queues Q1.., resources R1.., pipelines P1.., root signatures S1.., threads t1.. ("game": the
		// game's own)
		std::unordered_map<const void *, uint32_t> list_ids, queue_ids, resource_ids, pso_ids, root_ids;
		std::unordered_map<DWORD, uint32_t> thread_ids;

		// The totals over the frames logged (written with the file: dgVoodoo's calls, HiFi XI's own left out)
		struct list_totals
		{
			std::string rt = "?", ds = "?"; // (bound since the log started; "?" before)
			uint64_t resets = 0, closes = 0, submitted = 0, draws = 0, vertices = 0;
		};
		struct target_totals
		{
			uint64_t draws = 0, indexed = 0, vertices = 0;
		};

		// The game's Direct3D 8 draws (the game thread)
		std::atomic<bool> d3d8_recording { false };
		// The tap (set_d3d8_tap): set by the plugin, and what each slot held
		bool (*tap_on_fn)(const void *const taps[4], void *originals[4], std::string &why) = nullptr;
		void (*tap_off_fn)() = nullptr;
		void *tap_originals[4] = {};
		extern const void *const kTaps[4];
		uint32_t d3d8_presents = 0;
		std::string d3d8;
		uint32_t d3d8_index = 0;
		std::map<std::string, uint64_t> d3d8_sites;
		struct module_info
		{
			const uint8_t *base = nullptr;
			uint32_t code_from = 0, code_to = 0;
			bool looked = false;
		} ffximain;

		void add(std::string &s, const char *format, ...)
		{
			char buffer[1024];
			va_list a;
			va_start(a, format);
			const int n = std::vsnprintf(buffer, sizeof(buffer), format, a);
			va_end(a);
			if (n > 0)
				s.append(buffer, size_t(std::min(n, int(sizeof(buffer) - 1))));
		}

		double micros()
		{
			LARGE_INTEGER t;
			QueryPerformanceCounter(&t);
			return frequency.QuadPart != 0 ? double(t.QuadPart - started.QuadPart) * 1e6 / double(frequency.QuadPart) : 0.0;
		}

		template <typename T>
		const T *ptr(uint64_t v) { return reinterpret_cast<const T *>(uintptr_t(v)); }

		uint32_t id_of(std::unordered_map<const void *, uint32_t> &m, const void *p)
		{
			const auto it = m.find(p);
			if (it != m.end())
				return it->second;
			const uint32_t n = uint32_t(m.size()) + 1;
			m.emplace(p, n);
			return n;
		}

		// (only of resources the call itself uses: they're alive)
		std::string resource(const void *r)
		{
			if (r == nullptr)
				return "null";
			const uint32_t n = id_of(resource_ids, r);
			const D3D12_RESOURCE_DESC d = static_cast<ID3D12Resource *>(const_cast<void *>(r))->GetDesc();
			char s[96];
			if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
				std::snprintf(s, sizeof(s), "R%u(buffer %llu)", n, static_cast<unsigned long long>(d.Width));
			else if (d.SampleDesc.Count > 1)
				std::snprintf(s, sizeof(s), "R%u(%llux%u f%u x%u)", n, static_cast<unsigned long long>(d.Width), d.Height, unsigned(d.Format), d.SampleDesc.Count);
			else
				std::snprintf(s, sizeof(s), "R%u(%llux%u f%u)", n, static_cast<unsigned long long>(d.Width), d.Height, unsigned(d.Format));
			return s;
		}

		std::string view(uint64_t v)
		{
			if (v == 0)
				return "none";
			if (void *const r = views != nullptr ? views(v) : nullptr)
				return resource(r);
			char s[48];
			std::snprintf(s, sizeof(s), "view-0x%llX", static_cast<unsigned long long>(v));
			return s;
		}

		std::string named(std::unordered_map<const void *, uint32_t> &m, char letter, uint64_t p)
		{
			if (p == 0)
				return "none";
			char s[16];
			std::snprintf(s, sizeof(s), "%c%u", letter, id_of(m, ptr<void>(p)));
			return s;
		}

		std::string thread_name()
		{
			const DWORD t = GetCurrentThreadId();
			if (t == game_thread)
				return "game";
			const auto it = thread_ids.find(t);
			const uint32_t n = it != thread_ids.end() ? it->second : thread_ids.emplace(t, uint32_t(thread_ids.size()) + 1).first->second;
			return "t" + std::to_string(n);
		}

		const char *topology_name(uint64_t t)
		{
			switch (t)
			{
			case 1: return " (points)";
			case 2: return " (lines)";
			case 3: return " (line strip)";
			case 4: return " (triangles)";
			case 5: return " (triangle strip)";
			default: return "";
			}
		}

		const char *primitive_name(uint32_t t)
		{
			switch (t)
			{
			case 1: return "POINTLIST";
			case 2: return "LINELIST";
			case 3: return "LINESTRIP";
			case 4: return "TRIANGLELIST";
			case 5: return "TRIANGLESTRIP";
			case 6: return "TRIANGLEFAN";
			default: return "?";
			}
		}

		void rects_text(std::string &b, uint64_t n, const D3D12_RECT *r)
		{
			if (n == 0 || r == nullptr)
				b += " (all)";
			else
				add(b, " rects %u, first %ld,%ld-%ld,%ld", unsigned(n), r[0].left, r[0].top, r[0].right, r[0].bottom);
		}

		float from_bits(uint64_t v)
		{
			const uint32_t bits = uint32_t(v);
			float f;
			std::memcpy(&f, &bits, sizeof(f));
			return f;
		}

		// A call as a line's body (no index or time)
		std::string body(uint32_t s, const void *self, const uint64_t *a, bool quiet)
		{
			std::string b;
			std::string object = "dev";
			if (host::is_list_slot(s))
				object = "L" + std::to_string(id_of(list_ids, self));
			else if (s == host::queue_execute)
				object = "Q" + std::to_string(id_of(queue_ids, self));
			add(b, "%-5s %s%-4s ", thread_name().c_str(), quiet ? "hifi " : "", object.c_str());
			switch (s)
			{
			case host::list_reset:
				add(b, "Reset (pipeline %s)", named(pso_ids, 'P', a[1]).c_str());
				break;
			case host::list_close:
				b += "Close";
				break;
			case host::list_barrier:
			{
				const uint32_t n = uint32_t(a[0]);
				const D3D12_RESOURCE_BARRIER *br = ptr<D3D12_RESOURCE_BARRIER>(a[1]);
				add(b, "ResourceBarrier %u:", n);
				for (uint32_t i = 0; br != nullptr && i < n && i < 16; ++i)
				{
					if (br[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
					{
						const D3D12_RESOURCE_TRANSITION_BARRIER &t = br[i].Transition;
						char sub[16];
						if (t.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
							std::snprintf(sub, sizeof(sub), "all");
						else
							std::snprintf(sub, sizeof(sub), "sub %u", t.Subresource);
						add(b, " [%s %s 0x%X->0x%X]", resource(t.pResource).c_str(), sub, unsigned(t.StateBefore), unsigned(t.StateAfter));
					}
					else if (br[i].Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING)
						add(b, " [aliasing %s -> %s]", resource(br[i].Aliasing.pResourceBefore).c_str(), resource(br[i].Aliasing.pResourceAfter).c_str());
					else
						add(b, " [UAV %s]", resource(br[i].UAV.pResource).c_str());
				}
				if (n > 16)
					b += " ...";
				break;
			}
			case host::list_om_targets:
			{
				const uint32_t n = uint32_t(a[0]);
				const D3D12_CPU_DESCRIPTOR_HANDLE *rtvs = ptr<D3D12_CPU_DESCRIPTOR_HANDLE>(a[1]);
				const D3D12_CPU_DESCRIPTOR_HANDLE *dsv = ptr<D3D12_CPU_DESCRIPTOR_HANDLE>(a[3]);
				b += "OMSetRenderTargets";
				if (n == 0 || rtvs == nullptr)
					b += " none";
				else if (a[2] != 0)
				{
					b += " " + view(rtvs[0].ptr);
					if (n > 1)
						add(b, " (and %u more in its range)", n - 1);
				}
				else
					for (uint32_t i = 0; i < n && i < 8; ++i)
						b += " " + view(rtvs[i].ptr);
				b += " ds " + (dsv != nullptr ? view(dsv->ptr) : std::string("none"));
				break;
			}
			case host::list_pipeline:
				add(b, "SetPipelineState %s", named(pso_ids, 'P', a[0]).c_str());
				break;
			case host::list_topology:
				add(b, "IASetPrimitiveTopology %u%s", unsigned(a[0]), topology_name(a[0]));
				break;
			case host::list_viewports:
			{
				const D3D12_VIEWPORT *v = ptr<D3D12_VIEWPORT>(a[1]);
				add(b, "RSSetViewports %u", unsigned(a[0]));
				if (a[0] != 0 && v != nullptr)
					add(b, ": %.1f %.1f %.1f x %.1f, z %g..%g", v->TopLeftX, v->TopLeftY, v->Width, v->Height, v->MinDepth, v->MaxDepth);
				break;
			}
			case host::list_scissors:
			{
				const D3D12_RECT *r = ptr<D3D12_RECT>(a[1]);
				add(b, "RSSetScissorRects %u", unsigned(a[0]));
				if (a[0] != 0 && r != nullptr)
					add(b, ": %ld,%ld-%ld,%ld", r->left, r->top, r->right, r->bottom);
				break;
			}
			case host::list_root_signature:
				add(b, "SetGraphicsRootSignature %s", named(root_ids, 'S', a[0]).c_str());
				break;
			case host::list_heaps:
			{
				add(b, "SetDescriptorHeaps %u", unsigned(a[0]));
				const uintptr_t *h = ptr<uintptr_t>(a[1]);
				for (uint32_t i = 0; h != nullptr && i < a[0] && i < 2; ++i)
					add(b, " 0x%llX", static_cast<unsigned long long>(h[i]));
				break;
			}
			case host::list_root_table:
				add(b, "SetGraphicsRootDescriptorTable [%u] 0x%llX", unsigned(a[0]), static_cast<unsigned long long>(a[1]));
				break;
			case host::list_root_constant:
				add(b, "SetGraphicsRoot32BitConstant [%u] 0x%08X at %u", unsigned(a[0]), unsigned(a[1]), unsigned(a[2]));
				break;
			case host::list_root_constants:
			{
				const uint32_t n = uint32_t(a[1]);
				const uint32_t *values = ptr<uint32_t>(a[2]);
				add(b, "SetGraphicsRoot32BitConstants [%u] %u at %u:", unsigned(a[0]), n, unsigned(a[3]));
				for (uint32_t i = 0; values != nullptr && i < n && i < 4; ++i)
					add(b, " %08X", values[i]);
				if (n > 4)
					b += " ...";
				break;
			}
			case host::list_root_cbv:
			case host::list_root_srv:
			case host::list_root_uav:
				add(b, "SetGraphicsRoot%sView [%u] 0x%llX", s == host::list_root_cbv ? "ConstantBuffer" : s == host::list_root_srv ? "ShaderResource" : "UnorderedAccess",
					unsigned(a[0]), static_cast<unsigned long long>(a[1]));
				break;
			case host::list_clear_depth:
			{
				const uint64_t flags = a[1];
				const char *what = (flags & D3D12_CLEAR_FLAG_DEPTH) != 0 ? ((flags & D3D12_CLEAR_FLAG_STENCIL) != 0 ? "depth+stencil" : "depth")
					: (flags & D3D12_CLEAR_FLAG_STENCIL) != 0 ? "stencil" : "nothing";
				add(b, "ClearDepthStencilView %s %s, depth %g stencil %u", view(a[0]).c_str(), what, from_bits(a[2]), unsigned(a[3] & 0xFF));
				rects_text(b, a[4], ptr<D3D12_RECT>(a[5]));
				// (HiFi XI's own slot markers, slotmark.hpp: the game side's one-pixel stencil clears)
				if (const D3D12_RECT *r = ptr<D3D12_RECT>(a[5]); r != nullptr)
					if (slotmark::mark m; slotmark::read((flags & D3D12_CLEAR_FLAG_DEPTH) != 0, (flags & D3D12_CLEAR_FLAG_STENCIL) != 0, uint32_t(a[4]), r->left, r->top, r->right, r->bottom, m))
						add(b, "  <- slot marker: %s, seq %u", slotmark::kind_name(m.kind), m.seq);
				break;
			}
			case host::list_clear_target:
			{
				const float *c = ptr<float>(a[1]);
				add(b, "ClearRenderTargetView %s", view(a[0]).c_str());
				if (c != nullptr)
					add(b, " colour %.4f %.4f %.4f %.4f", c[0], c[1], c[2], c[3]);
				rects_text(b, a[2], ptr<D3D12_RECT>(a[3]));
				break;
			}
			case host::list_draw:
				add(b, "DrawInstanced %u x%u (vertex %u, instance %u)", unsigned(a[0]), unsigned(a[1]), unsigned(a[2]), unsigned(a[3]));
				break;
			case host::list_draw_indexed:
				add(b, "DrawIndexedInstanced %u x%u (index %u, base %d, instance %u)", unsigned(a[0]), unsigned(a[1]), unsigned(a[2]), int(int64_t(a[3])), unsigned(a[4]));
				break;
			case host::list_execute_indirect:
				add(b, "ExecuteIndirect (at most %u)", unsigned(a[1]));
				break;
			case host::list_execute_bundle:
				b += "ExecuteBundle";
				break;
			case host::list_clear_state:
				b += "ClearState";
				break;
			case host::list_barrier7:
				add(b, "Barrier (enhanced) %u group(s)", unsigned(a[0]));
				break;
			case host::queue_execute:
			{
				ID3D12CommandList *const *l = ptr<ID3D12CommandList *>(a[1]);
				b += "ExecuteCommandLists";
				for (uint32_t i = 0; l != nullptr && i < uint32_t(a[0]); ++i)
					add(b, " L%u", id_of(list_ids, l[i]));
				break;
			}
			case host::device_create_list:
				add(b, "CreateCommandList (type %u)", unsigned(a[1]));
				break;
			case host::device_create_rtv:
			case host::device_create_dsv:
			{
				// (both views' descriptions start with their format)
				const DXGI_FORMAT *format = ptr<DXGI_FORMAT>(a[1]);
				add(b, "Create%sView %s, format %u, at 0x%llX", s == host::device_create_rtv ? "RenderTarget" : "DepthStencil", resource(ptr<void>(a[0])).c_str(),
					format != nullptr ? unsigned(*format) : 0u, static_cast<unsigned long long>(a[2]));
				break;
			}
			case host::device_copy:
				add(b, "CopyDescriptors %u range(s) <- %u, heap type %u", unsigned(a[0]), unsigned(a[3]), unsigned(a[6]));
				break;
			case host::device_copy_simple:
				add(b, "CopyDescriptorsSimple %u, heap type %u", unsigned(a[0]), unsigned(a[3]));
				break;
			default:
				b += host::slot_name(s);
				break;
			}
			return b;
		}

		// A world frame's start: the depth cleared over all of a target at least 640 wide, by dgVoodoo
		bool frame_start(uint32_t s, const uint64_t *a, bool quiet)
		{
			if (quiet || s != host::list_clear_depth || (a[1] & D3D12_CLEAR_FLAG_DEPTH) == 0 || views == nullptr)
				return false;
			ID3D12Resource *const r = static_cast<ID3D12Resource *>(views(a[0]));
			if (r == nullptr)
				return false;
			const D3D12_RESOURCE_DESC d = r->GetDesc();
			if (d.Width < 640)
				return false;
			const uint32_t n = uint32_t(a[4]);
			const D3D12_RECT *rects = ptr<D3D12_RECT>(a[5]);
			if (n == 0 || rects == nullptr)
				return true;
			for (uint32_t i = 0; i < n; ++i)
				if (rects[i].left <= 0 && rects[i].top <= 0 && rects[i].right >= LONG(d.Width) && rects[i].bottom >= LONG(d.Height))
					return true;
			return false;
		}

		// (mutex held) What the totals need of a call
		record make_record(uint32_t s, const void *self, const uint64_t *a, bool quiet)
		{
			record r;
			r.slot = s;
			r.quiet = quiet;
			if (host::is_list_slot(s))
				r.list = id_of(list_ids, self);
			if (s == host::list_om_targets)
			{
				const D3D12_CPU_DESCRIPTOR_HANDLE *rtvs = ptr<D3D12_CPU_DESCRIPTOR_HANDLE>(a[1]);
				const D3D12_CPU_DESCRIPTOR_HANDLE *dsv = ptr<D3D12_CPU_DESCRIPTOR_HANDLE>(a[3]);
				r.rt = a[0] != 0 && rtvs != nullptr ? view(rtvs[0].ptr) : std::string("none");
				r.ds = dsv != nullptr ? view(dsv->ptr) : std::string("none");
			}
			else if (s == host::list_draw || s == host::list_draw_indexed)
				r.vertices = a[0] * a[1];
			else if (s == host::queue_execute)
			{
				ID3D12CommandList *const *l = ptr<ID3D12CommandList *>(a[1]);
				for (uint32_t i = 0; l != nullptr && i < uint32_t(a[0]); ++i)
					r.submitted.push_back(id_of(list_ids, l[i]));
			}
			return r;
		}

		// (mutex held)
		void append(double us, const std::string &b, record r)
		{
			r.at = text.size();
			add(text, "%6u %10.1f ", ++index, us);
			text += b;
			text += '\n';
			records.push_back(std::move(r));
		}

		// Where a world frame's own calls begin, of the last `count` (get(i): the i-th's record): its colour clear (the last
		// of dgVoodoo's among the last kRing), with the barriers and the reset just before it; count when there's none
		template <typename Get>
		size_t prelude_start(size_t count, Get get)
		{
			size_t from = count;
			for (size_t i = count, seen = 0; i-- > 0 && seen < kRing; ++seen)
				if (!get(i).quiet && get(i).slot == host::list_clear_target)
				{
					from = i;
					break;
				}
			while (from > 0 && from < count && !get(from - 1).quiet && (get(from - 1).slot == host::list_barrier || get(from - 1).slot == host::list_reset))
				from--;
			return from;
		}

		// The tracer: every hooked call while a log is being made
		void on_call(uint32_t s, const void *self, const uint64_t *a, uint32_t, bool quiet)
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (now != phase::waiting && now != phase::recording)
				return;
			const bool start = frame_start(s, a, quiet);
			if (now == phase::recording && start)
			{
				const size_t from = prelude_start(records.size(), [](size_t i) -> const record & { return records[i]; });
				if (frames_seen >= frames_wanted)
				{
					// (the next frame's own calls before its depth clear go too)
					if (from < records.size())
					{
						text.resize(records[from].at);
						records.resize(from);
						index = uint32_t(records.size());
					}
					now = phase::done;
					ended = "at the next world frame's start";
					return;
				}
				char marker[40];
				std::snprintf(marker, sizeof(marker), "---- world frame %u\n", ++frames_seen);
				const size_t length = std::strlen(marker);
				text.insert(from < records.size() ? records[from].at : text.size(), marker);
				for (size_t i = from; i < records.size(); ++i)
					records[i].at += length;
			}
			const double us = micros();
			std::string b = body(s, self, a, quiet);
			record r = make_record(s, self, a, quiet);
			if (now == phase::waiting)
			{
				if (!start)
				{
					ring.push_back({ us, std::move(b), std::move(r) });
					if (ring.size() > kRing)
						ring.erase(ring.begin());
					return;
				}
				const size_t from = prelude_start(ring.size(), [](size_t i) -> const record & { return ring[i].r; });
				now = phase::recording;
				frames_seen = 1;
				text += "---- world frame 1\n";
				for (size_t i = from; i < ring.size(); ++i)
					append(ring[i].us, ring[i].body, std::move(ring[i].r));
				ring.clear();
			}
			append(us, b, std::move(r));
			if (text.size() > kMaxBytes)
			{
				now = phase::done;
				ended = "cut short: too long";
			}
		}

		std::string sanitized(const std::string &s)
		{
			std::string out;
			for (char c : s)
				if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
					if (out.size() < 40)
						out += c;
			return out;
		}

		std::string thousands(uint64_t v)
		{
			std::string s = std::to_string(v);
			for (int i = int(s.size()) - 3; i > 0; i -= 3)
				s.insert(size_t(i), ",");
			return s;
		}

		// (mutex held) The file: header, the calls, the game's draws, the totals; its path, or empty when it couldn't be made
		std::wstring write_file(std::string &why)
		{
			SYSTEMTIME t;
			GetLocalTime(&t);
			wchar_t name[160];
			std::wstring wlabel(label.begin(), label.end());
			std::swprintf(name, 160, L"hifixi-stream-%04u%02u%02u-%02u%02u%02u%s%s.txt", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
				label.empty() ? L"" : L"-", wlabel.c_str());
			const std::wstring path = folder + name;
			FILE *f = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
			if (f == nullptr)
			{
				why = "couldn't make the file";
				return std::wstring();
			}
			std::string h;
			add(h, "HiFi XI stream log (/hifi streamlog), %04u-%02u-%02u %02u:%02u:%02u%s%s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
				label.empty() ? "" : ", ", label.c_str());
			h += context;
			if (!context.empty() && context.back() != '\n')
				h += '\n';
			add(h, "World frames asked for: %u; logged: %u. Ended: %s.\n", frames_wanted, frames_seen, ended.c_str());
			h += "A world frame starts at the depth clear over all of a target at least 640 wide (dgVoodoo clears the world's colour, then its\n"
				 "depth); the colour clear before it, and anything between, comes from the calls kept while waiting.\n"
				 "Each line: the call's number, microseconds since the log started (the present after the request), the thread (game = the\n"
				 "game's own; t1.. dgVoodoo's), 'hifi' for HiFi XI's own calls (World FX drawing into the frame), the object (L = command\n"
				 "list, Q = queue, dev = device), the call. R<n>(WxH f<DXGI format> [x<samples>]) = a resource, P<n> a pipeline, S<n> a root\n"
				 "signature; barrier states are D3D12_RESOURCE_STATES (0x4 render target, 0x10 depth write, 0x20 depth read, 0x80 pixel\n"
				 "shader resource, 0x400/0x800 copy dest/source); depth's sub 0 is its depth plane, sub 1 its stencil.\n";
			std::fputs(h.c_str(), f);
			std::fputs("\n==== Direct3D 12: dgVoodoo's calls (and HiFi XI's own) ====\n", f);
			std::fputs(text.c_str(), f);
			std::fprintf(f, "\n==== The game's Direct3D 8 draws in the same game frame(s): HiFi XI's tap on the client device's four draw calls, with the FFXiMain return addresses on the stack (image offsets, innermost first) ====\n");
			std::fputs(d3d8.c_str(), f);
			// The totals, from the calls' records
			std::map<uint32_t, list_totals> lists;
			std::map<std::pair<std::string, std::string>, target_totals> targets;
			uint64_t by_slot[host::slot_count] = {};
			uint64_t calls = 0, own_calls = 0;
			for (const record &r : records)
			{
				if (r.quiet)
				{
					own_calls++;
					continue;
				}
				calls++;
				if (r.slot < host::slot_count)
					by_slot[r.slot]++;
				if (r.slot == host::queue_execute)
					for (uint32_t id : r.submitted)
						lists[id].submitted++;
				if (!host::is_list_slot(r.slot))
					continue;
				list_totals &l = lists[r.list];
				switch (r.slot)
				{
				case host::list_reset:
					l.resets++;
					l.rt = l.ds = "none";
					break;
				case host::list_close:
					l.closes++;
					break;
				case host::list_om_targets:
					l.rt = r.rt;
					l.ds = r.ds;
					break;
				case host::list_draw:
				case host::list_draw_indexed:
				{
					l.draws++;
					l.vertices += r.vertices;
					target_totals &pair = targets[{ l.rt, l.ds }];
					pair.draws++;
					pair.vertices += r.vertices;
					if (r.slot == host::list_draw_indexed)
						pair.indexed++;
					break;
				}
				default:
					break;
				}
			}
			std::string s;
			add(s, "\n==== Totals over the world frames above (dgVoodoo's calls; HiFi XI's own, %s of them, left out) ====\n", thousands(own_calls).c_str());
			add(s, "calls: %s;", thousands(calls).c_str());
			for (uint32_t i = 0; i < host::slot_count; ++i)
				if (by_slot[i] != 0)
					add(s, " %s %s;", host::slot_name(i), thousands(by_slot[i]).c_str());
			s += "\nlists:\n";
			for (const auto &l : lists)
				add(s, "  L%u: %s draws, %s vertices; reset %s, closed %s, submitted %s times\n", l.first, thousands(l.second.draws).c_str(),
					thousands(l.second.vertices).c_str(), thousands(l.second.resets).c_str(), thousands(l.second.closes).c_str(), thousands(l.second.submitted).c_str());
			s += "draws by the target and depth bound (as World FX finds the world: the pair with the most vertices):\n";
			std::vector<std::pair<std::pair<std::string, std::string>, target_totals>> sorted(targets.begin(), targets.end());
			std::sort(sorted.begin(), sorted.end(), [](const auto &x, const auto &y) { return x.second.vertices > y.second.vertices; });
			for (const auto &p : sorted)
				add(s, "  %s + %s: %s draws (%s indexed), %s vertices\n", p.first.first.c_str(), p.first.second.c_str(), thousands(p.second.draws).c_str(),
					thousands(p.second.indexed).c_str(), thousands(p.second.vertices).c_str());
			add(s, "the game's Direct3D 8 draws: %s, by where they're made (the device call's return address < its caller's), the most first:\n", thousands(d3d8_index).c_str());
			std::vector<std::pair<std::string, uint64_t>> sites(d3d8_sites.begin(), d3d8_sites.end());
			std::sort(sites.begin(), sites.end(), [](const auto &x, const auto &y) { return x.second > y.second; });
			for (const auto &site : sites)
				add(s, "  %8s  %s\n", thousands(site.second).c_str(), site.first.c_str());
			std::fputs(s.c_str(), f);
			std::fclose(f);
			return path;
		}

		void reset_log()
		{
			text.clear();
			text.reserve(size_t(4) << 20);
			ring.clear();
			index = 0;
			frames_seen = 0;
			ended = "not yet";
			list_ids.clear();
			queue_ids.clear();
			resource_ids.clear();
			pso_ids.clear();
			root_ids.clear();
			thread_ids.clear();
			records.clear();
			d3d8.clear();
			d3d8_index = 0;
			d3d8_sites.clear();
			d3d8_presents = 0;
		}

		void find_ffximain()
		{
			ffximain.looked = true;
			const HMODULE m = GetModuleHandleW(L"FFXiMain.dll");
			if (m == nullptr)
				return;
			const uint8_t *base = reinterpret_cast<const uint8_t *>(m);
			const IMAGE_DOS_HEADER *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
			const IMAGE_NT_HEADERS *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
			const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
			for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
				if ((sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0)
				{
					ffximain.base = base;
					ffximain.code_from = sec[i].VirtualAddress;
					ffximain.code_to = sec[i].VirtualAddress + std::max<uint32_t>(sec[i].Misc.VirtualSize, sec[i].SizeOfRawData);
					return;
				}
		}
	}

	namespace
	{
		// The device's four draw methods as COM calls them (the device first, stdcall): each handed to d3d8_draw, then on
		// to what its slot held
		using dp_fn = long(__stdcall *)(void *, uint32_t, uint32_t, uint32_t);
		using dip_fn = long(__stdcall *)(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
		using dpup_fn = long(__stdcall *)(void *, uint32_t, uint32_t, const void *, uint32_t);
		using dipup_fn = long(__stdcall *)(void *, uint32_t, uint32_t, uint32_t, uint32_t, const void *, uint32_t, const void *, uint32_t);
		long __stdcall tap_dp(void *self, uint32_t type, uint32_t start, uint32_t primitives)
		{
			d3d8_draw("DP", type, primitives, 0);
			return reinterpret_cast<dp_fn>(tap_originals[0])(self, type, start, primitives);
		}
		long __stdcall tap_dip(void *self, uint32_t type, uint32_t min_index, uint32_t vertices, uint32_t start, uint32_t primitives)
		{
			d3d8_draw("DIP", type, primitives, vertices);
			return reinterpret_cast<dip_fn>(tap_originals[1])(self, type, min_index, vertices, start, primitives);
		}
		long __stdcall tap_dpup(void *self, uint32_t type, uint32_t primitives, const void *data, uint32_t stride)
		{
			d3d8_draw("DPUP", type, primitives, stride);
			return reinterpret_cast<dpup_fn>(tap_originals[2])(self, type, primitives, data, stride);
		}
		long __stdcall tap_dipup(void *self, uint32_t type, uint32_t min_index, uint32_t vertices, uint32_t primitives, const void *indices,
			uint32_t format, const void *data, uint32_t stride)
		{
			d3d8_draw("DIPUP", type, primitives, vertices);
			return reinterpret_cast<dipup_fn>(tap_originals[3])(self, type, min_index, vertices, primitives, indices, format, data, stride);
		}
		const void *const kTaps[4] = { reinterpret_cast<const void *>(&tap_dp), reinterpret_cast<const void *>(&tap_dip),
			reinterpret_cast<const void *>(&tap_dpup), reinterpret_cast<const void *>(&tap_dipup) };
	}

	void set_d3d8_tap(bool (*on)(const void *const taps[4], void *originals[4], std::string &why), void (*off)())
	{
		tap_on_fn = on;
		tap_off_fn = off;
	}

	uint32_t callers(const uint8_t *image, uintptr_t base, uint32_t code_from, uint32_t code_to, const uint32_t *stack, uint32_t words, uint32_t *out,
		uint32_t most)
	{
		uint32_t n = 0;
		for (uint32_t i = 0; i < words && n < most; ++i)
		{
			const uintptr_t v = stack[i];
			if (v < base + code_from + 7 || v >= base + code_to)
				continue;
			const uint8_t *p = image + (v - base);
			// call rel32; call [abs32]; call [reg+disp32] / [sib+disp32]; call [reg+disp8] / [sib+disp8]; call [reg] / [sib];
			// call reg
			const bool after_call = p[-5] == 0xE8 || (p[-6] == 0xFF && p[-5] == 0x15) ||
				(p[-6] == 0xFF && (p[-5] & 0xF8) == 0x90 && p[-5] != 0x94) || (p[-7] == 0xFF && p[-6] == 0x94) ||
				(p[-3] == 0xFF && (p[-2] & 0xF8) == 0x50 && p[-2] != 0x54) || (p[-4] == 0xFF && p[-3] == 0x54) ||
				(p[-2] == 0xFF && (p[-1] & 0xF8) == 0x10 && p[-1] != 0x14 && p[-1] != 0x15) || (p[-3] == 0xFF && p[-2] == 0x14) ||
				(p[-2] == 0xFF && (p[-1] & 0xF8) == 0xD0);
			if (after_call)
				out[n++] = uint32_t(v - base);
		}
		return n;
	}

	bool request(uint32_t frames, const std::string &l, const std::string &ctx)
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (now != phase::idle)
			return false;
		frames_wanted = std::clamp<uint32_t>(frames, 1, kMaxFrames);
		label = sanitized(l);
		context = ctx;
		now = phase::asked;
		active.store(true);
		return true;
	}

	bool busy()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return now != phase::idle;
	}

	std::string present()
	{
		if (!active.load())
			return std::string();
		// The game's frames of draws: one from each present to the next, as many as world frames were asked for
		if (d3d8_recording.load(std::memory_order_relaxed))
		{
			if (++d3d8_presents >= frames_wanted)
			{
				d3d8_recording.store(false, std::memory_order_relaxed);
				if (tap_off_fn != nullptr)
					tap_off_fn();
			}
			else
				add(d3d8, "---- game frame %u\n", d3d8_presents + 1);
		}
		const ULONGLONG t = GetTickCount64();
		std::lock_guard<std::mutex> lock(mutex);
		if (now == phase::asked)
		{
			reset_log();
			game_thread = GetCurrentThreadId();
			QueryPerformanceFrequency(&frequency);
			QueryPerformanceCounter(&started);
			armed_at = t;
			now = phase::waiting;
			d3d8 = "---- game frame 1\n";
			d3d8_recording.store(true, std::memory_order_relaxed);
			// (the game's draws through the tap on its device: on until the game's frames are in)
			std::string why = "no tap set";
			if (tap_on_fn == nullptr || !tap_on_fn(kTaps, tap_originals, why))
				d3d8 = "(the game's draws not tapped: " + why + ")\n" + d3d8;
			host::set_tracer(on_call);
			return std::string();
		}
		if (now == phase::waiting && t - armed_at > wait_ms)
		{
			now = phase::done;
			ended = "no world frame started within " + std::to_string(wait_ms) + " ms (no world on screen: zoning, a menu over it?)";
			// (what was seen while waiting, so the file isn't empty)
			text += "---- no world frame start seen; the last calls:\n";
			for (kept &k : ring)
				append(k.us, k.body, std::move(k.r));
			ring.clear();
		}
		if ((now == phase::waiting || now == phase::recording) && t - armed_at > kMaxMs)
		{
			now = phase::done;
			ended = "stopped after " + std::to_string(kMaxMs / 1000) + " s";
		}
		if (now != phase::done || d3d8_recording.load(std::memory_order_relaxed))
			return std::string();
		host::set_tracer(nullptr);
		std::string why;
		const std::wstring path = write_file(why);
		now = phase::idle;
		active.store(false);
		text.clear();
		text.shrink_to_fit();
		const uint32_t lines = index;
		records.clear();
		records.shrink_to_fit();
		d3d8.clear();
		d3d8.shrink_to_fit();
		if (path.empty())
			return "Stream log not written: " + why;
		std::string narrow;
		for (wchar_t c : path)
			narrow += c < 128 ? char(c) : '?';
		return "Stream log written: " + narrow + " (" + std::to_string(frames_seen) + " world frame(s), " + thousands(lines) + " calls, " +
			thousands(d3d8_index) + " of the game's draws)";
	}

	bool d3d8_on()
	{
		return d3d8_recording.load(std::memory_order_relaxed);
	}

	void d3d8_draw(const char *call, uint32_t type, uint32_t primitives, uint32_t extra)
	{
		if (!d3d8_on())
			return;
		if (!ffximain.looked)
			find_ffximain();
		uint32_t found[3] = {};
		uint32_t n = 0;
		if (ffximain.base != nullptr)
		{
			// (this thread's stack from here up: the game's return addresses are above Ashita's)
			const NT_TIB *tib = reinterpret_cast<const NT_TIB *>(NtCurrentTeb());
			volatile uint32_t here = 0;
			const uint32_t *from = reinterpret_cast<const uint32_t *>(const_cast<uint32_t *>(&here));
			const uint32_t *top = static_cast<const uint32_t *>(tib->StackBase);
			const uint32_t words = top > from ? uint32_t(std::min<ptrdiff_t>(top - from, 4096)) : 0;
			n = callers(ffximain.base, uintptr_t(ffximain.base), ffximain.code_from, ffximain.code_to, from, words, found, 3);
		}
		std::string line;
		add(line, "%6u %10.1f D3D8 %s %s prims %u", ++d3d8_index, micros(), call, primitive_name(type), primitives);
		if (std::strcmp(call, "DIP") == 0 || std::strcmp(call, "DIPUP") == 0)
			add(line, " verts %u", extra);
		else if (std::strcmp(call, "DPUP") == 0)
			add(line, " stride %u", extra);
		line += " [";
		for (uint32_t i = 0; i < n; ++i)
			add(line, i == 0 ? "%06X" : " < %06X", found[i]);
		line += "]\n";
		d3d8 += line;
		char site[40] = "(not in FFXiMain)";
		if (n >= 2)
			std::snprintf(site, sizeof(site), "%06X < %06X", found[0], found[1]);
		else if (n == 1)
			std::snprintf(site, sizeof(site), "%06X", found[0]);
		d3d8_sites[site]++;
	}

	void set_views(void *(*resource_of_view)(uint64_t view))
	{
		std::lock_guard<std::mutex> lock(mutex);
		views = resource_of_view;
	}

	void set_folder(const std::wstring &f)
	{
		std::lock_guard<std::mutex> lock(mutex);
		folder = f;
	}

	void set_wait_ms(uint32_t ms)
	{
		std::lock_guard<std::mutex> lock(mutex);
		wait_ms = ms;
	}
}
