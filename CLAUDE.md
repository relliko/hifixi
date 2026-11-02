# HiFi XI

Before changing GPU code (anything that records a command list, makes or frees a GPU object, adds a queue, fence,
descriptor heap, root signature, pipeline or thread, or touches barriers), read the page for it in docs/d3d12/:

- docs/d3d12/sync.md: queues, fences, when an object may be released (host/retire.hpp, the +2 rule; copy queues)
- docs/d3d12/barriers.md: resource states, planes, promotion and decay, what our d3d8.dll hands over
- docs/d3d12/descriptors.md: descriptor heaps, root signatures and their limits
- docs/d3d12/threads-pso.md: what is free-threaded, never holding our locks across D3D12/DXGI calls, pipelines
- docs/d3d12/debugging.md: debug layer, GPU-based validation, DRED, our breadcrumbs, reading a hang report

Each page lists the rules, where HiFi XI relies on them (file:line, as of dev-hdr 6d321d5: check they still point
there) and the mistakes this project made. Keep them true: when a change moves or breaks what a page says, fix the page
in the same commit.

Tests: `build.cmd` builds everything; `python -I tests\run_suites.py` runs every offline suite.
