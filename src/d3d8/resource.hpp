#pragma once

// Direct3D 8's resources on Direct3D 12.
//
// Every texture, cube texture, volume texture and stand-alone surface (render target, depth buffer, image surface, back
// buffer) is an `image`: the GPU texture (none in the system-memory pool) and, for what the game can lock or copy from on
// the CPU, a copy of each subresource in the game's own bytes. A lock writes the copy and marks the subresource dirty;
// it's uploaded on the list just before the GPU next reads it (a draw, a copy), so the upload keeps its place among the
// draws. Render targets live on the GPU only: a lock of one, or a copy from one into system memory, reads it back
// (the list submitted and waited on, as dgVoodoo does at the game's visibility tests).
//
// Vertex and index buffers keep the game's bytes too. A buffer the game writes often (DYNAMIC, or locked again after it
// was drawn with) streams: each frame it's read by the GPU straight from upload memory, a fresh copy after any lock
// that wasn't NOOVERWRITE (DISCARD and plain locks alike), the locked range copied in place after a NOOVERWRITE one. The
// rest are copied once into a buffer of their own (and again when locked).

#include "gpu.hpp"
#include "format.hpp"
#include <map>
#include <memory>

namespace hifi::d3d8
{
	// SetPrivateData's store (every resource has one)
	struct private_data
	{
		std::map<GUID, std::vector<uint8_t>, bool (*)(const GUID &, const GUID &)> items { [](const GUID &a, const GUID &b) { return std::memcmp(&a, &b, sizeof(GUID)) < 0; } };
		std::map<GUID, IUnknown *, bool (*)(const GUID &, const GUID &)> objects { [](const GUID &a, const GUID &b) { return std::memcmp(&a, &b, sizeof(GUID)) < 0; } };
		HRESULT set(REFGUID g, const void *data, DWORD size, DWORD flags);
		HRESULT get(REFGUID g, void *data, DWORD *size);
		HRESULT drop(REFGUID g);
		~private_data();
	};

	// What an image is used as
	enum class image_kind : uint8_t { texture, target, depth, sysmem };

	// A texture's (or a stand-alone surface's) GPU texture and the game's copies of its subresources
	class image
	{
	public:
		image(device *d) : dev(d) {}
		~image();
		bool make(D3DRESOURCETYPE type, UINT w, UINT h, UINT depth, UINT levels, UINT faces, DWORD usage, D3DFORMAT f, D3DPOOL pool, D3DMULTISAMPLE_TYPE ms);

		device *dev;
		D3DRESOURCETYPE type = D3DRTYPE_TEXTURE;
		D3DFORMAT format = D3DFMT_UNKNOWN;
		D3DPOOL pool = D3DPOOL_DEFAULT;
		DWORD usage = 0;
		D3DMULTISAMPLE_TYPE multisample = D3DMULTISAMPLE_NONE;
		UINT width = 0, height = 0, depth = 1, levels = 1, faces = 1;
		image_kind kind = image_kind::texture;
		bool decode = false;     // DXT level not in whole blocks: uploaded decoded to BGRA8
		gpu_resource gpu;        // (res null: system memory)
		DXGI_FORMAT gpu_format = DXGI_FORMAT_UNKNOWN;
		ID3D12Resource *resolved = nullptr; // a multisampled target's single-sample copy (sampled, read back)
		D3D12_CPU_DESCRIPTOR_HANDLE srv = {};
		std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvs, dsvs; // per subresource (targets, depth)
		D3D12_SRV_DIMENSION srv_dim = D3D12_SRV_DIMENSION_TEXTURE2D;

		// The game's copy of each subresource (face * levels + level), made when first needed
		std::vector<std::vector<uint8_t>> cpu;
		std::vector<uint8_t> dirty;          // per subresource: the copy is newer than the GPU's
		std::vector<uint8_t> cpu_stale;      // per subresource: the GPU's is newer than the copy (a target drawn to)
		// A copy from the GPU on its way into a subresource (CopyRects into system memory), done at the next CPU access
		struct pending_read
		{
			readback rb;
			UINT sub = 0, x = 0, y = 0, w = 0, h = 0, row_pitch = 0;
			D3DFORMAT from = D3DFMT_UNKNOWN; // the source's format
			bool target = false;             // from a render target (RGBA8): converted
		};
		std::vector<pending_read> reads;

