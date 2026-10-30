#pragma once

// IDirect3DDevice8 on Direct3D 12 (docs/own-frame/d3d8.md).
//
// The device keeps Direct3D 8's state as the game sets it (render states, texture stage states, transforms, lights,
// the material, textures, streams, shaders and their constants; state blocks record and replay it). Nothing reaches the
// GPU until a draw or a clear needs it, as dgVoodoo does: at a draw the shaders and the pipeline for the state are
// found (made the first time), the constants uploaded, textures and buffers brought up to date, resources put in the
// states the draw needs, and only what changed since the last draw on this list bound again.
//
// On the list, as HiFi XI's hooks expect it of dgVoodoo:
// - render targets R8G8B8A8 (views of format 28 on typeless resources of 27), depth D24S8 (45 on 44);
// - viewports moved by half a pixel (Direct3D 8's pixel centres), depth range 0..1 (the viewport's MinZ/MaxZ go into
//   the shaders), scissors the viewport;
// - a Clear goes on as ClearRenderTargetView / ClearDepthStencilView in its place, with the game's rects (one rect of
//   1x1 for the slot markers' stencil-only clears), the targets bound first;
// - the depth buffer's planes follow what the draw writes: depth DEPTH_WRITE while depth is written, else DEPTH_READ;
//   stencil the same (DEPTH_READ but around stencil clears and stencil writes);
// - a target goes to PIXEL_SHADER_RESOURCE when read (the resolve), RENDER_TARGET when drawn to; the back buffer to
//   COPY_SOURCE at Present;
// - descriptor heaps set once after each Reset; the list closed at readbacks, every kSplitDraws draws and at Present.

#include "resource.hpp"
#include "shaders.hpp"
#include <memory>
#include <unordered_map>

namespace hifi::d3d8
{
	constexpr uint32_t kMaxStreams = 16;
	constexpr uint32_t kMaxLights = 64;
	constexpr uint32_t kRenderStates = 256;
	constexpr uint32_t kStageStates = 32;

	struct light_slot
	{
		D3DLIGHT8 light = {};
		bool set = false;
		bool enabled = false;
	};

	// A vertex declaration (an FVF read as one, or a vertex shader's) and its input layout
	struct declaration
	{
		struct element
		{
			uint8_t stream, reg, type; // D3DVSDT_*
			uint16_t offset;
		};
		std::vector<element> elements;
		uint8_t inputs[16] = {};          // input_kind per register
		uint16_t streams = 0;             // streams read
		std::vector<D3D12_INPUT_ELEMENT_DESC> layout;
		uint64_t hash = 0;
		bool rhw = false;                 // XYZRHW
		void finish();
	};

	struct vertex_shader
	{
		declaration decl;
		std::vector<std::pair<uint32_t, std::vector<float>>> constants; // the declaration's D3DVSD_CONST
		std::vector<DWORD> decl_tokens;
		sm1_shader code;                  // (ok false: fixed function with this declaration)
		bool programmable = false;
	};

	struct pixel_shader
	{
		sm1_shader code;
	};

	// Everything a state block can hold (the device's own is one)
	struct device_state
	{
		DWORD rs[kRenderStates] = {};
		DWORD tss[kMaxStages][kStageStates] = {};
		D3DMATRIX world[4] = {}, view = {}, proj = {}, texm[kMaxStages] = {};
		D3DMATERIAL8 material = {};
		light_slot lights[kMaxLights];
		float clip[6][4] = {};
		D3DVIEWPORT8 viewport = {};
		IDirect3DBaseTexture8 *textures[kMaxStages] = {};
		IDirect3DVertexBuffer8 *streams[kMaxStreams] = {};
		UINT strides[kMaxStreams] = {};
		IDirect3DIndexBuffer8 *indices = nullptr;
		UINT base_vertex = 0;
		DWORD vs = 0, ps = 0;
		float vsc[kVsConstRegs][4] = {};
		float psc[8][4] = {};
		UINT palette = 0;
	};

	// Which parts of a device_state a state block has
	struct state_mask
	{
		uint8_t rs[kRenderStates] = {};
		uint8_t tss[kMaxStages][kStageStates] = {};
		uint8_t world[4] = {}, view = 0, proj = 0, texm[kMaxStages] = {};
		uint8_t material = 0;
		uint8_t light[kMaxLights] = {}, light_enable[kMaxLights] = {};
		uint8_t clip[6] = {};
		uint8_t viewport = 0;
		uint8_t textures[kMaxStages] = {};
		uint8_t streams[kMaxStreams] = {};
		uint8_t indices = 0, vs = 0, ps = 0;
		uint8_t vsc[kVsConstRegs] = {};
		uint8_t psc[8] = {};
		uint8_t palette = 0;
	};

