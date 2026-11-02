// The helper's inline ray tracing: spp rays a pixel against the terrain grid. Hit: triangle id / hit share / distance;
// miss: blue = the frame number the host put in shared memory (so the host can tell the answer is this frame's).
#define RS "RootConstants(num32BitConstants=4, b0), SRV(t0), DescriptorTable(UAV(u0))"

cbuffer C : register(b0) { uint frame; uint spp; uint width; uint height; };
RaytracingAccelerationStructure scene : register(t0);
RWTexture2D<float4> outp : register(u0);

float hash(uint2 p, uint s)
{
	uint h = p.x * 73856093u ^ p.y * 19349663u ^ s * 83492791u;
	h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
	return (h & 0xffffu) / 65535.0 - 0.5;
}

[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
	if (id.x >= width || id.y >= height)
		return;
	float2 uv = (id + 0.5) / float2(width, height);
	float hits = 0, t = 0;
	uint first = 0;
	for (uint s = 0; s < spp; ++s)
	{
		RayDesc r;
		r.Origin = float3(0, 40, -60);
		r.Direction = normalize(float3((uv.x - 0.5) * 1.6 + hash(id, s) * 0.01, -(uv.y - 0.5) - 0.5 + hash(id, s + 7) * 0.01, 1));
		r.TMin = 0.01;
		r.TMax = 10000;
		RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
		q.TraceRayInline(scene, 0, 0xff, r);
		q.Proceed();
		if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
		{
			if (hits == 0)
				first = q.CommittedPrimitiveIndex();
			hits += 1;
			t += q.CommittedRayT();
		}
	}
	if (hits > 0)
		outp[id] = float4(frac(first * 0.618), hits / spp, saturate(t / hits / 200), 1);
	else
		outp[id] = float4(0, 0, (frame & 255) / 255.0, 1);
}