		UINT subresource(UINT face, UINT level) const { return face * levels + level; }
		UINT level_width(UINT level) const { return level_dim(width, level); }
		UINT level_height(UINT level) const { return level_dim(height, level); }
		UINT level_depth(UINT level) const { return level_dim(depth, level); }
		UINT pitch(UINT level) const { return row_pitch(format, level_width(level)); }
		UINT slice(UINT level) const { return pitch(level) * row_count(format, level_height(level)); }
		std::vector<uint8_t> &copy(UINT sub);  // the game's copy (made, read back first when the GPU's is newer)
		void finish_reads();                    // pending copies from the GPU in
		void upload();                          // dirty subresources up, on the list
		void upload(UINT sub);
		void read_back(UINT sub);               // the GPU's subresource into the copy (submits and waits)
		void write_target(UINT sub, const RECT *r); // a target's copy (locked and written) to the GPU
		bool lockable() const { return kind != image_kind::depth; }
		ID3D12Resource *sample_resource(); // the single-sample texture to read (a multisampled target resolved first)
		void drop_gpu();
	};

	// IDirect3DResource8's shared part
	struct resource_common
	{
		private_data data;
		DWORD priority = 0;
	};

	class surface final : public IDirect3DSurface8
	{
	public:
		// A level (face) of a texture, or a stand-alone surface that owns its image
		surface(device *d, image *img, UINT face, UINT level, IUnknown *container, bool owns);
		~surface();
		image *img;
		UINT face, level;
		IUnknown *container; // (not referenced: the texture or swap chain this is part of; null when it owns img)
		bool owns;
		device *dev;
		refcount rc;
		resource_common common;
		bool locked = false;
		DWORD lock_flags = 0;
		RECT lock_rect = {};
		std::vector<uint8_t> lock_buffer; // a target's lock: its pixels read back

		UINT sub() const { return img->subresource(face, level); }
		UINT width() const { return img->level_width(level); }
		UINT height() const { return img->level_height(level); }
		D3D12_CPU_DESCRIPTOR_HANDLE rtv() const { return img->rtvs.empty() ? D3D12_CPU_DESCRIPTOR_HANDLE {} : img->rtvs[sub()]; }
		D3D12_CPU_DESCRIPTOR_HANDLE dsv() const { return img->dsvs.empty() ? D3D12_CPU_DESCRIPTOR_HANDLE {} : img->dsvs[sub()]; }

		STDMETHOD(QueryInterface)(REFIID riid, void **ppv) override;
		STDMETHOD_(ULONG, AddRef)() override;
		STDMETHOD_(ULONG, Release)() override;
		STDMETHOD(GetDevice)(IDirect3DDevice8 **ppDevice) override;
		STDMETHOD(SetPrivateData)(REFGUID refguid, CONST void *pData, DWORD SizeOfData, DWORD Flags) override;
		STDMETHOD(GetPrivateData)(REFGUID refguid, void *pData, DWORD *pSizeOfData) override;
		STDMETHOD(FreePrivateData)(REFGUID refguid) override;
		STDMETHOD(GetContainer)(REFIID riid, void **ppContainer) override;
		STDMETHOD(GetDesc)(D3DSURFACE_DESC *pDesc) override;
		STDMETHOD(LockRect)(D3DLOCKED_RECT *pLockedRect, CONST RECT *pRect, DWORD Flags) override;
		STDMETHOD(UnlockRect)() override;
	};

	class volume final : public IDirect3DVolume8
	{
	public:
		volume(device *d, image *img, UINT level, IUnknown *container);
		image *img;
		UINT level;
		IUnknown *container;
		device *dev;
		resource_common common;
		bool locked = false;