	struct state_block
	{
		device_state values;
		state_mask mask;
		void hold();    // references on the textures and buffers it has
		void let_go();
	};

	class device final : public IDirect3DDevice8
	{
	public:
		device(direct3d *parent, UINT adapter, HWND focus, DWORD behaviour);
		~device();
		bool init(D3DPRESENT_PARAMETERS *pp, std::string &why);

		gpu g;
		direct3d *parent;
		refcount rc;
		UINT adapter;
		HWND focus;
		DWORD behaviour;
		D3DPRESENT_PARAMETERS params = {};
		std::unique_ptr<swap_chain> chain;
		PALETTEENTRY palettes[256][256] = {};
		uint32_t palettes_set = 0;
		bool in_scene = false;
		D3DGAMMARAMP gamma = {};
		bool gamma_set = false;

		// The current targets (referenced)
		surface *rt = nullptr, *ds = nullptr;
		surface *auto_ds = nullptr;

		// Draw-time objects: pipelines by key, shaders by key, declarations by FVF
		struct pso_entry { ID3D12PipelineState *pso = nullptr; };
		std::unordered_map<uint64_t, pso_entry> psos;
		std::unordered_map<uint64_t, const std::vector<uint8_t> *> vs_cache, ps_cache;
		std::unordered_map<DWORD, std::unique_ptr<declaration>> fvf_decls;
		std::vector<std::unique_ptr<vertex_shader>> vshaders; // handle = (index << 1) | 1 ... (2 + index) * 2 + 1
		std::vector<std::unique_ptr<pixel_shader>> pshaders;  // handle = index + 1
		std::vector<std::unique_ptr<state_block>> blocks;     // token = index + 1
		state_block *recording = nullptr;

		// The current state, and where setters write (the recording block's while one records)
		device_state cur;
		device_state &target() { return recording != nullptr ? recording->values : cur; }

		// Images and buffers made, for Reset's checks and statistics
		std::vector<image *> images;
		uint32_t buffers_alive = 0;

		// Counters for the tests and the log
		struct counters
		{
			uint64_t draws = 0, clears = 0, readbacks = 0, presents = 0, pipelines = 0, uploads = 0, upload_bytes = 0;
			uint64_t async_answered = 0, async_unanswered = 0; // asynchronous readbacks answered from an earlier frame, or not yet
		} stats;

		// Asynchronous readbacks (AsyncReadback=1): a copy from a target into memory answered by the same copy (surface,
		// rect, place in the frame's order) from the newest frame the GPU has finished; never waits
		bool async_readback = false;
		bool sync_copies = false; // (GetFrontBuffer: always this frame's)
		struct async_read
		{
			image::pending_read read;
			image *image = nullptr;
			uint64_t key = 0;
		};
		struct async_result
		{
			std::vector<uint8_t> pixels; // the surface's format, w x h
			UINT w = 0, h = 0;
			uint64_t frame = 0;
		};
		std::vector<async_read> async_reads;
		std::unordered_map<uint64_t, async_result> async_results;
		std::unordered_map<const image *, uint32_t> async_index; // copies into each surface this frame
		uint64_t async_key(const image *img, const image::pending_read &p, uint32_t index) const;
		void poll_async(bool all_done_only);

		// Binding cache: what this list has bound (valid while epoch matches the gpu's)
		struct bound_state
		{
			uint64_t epoch = 0;
			D3D12_CPU_DESCRIPTOR_HANDLE rtv = {}, dsv = {};
			bool targets = false;
			D3D12_VIEWPORT viewport = {};
			D3D12_RECT scissor = {};
			bool vp = false;
			ID3D12PipelineState *pso = nullptr;
			D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
			D3D12_GPU_VIRTUAL_ADDRESS vs_cb = 0, ps_cb = 0;
			D3D12_GPU_DESCRIPTOR_HANDLE srvs = {}, samplers = {};
			D3D12_VERTEX_BUFFER_VIEW vbs[kMaxStreams] = {};
			uint32_t vb_count = 0;
			D3D12_INDEX_BUFFER_VIEW ib = {};
			UINT stencil_ref = 0xFFFFFFFF;
		} bound;

