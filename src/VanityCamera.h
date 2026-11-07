#pragma once

namespace dvb::VanityCamera
{
	// Stops the engine's idle auto-vanity camera by replacing the idle-timeout call inside PlayerCamera::Update.
	// Camera action setPov='vanity' still works. SE and AE only.
	void Install();
}