		STDMETHOD(QueryInterface)(REFIID riid, void **ppv) override;
		STDMETHOD_(ULONG, AddRef)() override;
		STDMETHOD_(ULONG, Release)() override;
		STDMETHOD(GetDevice)(IDirect3DDevice8 **ppDevice) override;
		STDMETHOD(SetPrivateData)(REFGUID refguid, CONST void *pData, DWORD SizeOfData, DWORD Flags) override;
		STDMETHOD(GetPrivateData)(REFGUID refguid, void *pData, DWORD *pSizeOfData) override;
		STDMETHOD(FreePrivateData)(REFGUID refguid) override;
		STDMETHOD(GetContainer)(REFIID riid, void **ppContainer) override;
		STDMETHOD(GetDesc)(D3DVOLUME_DESC *pDesc) override;
		STDMETHOD(LockBox)(D3DLOCKED_BOX *pLockedVolume, CONST D3DBOX *pBox, DWORD Flags) override;
		STDMETHOD(UnlockBox)() override;
	};

	// The three texture kinds share this: the image and its surfaces
	class base_texture
	{
	public:
		virtual ~base_texture() = default;
		image *img = nullptr;
		DWORD lod = 0;
		virtual IDirect3DBaseTexture8 *as_base() = 0;
	};

#define HIFI_D3D8_RESOURCE_METHODS \
	STDMETHOD(QueryInterface)(REFIID riid, void **ppv) override; \
	STDMETHOD_(ULONG, AddRef)() override; \
	STDMETHOD_(ULONG, Release)() override; \
	STDMETHOD(GetDevice)(IDirect3DDevice8 **ppDevice) override; \
	STDMETHOD(SetPrivateData)(REFGUID refguid, CONST void *pData, DWORD SizeOfData, DWORD Flags) override; \
	STDMETHOD(GetPrivateData)(REFGUID refguid, void *pData, DWORD *pSizeOfData) override; \
	STDMETHOD(FreePrivateData)(REFGUID refguid) override; \
	STDMETHOD_(DWORD, SetPriority)(DWORD PriorityNew) override; \
	STDMETHOD_(DWORD, GetPriority)() override; \
	STDMETHOD_(void, PreLoad)() override; \
	STDMETHOD_(D3DRESOURCETYPE, GetType)() override;

#define HIFI_D3D8_BASE_TEXTURE_METHODS \
	STDMETHOD_(DWORD, SetLOD)(DWORD LODNew) override; \
	STDMETHOD_(DWORD, GetLOD)() override; \
	STDMETHOD_(DWORD, GetLevelCount)() override;

	class texture final : public IDirect3DTexture8, public base_texture
	{
	public:
		texture(device *d) : dev(d) {}
		~texture();
		device *dev;
		refcount rc;
		resource_common common;
		std::vector<std::unique_ptr<surface>> levels;
		IDirect3DBaseTexture8 *as_base() override { return this; }

		HIFI_D3D8_RESOURCE_METHODS
		HIFI_D3D8_BASE_TEXTURE_METHODS
		STDMETHOD(GetLevelDesc)(UINT Level, D3DSURFACE_DESC *pDesc) override;
		STDMETHOD(GetSurfaceLevel)(UINT Level, IDirect3DSurface8 **ppSurfaceLevel) override;
		STDMETHOD(LockRect)(UINT Level, D3DLOCKED_RECT *pLockedRect, CONST RECT *pRect, DWORD Flags) override;
		STDMETHOD(UnlockRect)(UINT Level) override;
		STDMETHOD(AddDirtyRect)(CONST RECT *pDirtyRect) override;
	};

	class cube_texture final : public IDirect3DCubeTexture8, public base_texture
	{
	public:
		cube_texture(device *d) : dev(d) {}
		~cube_texture();
		device *dev;
		refcount rc;
		resource_common common;
		std::vector<std::unique_ptr<surface>> faces; // face * levels + level
		IDirect3DBaseTexture8 *as_base() override { return this; }

		HIFI_D3D8_RESOURCE_METHODS
		HIFI_D3D8_BASE_TEXTURE_METHODS
		STDMETHOD(GetLevelDesc)(UINT Level, D3DSURFACE_DESC *pDesc) override;
		STDMETHOD(GetCubeMapSurface)(D3DCUBEMAP_FACES FaceType, UINT Level, IDirect3DSurface8 **ppCubeMapSurface) override;
		STDMETHOD(LockRect)(D3DCUBEMAP_FACES FaceType, UINT Level, D3DLOCKED_RECT *pLockedRect, CONST RECT *pRect, DWORD Flags) override;
		STDMETHOD(UnlockRect)(D3DCUBEMAP_FACES FaceType, UINT Level) override;
		STDMETHOD(AddDirtyRect)(D3DCUBEMAP_FACES FaceType, CONST RECT *pDirtyRect) override;
	};

