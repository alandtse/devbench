#pragma once

namespace RE
{
	class MessageBoxData;
}

// Captures the active modal by detouring MessageBoxMenu::QueueMessage — the single
// chokepoint every message box passes through — so the `menu` tool can read a blocking
// Yes/No dialog and select a button (e.g. accept the "content no longer present"
// confirmation that otherwise gates an automated load). The detour only records the
// latest queued MessageBoxData*; consumers guard reads on the menu actually being open.
namespace dvb::ModalCapture
{
	/// Install the QueueMessage detour. Idempotent; the SKSE trampoline must have space.
	void Install();

	/// The most-recently-queued message-box data, or nullptr. Only meaningful while a
	/// MessageBoxMenu is open — the pointer is owned by the game.
	RE::MessageBoxData* Current();

	/// Forget the captured pointer (e.g. when the menu closes).
	void Clear();
}
