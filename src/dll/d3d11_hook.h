#pragma once

// Hooks IDXGISwapChain::Present / Present1 / ResizeBuffers process-wide.
// Present is the "here's a finished frame" call every D3D11 game makes each
// frame — our hook is where all VR work happens.

bool InstallD3D11Hooks();

// --- Desktop-window fit (config.fit_desktop_window) -------------------------
// The forced full-render backbuffer size (0,0 when the fit is off or not yet
// initialized). menu.cpp uses it while rewriting MCC's WM_SIZE transaction so
// the engine keeps drawing the full headset render into the fitted window.
void D3D_GetForcedRenderSize(unsigned& width, unsigned& height);

// True only when the desktop fit is on AND its hooks installed at startup. All
// the window-shrink behavior in menu.cpp is gated on this (not the live config
// flag), so ticking the restart-required checkbox mid-session can't half-engage
// the fit without the backbuffer force behind it.
bool D3D_FitActive();

// menu.cpp brackets the game's own WM_SIZE handling with this. While set, our
// GetClientRect hook returns the full render size to the game's resize code on
// THAT call stack only (it is thread-local), so MCC keeps sizing its render to
// the full backbuffer. DXGI/DWM present-time client queries -- on the render
// thread -- never see it, so they still downscale the full frame into the real,
// smaller window instead of clipping it to a corner.
void D3D_SetForcedClientLie(bool on);

// --- Co-op drop probe (config.coop_probe) -----------------------------------
// The VR frame is submitted inside MCC's own Present, on MCC's render thread --
// the thread MCC paces its synchronised campaign co-op simulation from. This
// records how long the mod holds that thread each frame and dumps the run-up
// when a level is torn down, so a real co-op kick leaves measurements behind
// instead of a theory. `reason` names the mode we fell out of.
void CoopProbe_DumpRunUp(const char* reason);
