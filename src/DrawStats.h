#pragma once

#include "Json.h"

namespace dvb::DrawStats
{
	// Counts and times what each frame draws: objects per BSShader type in the main pass and inside each kind of
	// shadow-casting light's Render, the main pass broken down by the render target it draws into, shadow casters by
	// size on screen, and the GPU time of each shadow kind and target (D3D11 timestamp queries, read a few frames
	// late without stalling). Nothing is hooked until the first read; after that the hooks stay for the session.

	/// Main thread. Installs the hooks on the first call. The reading covers the frames since the counting window
	/// began; `a_reset` begins a new window after this reading.
	json Read(bool a_reset);

	/// A provider answers `inspect kind=draws` from now on: the hooks are never installed, and hooks already installed
	/// pass every call straight through (a vtable slot is never restored - another mod may have hooked over it).
	/// Thread-safe; Read is never called after it.
	void StandDown();
}