		// Dirty flags: what to look at again before the next draw
		enum dirty_bits : uint32_t
		{
			d_ffvs = 1, d_vsc = 2, d_psc = 4, d_textures = 8, d_pipeline = 16, d_targets = 32, d_viewport = 64, d_vs_key = 128, d_ps_key = 256,
			d_all = 0xFFFFFFFF,
		};
		uint32_t dirty = d_all;
		D3D12_GPU_VIRTUAL_ADDRESS vs_cb = 0, ps_cb = 0; // the constants uploaded this frame (0: again)
		uint64_t vs_cb_frame = ~0ull, ps_cb_frame = ~0ull;
		int vs_cb_kind = 0; // 1 the fixed function's constants, 2 a vs.1.1 shader's

		// Helpers the resources use
		D3D12_RESOURCE_STATES read_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		void forget(image *img);
		void unbind(IUnknown *object); // a texture or buffer let go while bound (refs kept: never)

		// Drawing
		HRESULT draw(D3DPRIMITIVETYPE type, UINT count, bool indexed, UINT start, INT base_vertex, UINT min_index, UINT num_vertices,
			const void *up_vertices, UINT up_stride, const void *up_indices, D3DFORMAT up_index_format);
		void bind_targets();
		HRESULT clear(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil);
		void present_frame(HWND override_window, const RECT *src, const RECT *dst);
		void reset_state();
		const declaration *current_declaration(const vertex_shader **vs);
		const declaration *fvf_declaration(DWORD fvf);

		// Copies between surfaces (CopyRects, UpdateTexture, GetFrontBuffer)
		HRESULT copy_rect(surface *src, const RECT &r, surface *dst, POINT at);

