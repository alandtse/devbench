// SPDX-License-Identifier: MIT
// MIT-licensed (see DevBenchAPI.LICENSE.txt) so any plugin may vendor it; the devbench
// plugin itself is GPL-3.0. Compile this in YOUR plugin only (not in devbench).
#include "DevBenchAPI.h"

// Consumer-side helper — compile this in YOUR plugin (devbench itself does not build
// this file). Mirrors MergeMapper's GetMergeMapperInterface001().
DevBenchAPI::IDevBenchInterface001* g_devBenchInterface = nullptr;

namespace DevBenchAPI
{
	IDevBenchInterface001* GetDevBenchInterface001()
	{
		if (g_devBenchInterface)
			return g_devBenchInterface;

		const auto messaging = SKSE::GetMessagingInterface();
		if (!messaging)
			return nullptr;

		// Synchronous: dispatching to the named provider invokes its listener inline,
		// which fills message.GetApiFunction in this stack struct.
		DevBenchMessage message;
		messaging->Dispatch(DevBenchMessage::kMessage_GetInterface, &message,
			sizeof(DevBenchMessage*), DevBenchPluginName);
		if (!message.GetApiFunction)
			return nullptr;

		g_devBenchInterface = static_cast<IDevBenchInterface001*>(message.GetApiFunction(1));
		return g_devBenchInterface;
	}
}
