#pragma once
// HiFi XI's one addition to AMD's frame interpolation swap chain (FrameInterpolationSwapchainDX12.cpp): called first
// thing on its interpolation and presenter threads, when set (framegen::set_thread_start), before they record or
// present anything. HiFi XI's hooks keep quiet there: nothing those threads record is the game's world, and DXGI's own
// list, reset inside the presenter thread's Present, would otherwise wait on World FX's lock while the game's thread
// held it and waited on Direct3D's (the debug layer's) lock that Present held: a deadlock (2026-11-02).
extern "C" void (*ffx_fg_thread_start)();
