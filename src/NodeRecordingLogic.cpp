#include "NodeRecordingLogic.h"

#include <algorithm>
#include <format>

namespace dvb::NodeRecordingLogic
{
	std::optional<std::vector<std::string>> ParseNodeNames(const json& a_nodes, std::string& a_error)
	{
		if (!a_nodes.is_array() || a_nodes.empty()) {
			a_error = "'nodes' must be a non-empty array of node names";
			return std::nullopt;
		}
		if (a_nodes.size() > kMaxNodes) {
			a_error = std::format("'nodes' has {} names, at most {} are allowed", a_nodes.size(), kMaxNodes);
			return std::nullopt;
		}
		std::vector<std::string> names;
		for (const auto& item : a_nodes) {
			if (!item.is_string() || item.get<std::string>().empty()) {
				a_error = "'nodes' entries must be non-empty strings";
				return std::nullopt;
			}
			auto name = item.get<std::string>();
			if (name.size() > kMaxNameLength) {
				a_error = std::format("a node name is longer than {} characters", kMaxNameLength);
				return std::nullopt;
			}
			if (std::find(names.begin(), names.end(), name) != names.end()) {
				a_error = std::format("node '{}' is listed twice", name);
				return std::nullopt;
			}
			names.push_back(std::move(name));
		}
		return names;
	}
}