		// IDirect3DDevice8
		STDMETHOD(QueryInterface)(REFIID riid, void **ppvObj) override;
		STDMETHOD_(ULONG, AddRef)() override;
		STDMETHOD_(ULONG, Release)() override;
		STDMETHOD(TestCooperativeLevel)() override;
		STDMETHOD_(UINT, GetAvailableTextureMem)() override;
		STDMETHOD(ResourceManagerDiscardBytes)(DWORD Bytes) override;
		STDMETHOD(GetDirect3D)(IDirect3D8 **ppD3D8) override;
		STDMETHOD(GetDeviceCaps)(D3DCAPS8 *pCaps) override;
		STDMETHOD(GetDisplayMode)(D3DDISPLAYMODE *pMode) override;
		STDMETHOD(GetCreationParameters)(D3DDEVICE_CREATION_PARAMETERS *pParameters) override;
		STDMETHOD(SetCursorProperties)(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface8 *pCursorBitmap) override;
		STDMETHOD_(void, SetCursorPosition)(int X, int Y, DWORD Flags) override;
		STDMETHOD_(BOOL, ShowCursor)(BOOL bShow) override;
		STDMETHOD(CreateAdditionalSwapChain)(D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DSwapChain8 **pSwapChain) override;
		STDMETHOD(Reset)(D3DPRESENT_PARAMETERS *pPresentationParameters) override;
		STDMETHOD(Present)(CONST RECT *pSourceRect, CONST RECT *pDestRect, HWND hDestWindowOverride, CONST RGNDATA *pDirtyRegion) override;
		STDMETHOD(GetBackBuffer)(UINT BackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface8 **ppBackBuffer) override;
		STDMETHOD(GetRasterStatus)(D3DRASTER_STATUS *pRasterStatus) override;
		STDMETHOD_(void, SetGammaRamp)(DWORD Flags, CONST D3DGAMMARAMP *pRamp) override;
		STDMETHOD_(void, GetGammaRamp)(D3DGAMMARAMP *pRamp) override;
		STDMETHOD(CreateTexture)(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture8 **ppTexture) override;
		STDMETHOD(CreateVolumeTexture)(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture8 **ppVolumeTexture) override;
		STDMETHOD(CreateCubeTexture)(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture8 **ppCubeTexture) override;
		STDMETHOD(CreateVertexBuffer)(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer8 **ppVertexBuffer) override;
		STDMETHOD(CreateIndexBuffer)(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer8 **ppIndexBuffer) override;
		STDMETHOD(CreateRenderTarget)(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, BOOL Lockable, IDirect3DSurface8 **ppSurface) override;
		STDMETHOD(CreateDepthStencilSurface)(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, IDirect3DSurface8 **ppSurface) override;
		STDMETHOD(CreateImageSurface)(UINT Width, UINT Height, D3DFORMAT Format, IDirect3DSurface8 **ppSurface) override;
		STDMETHOD(CopyRects)(IDirect3DSurface8 *pSourceSurface, CONST RECT *pSourceRectsArray, UINT cRects, IDirect3DSurface8 *pDestinationSurface, CONST POINT *pDestPointsArray) override;
		STDMETHOD(UpdateTexture)(IDirect3DBaseTexture8 *pSourceTexture, IDirect3DBaseTexture8 *pDestinationTexture) override;
		STDMETHOD(GetFrontBuffer)(IDirect3DSurface8 *pDestSurface) override;
		STDMETHOD(SetRenderTarget)(IDirect3DSurface8 *pRenderTarget, IDirect3DSurface8 *pNewZStencil) override;
		STDMETHOD(GetRenderTarget)(IDirect3DSurface8 **ppRenderTarget) override;
		STDMETHOD(GetDepthStencilSurface)(IDirect3DSurface8 **ppZStencilSurface) override;
		STDMETHOD(BeginScene)() override;
		STDMETHOD(EndScene)() override;
		STDMETHOD(Clear)(DWORD Count, CONST D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) override;
		STDMETHOD(SetTransform)(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX *pMatrix) override;
		STDMETHOD(GetTransform)(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix) override;
		STDMETHOD(MultiplyTransform)(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX *pMatrix) override;
		STDMETHOD(SetViewport)(CONST D3DVIEWPORT8 *pViewport) override;
		STDMETHOD(GetViewport)(D3DVIEWPORT8 *pViewport) override;
		STDMETHOD(SetMaterial)(CONST D3DMATERIAL8 *pMaterial) override;
		STDMETHOD(GetMaterial)(D3DMATERIAL8 *pMaterial) override;
		STDMETHOD(SetLight)(DWORD Index, CONST D3DLIGHT8 *pLight) override;
		STDMETHOD(GetLight)(DWORD Index, D3DLIGHT8 *pLight) override;
		STDMETHOD(LightEnable)(DWORD Index, BOOL Enable) override;
		STDMETHOD(GetLightEnable)(DWORD Index, BOOL *pEnable) override;
		STDMETHOD(SetClipPlane)(DWORD Index, CONST float *pPlane) override;
		STDMETHOD(GetClipPlane)(DWORD Index, float *pPlane) override;
		STDMETHOD(SetRenderState)(D3DRENDERSTATETYPE State, DWORD Value) override;
		STDMETHOD(GetRenderState)(D3DRENDERSTATETYPE State, DWORD *pValue) override;
		STDMETHOD(BeginStateBlock)() override;
		STDMETHOD(EndStateBlock)(DWORD *pToken) override;
		STDMETHOD(ApplyStateBlock)(DWORD Token) override;
		STDMETHOD(CaptureStateBlock)(DWORD Token) override;
		STDMETHOD(DeleteStateBlock)(DWORD Token) override;
		STDMETHOD(CreateStateBlock)(D3DSTATEBLOCKTYPE Type, DWORD *pToken) override;
		STDMETHOD(SetClipStatus)(CONST D3DCLIPSTATUS8 *pClipStatus) override;
		STDMETHOD(GetClipStatus)(D3DCLIPSTATUS8 *pClipStatus) override;
		STDMETHOD(GetTexture)(DWORD Stage, IDirect3DBaseTexture8 **ppTexture) override;
		STDMETHOD(SetTexture)(DWORD Stage, IDirect3DBaseTexture8 *pTexture) override;
		STDMETHOD(GetTextureStageState)(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) override;
		STDMETHOD(SetTextureStageState)(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override;
		STDMETHOD(ValidateDevice)(DWORD *pNumPasses) override;
		STDMETHOD(GetInfo)(DWORD DevInfoID, void *pDevInfoStruct, DWORD DevInfoStructSize) override;
		STDMETHOD(SetPaletteEntries)(UINT PaletteNumber, CONST PALETTEENTRY *pEntries) override;
		STDMETHOD(GetPaletteEntries)(UINT PaletteNumber, PALETTEENTRY *pEntries) override;
		STDMETHOD(SetCurrentTexturePalette)(UINT PaletteNumber) override;
		STDMETHOD(GetCurrentTexturePalette)(UINT *PaletteNumber) override;
		STDMETHOD(DrawPrimitive)(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) override;
		STDMETHOD(DrawIndexedPrimitive)(D3DPRIMITIVETYPE PrimitiveType, UINT minIndex, UINT NumVertices, UINT startIndex, UINT primCount) override;
		STDMETHOD(DrawPrimitiveUP)(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, CONST void *pVertexStreamZeroData, UINT VertexStreamZeroStride) override;
		STDMETHOD(DrawIndexedPrimitiveUP)(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertexIndices, UINT PrimitiveCount, CONST void *pIndexData, D3DFORMAT IndexDataFormat, CONST void *pVertexStreamZeroData, UINT VertexStreamZeroStride) override;
		STDMETHOD(ProcessVertices)(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer8 *pDestBuffer, DWORD Flags) override;
		STDMETHOD(CreateVertexShader)(CONST DWORD *pDeclaration, CONST DWORD *pFunction, DWORD *pHandle, DWORD Usage) override;
		STDMETHOD(SetVertexShader)(DWORD Handle) override;
		STDMETHOD(GetVertexShader)(DWORD *pHandle) override;
		STDMETHOD(DeleteVertexShader)(DWORD Handle) override;
		STDMETHOD(SetVertexShaderConstant)(DWORD Register, CONST void *pConstantData, DWORD ConstantCount) override;
		STDMETHOD(GetVertexShaderConstant)(DWORD Register, void *pConstantData, DWORD ConstantCount) override;
		STDMETHOD(GetVertexShaderDeclaration)(DWORD Handle, void *pData, DWORD *pSizeOfData) override;
		STDMETHOD(GetVertexShaderFunction)(DWORD Handle, void *pData, DWORD *pSizeOfData) override;
		STDMETHOD(SetStreamSource)(UINT StreamNumber, IDirect3DVertexBuffer8 *pStreamData, UINT Stride) override;
		STDMETHOD(GetStreamSource)(UINT StreamNumber, IDirect3DVertexBuffer8 **ppStreamData, UINT *pStride) override;
		STDMETHOD(SetIndices)(IDirect3DIndexBuffer8 *pIndexData, UINT BaseVertexIndex) override;
		STDMETHOD(GetIndices)(IDirect3DIndexBuffer8 **ppIndexData, UINT *pBaseVertexIndex) override;
		STDMETHOD(CreatePixelShader)(CONST DWORD *pFunction, DWORD *pHandle) override;
		STDMETHOD(SetPixelShader)(DWORD Handle) override;
		STDMETHOD(GetPixelShader)(DWORD *pHandle) override;
		STDMETHOD(DeletePixelShader)(DWORD Handle) override;
		STDMETHOD(SetPixelShaderConstant)(DWORD Register, CONST void *pConstantData, DWORD ConstantCount) override;
		STDMETHOD(GetPixelShaderConstant)(DWORD Register, void *pConstantData, DWORD ConstantCount) override;
		STDMETHOD(GetPixelShaderFunction)(DWORD Handle, void *pData, DWORD *pSizeOfData) override;
		STDMETHOD(DrawRectPatch)(UINT Handle, CONST float *pNumSegs, CONST D3DRECTPATCH_INFO *pRectPatchInfo) override;
		STDMETHOD(DrawTriPatch)(UINT Handle, CONST float *pNumSegs, CONST D3DTRIPATCH_INFO *pTriPatchInfo) override;
		STDMETHOD(DeletePatch)(UINT Handle) override;

	private:
		void make_state_mask(D3DSTATEBLOCKTYPE type, state_mask &m);
		void apply(const state_block &b);
		void capture(state_block &b);
		vertex_shader *vshader(DWORD handle);
		pixel_shader *pshader(DWORD handle);
		void set_texture_ref(IDirect3DBaseTexture8 *&slot, IDirect3DBaseTexture8 *t);
		void make_auto_depth();
		// The device's own references on its targets: none on its own back buffer and depth buffer (theirs would be
		// the device's, which would then never go)
		void hold_surface(surface *s);
		void drop_surface(surface *s);
	};

	// The device's own draw-time pieces (device_draw.cpp)
	void fill_ff_constants(const device_state &s, const D3DVIEWPORT8 &vp, ff_vs_constants &c);
	void fill_ps_constants(const device_state &s, ps_constants &c);
	DWORD default_render_state(DWORD state, bool auto_depth);
	DWORD default_stage_state(DWORD stage, DWORD type);
}
