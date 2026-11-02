# rtprobe: ray tracing from a 64-bit helper for the 32-bit game

FFXI's 32-bit process gets D3D12 ray tracing tier 0 (no DXR). This probe checks that a 64-bit helper on the same GPU
can trace rays for it and measures what the split costs.

- `host32.exe` (x86, the game's side): makes a shared texture, two shared fences (req: host -> helper, done: helper ->
  host) and a shared memory page, all inheritable NT handles, and starts `helper64.exe` with their values.
- `helper64.exe` (x64): builds a BLAS/TLAS over 204,800 triangles and traces into the texture with inline ray tracing
  (`rt.hlsl`, RayQuery, cs_6_5).

`b.cmd` builds `rt.cso`, `host32.exe` and `helper64.exe` (dxc from Windows SDK 10.0.26100). Run `host32.exe` with the
game closed. `RTP_COPIES` (host work, default 12) and `RTP_SPP` (helper rays a pixel, default 16) change phase 3.

Phases: 1 the host reads the helper's picture (must PASS), 2 round-trip latency, 3 frame cost: host work alone, then
with the helper used a frame late or waited for in the same frame, the helper woken by the CPU or queued ahead.

## Results, 2026-11-02 (RTX 4070 Ti, driver 617.42, three runs)

- Host: DXR tier 0. Helper: tier 1.2. Phase 1 PASS, 0 wrong pixels.
- Round trip: 0.17-0.19 ms, of which the helper's GPU work 0.09 ms; the helper's CPU wakes 0.005 ms after the signal.
- Phase 3 (host frame 2.06 ms; helper 16.8 M rays, about 1.5 ms), overhead beyond host + helper work:

| Result used        | helper woken by the CPU | helper queued ahead |
|--------------------|-------------------------|---------------------|
| a frame late       | +0.24 ms                | +0.15 ms            |
| in the same frame  | +0.28-0.36 ms           | +0.22-0.25 ms       |

The host's work here is texture copies, not a real game frame; AMD cards not tried.
