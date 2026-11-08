#include "MouseInput.h"

#include "MainThread.h"
#include "ToolRegistry.h"

#include <cmath>

namespace dvb
{
	namespace
	{
		constexpr double kMaxCoordinate = 16384.0;

		struct WindowSearch
		{
			DWORD pid;
			HWND  found;
		};

		BOOL CALLBACK PickGameWindow(HWND a_hwnd, LPARAM a_param)
		{
			auto* search = reinterpret_cast<WindowSearch*>(a_param);
			DWORD pid = 0;
			::GetWindowThreadProcessId(a_hwnd, &pid);
			if (pid != search->pid || !::IsWindowVisible(a_hwnd) || ::GetWindow(a_hwnd, GW_OWNER))
				return TRUE;
			RECT rc{};
			::GetClientRect(a_hwnd, &rc);
			if (rc.right <= 0 || rc.bottom <= 0)
				return TRUE;
			search->found = a_hwnd;
			return FALSE;
		}

		HWND FindGameWindow()
		{
			WindowSearch search{ ::GetCurrentProcessId(), nullptr };
			::EnumWindows(PickGameWindow, reinterpret_cast<LPARAM>(&search));
			return search.found;
		}

		bool BringToForeground(HWND a_hwnd)
		{
			if (::GetForegroundWindow() == a_hwnd)
				return true;
			const DWORD thisThread = ::GetCurrentThreadId();
			const DWORD foregroundThread = ::GetWindowThreadProcessId(::GetForegroundWindow(), nullptr);
			const bool  attached = foregroundThread && foregroundThread != thisThread && ::AttachThreadInput(thisThread, foregroundThread, TRUE);
			if (::IsIconic(a_hwnd))
				::ShowWindow(a_hwnd, SW_RESTORE);
			::SetForegroundWindow(a_hwnd);
			::BringWindowToTop(a_hwnd);
			if (attached)
				::AttachThreadInput(thisThread, foregroundThread, FALSE);
			return ::GetForegroundWindow() == a_hwnd;
		}

		double CoordinateArgument(const json& a_args, const char* a_name)
		{
			if (!a_args.contains(a_name) || !a_args[a_name].is_number())
				throw ToolError(400, std::format("mouse move requires a numeric '{}' (client pixels)", a_name));
			const double value = a_args[a_name].get<double>();
			if (!std::isfinite(value) || value < 0.0 || value > kMaxCoordinate)
				throw ToolError(400, std::format("'{}' must be between 0 and {}", a_name, static_cast<int>(kMaxCoordinate)));
			return value;
		}

		json CursorState(HWND a_hwnd)
		{
			RECT rc{};
			if (a_hwnd)
				::GetClientRect(a_hwnd, &rc);
			json out{ { "windowFound", a_hwnd != nullptr }, { "windowFocused", a_hwnd && ::GetForegroundWindow() == a_hwnd },
				{ "client", json{ { "width", rc.right }, { "height", rc.bottom } } } };
			if (auto* cursor = RE::MenuCursor::GetSingleton()) {
				const auto& data = cursor->GetRuntimeData();
				out["menuCursor"] = json{ { "x", data.cursorPosX }, { "y", data.cursorPosY }, { "width", data.screenWidthX }, { "height", data.screenWidthY } };
			}
			POINT os{};
			if (a_hwnd && ::GetCursorPos(&os) && ::ScreenToClient(a_hwnd, &os))
				out["osCursor"] = json{ { "x", os.x }, { "y", os.y } };
			return out;
		}
	}

	json HandleMousePointer(const json& a_args)
	{
		const std::string action = a_args.value("action", std::string{});
		HWND              hwnd = FindGameWindow();
		if (action == "status") {
			json out = CursorState(hwnd);
			out["device"] = "mouse";
			return out;
		}

		const double x = CoordinateArgument(a_args, "x");
		const double y = CoordinateArgument(a_args, "y");
		if (!hwnd)
			throw ToolError(409, "game window not found");
		RECT client{};
		::GetClientRect(hwnd, &client);
		if (x >= client.right || y >= client.bottom)
			throw ToolError(400, std::format("({}, {}) is outside the {}x{} client area", x, y, client.right, client.bottom));

		// The engine's own menu cursor (Scaleform menus) is set on the main thread, scaled from client pixels.
		MainThread::RunAndWait([x, y, client]() -> json {
			if (auto* cursor = RE::MenuCursor::GetSingleton()) {
				auto& data = cursor->GetRuntimeData();
				data.cursorPosX = static_cast<float>(x * data.screenWidthX / client.right);
				data.cursorPosY = static_cast<float>(y * data.screenWidthY / client.bottom);
			}
			return json::object();
		});

		// Overlays that read the OS cursor (ImGui) only look at it while the game window is the foreground window.
		bool osCursorSet = false;
		if (a_args.value("focusWindow", false)) {
			if (!BringToForeground(hwnd))
				throw ToolError(409, "could not bring the game window to the foreground (focusWindow); the engine cursor was moved but the OS cursor was not");
			POINT point{ static_cast<LONG>(x), static_cast<LONG>(y) };
			osCursorSet = ::ClientToScreen(hwnd, &point) && ::SetCursorPos(point.x, point.y);
		}

		json out = CursorState(hwnd);
		out["device"] = "mouse";
		out["action"] = "move";
		out["requested"] = json{ { "x", x }, { "y", y } };
		out["osCursorSet"] = osCursorSet;
		return out;
	}
}
