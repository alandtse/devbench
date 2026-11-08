#pragma once

#include "Json.h"

namespace dvb
{
	/// input device='mouse' pointer actions: move (game-window client pixels) and status. Buttons go through the
	/// keyboard path as mouseLeft/mouseRight/mouseMiddle.
	json HandleMousePointer(const json& a_args);
}