	class volume_texture final : public IDirect3DVolumeTexture8, public base_texture
	{
	public:
		volume_texture(device *d) : dev(d) {}
		~volume_texture();
		device *dev;
		refcount rc;
		resource_common common;
		std::vector<std::unique_ptr<volume>> levels;
		IDirect3DBaseTexture8 *as_base() override { return this; }

		HIFI_D3D8_RESOURCE_METHODS
		HIFI_D3D8_BASE_TEXTURE_METHODS
		STDMETHOD(GetLevelDesc)(UINT Level, D3DVOLUME_DESC *pDesc) override;
		STDMETHOD(GetVolumeLevel)(UINT Level, IDirect3DVolume8 **ppVolumeLevel) override;
		STDMETHOD(LockBox)(UINT Level, D3DLOCKED_BOX *pLockedVolume, CONST D3DBOX *pBox, DWORD Flags) override;
		STDMETHOD(UnlockBox)(UINT Level) override;
		STDMETHOD(AddDirtyBox)(CONST D3DBOX *pDirtyBox) override;
	};

	// The texture behind a base texture pointer the game hands in (one of ours, or null)
	base_texture *texture_of(IDirect3DBaseTexture8 *t);

	// A vertex or index buffer's bytes and their GPU copy
	class buffer
	{
	public:
		buffer(device *d) : dev(d) {}
		~buffer();
		bool make(UINT size, DWORD usage, D3DPOOL pool);
		device *dev;
		UINT size = 0;
		DWORD usage = 0;
		D3DPOOL pool = D3DPOOL_DEFAULT;
		std::vector<uint8_t> bytes;
		bool streaming = false;
		gpu_resource gpu;        // (static buffers)
		uint64_t state_epoch = 0; // buffers fall back to COMMON at each submit: the state below is this list's
		D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
		D3D12_GPU_VIRTUAL_ADDRESS stream_at = 0; // this frame's copy
		uint8_t *stream_cpu = nullptr;
		uint64_t stream_frame = ~0ull;
		UINT dirty_lo = 0, dirty_hi = 0;          // changed since the GPU's copy
		bool all_noov = true;                     // every lock since was NOOVERWRITE
		uint32_t uploads = 0;
		uint64_t last_draw_epoch = 0;

		void lock(UINT offset, UINT n, DWORD flags);
		// The GPU address to read the buffer from now (on the list: the static buffer brought up to date first)
		D3D12_GPU_VIRTUAL_ADDRESS address(D3D12_RESOURCE_STATES use);
	};

	class vertex_buffer final : public IDirect3DVertexBuffer8
	{
	public:
		vertex_buffer(device *d) : dev(d), buf(d) {}
		device *dev;
		refcount rc;
		resource_common common;
		buffer buf;
		DWORD fvf = 0;

		HIFI_D3D8_RESOURCE_METHODS
		STDMETHOD(Lock)(UINT OffsetToLock, UINT SizeToLock, BYTE **ppbData, DWORD Flags) override;
		STDMETHOD(Unlock)() override;
		STDMETHOD(GetDesc)(D3DVERTEXBUFFER_DESC *pDesc) override;
	};

	class index_buffer final : public IDirect3DIndexBuffer8
	{
	public:
		index_buffer(device *d) : dev(d), buf(d) {}
		device *dev;
		refcount rc;
		resource_common common;
		buffer buf;
		D3DFORMAT format = D3DFMT_INDEX16;

		HIFI_D3D8_RESOURCE_METHODS
		STDMETHOD(Lock)(UINT OffsetToLock, UINT SizeToLock, BYTE **ppbData, DWORD Flags) override;
		STDMETHOD(Unlock)() override;
		STDMETHOD(GetDesc)(D3DINDEXBUFFER_DESC *pDesc) override;
	};
}
