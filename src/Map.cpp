#include "Map.h"

#include "FormLookup.h"
#include "GameText.h"
#include "MainThread.h"
#include "MapLogic.h"

namespace dvb::Map
{
	namespace
	{
		struct Place
		{
			RE::NiPoint3       position;
			RE::TESWorldSpace* worldspace = nullptr;
		};

		json FormJson(const RE::TESForm* a_form)
		{
			if (!a_form)
				return nullptr;
			json out{ { "formId", std::format("0x{:08X}", a_form->GetFormID()) } };
			if (const char* editorId = a_form->GetFormEditorID(); editorId && *editorId)
				out["editorId"] = editorId;
			if (const char* name = a_form->GetName(); name && *name)
				out["name"] = FromGame(name);
			return out;
		}

		json PlaceJson(const Place& a_place)
		{
			return { { "x", a_place.position.x }, { "y", a_place.position.y }, { "z", a_place.position.z }, { "worldspace", FormJson(a_place.worldspace) } };
		}

		void SetWaypoint(RE::PlayerCharacter* a_player, const RE::NiPoint3& a_position, RE::TESForm* a_worldSpaceOrCell)
		{
			using func_t = void(RE::PlayerCharacter*, const RE::NiPoint3*, RE::TESForm*);
			static REL::Relocation<func_t> func{ RELOCATION_ID(39458, 40535) };
			func(a_player, &a_position, a_worldSpaceOrCell);
		}

		void RemoveWaypoint(RE::PlayerCharacter* a_player)
		{
			using func_t = void(RE::PlayerCharacter*);
			static REL::Relocation<func_t> func{ RELOCATION_ID(39459, 40536) };
			func(a_player);
		}

		RE::PlayerCharacter* RequirePlayer()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player)
				throw ToolError(409, "player not ready: load a game first");
			return player;
		}

		// On VR the shared player runtime data accessor points at the SE/AE layout and reads the wrong bytes.
		RE::ObjectRefHandle MarkerHandle(RE::PlayerCharacter* a_player)
		{
			if (REL::Module::IsVR())
				return a_player->GetVRInfoRuntimeData()->playerMapMarker;
			return a_player->GetPlayerRuntimeData().playerMapMarker;
		}

		std::optional<Place> CurrentWaypoint(RE::PlayerCharacter* a_player)
		{
			const auto marker = MarkerHandle(a_player).get();
			if (!marker)
				return std::nullopt;
			return Place{ marker->GetPosition(), marker->GetWorldspace() };
		}

		RE::BGSLocation* RequireLocation(const std::string& a_ref)
		{
			auto* form = FormLookup::LookupFormArg(a_ref);
			if (!form)
				throw ToolError(404, "location not found: " + a_ref);
			auto* location = form->As<RE::BGSLocation>();
			if (!location)
				throw ToolError(400, std::format("{} is a {}, not a Location", a_ref, RE::FormTypeToString(form->GetFormType())));
			return location;
		}

		Place LocationPlace(const RE::BGSLocation* a_location, const std::string& a_ref)
		{
			const auto marker = a_location->worldLocMarker.get();
			if (!marker)
				throw ToolError(404, std::format("{} has no world marker loaded: it has none, or its reference is not loaded", a_ref));
			Place place{ marker->GetPosition(), marker->GetWorldspace() };
			if (!place.worldspace)
				throw ToolError(422, std::format("the world marker of {} is not in a worldspace", a_ref));
			return place;
		}

		RE::TESWorldSpace* ResolveWorldspace(const std::string& a_ref, RE::PlayerCharacter* a_player)
		{
			if (a_ref.empty()) {
				auto* current = a_player->GetWorldspace();
				if (!current)
					throw ToolError(409, "the player is not in a worldspace: pass worldspace");
				return current;
			}
			auto* form = FormLookup::LookupFormArg(a_ref);
			if (!form)
				throw ToolError(404, "worldspace not found: " + a_ref);
			auto* worldspace = form->As<RE::TESWorldSpace>();
			if (!worldspace)
				throw ToolError(400, std::format("{} is a {}, not a WorldSpace", a_ref, RE::FormTypeToString(form->GetFormType())));
			return worldspace;
		}

		json WaypointJson(const std::optional<Place>& a_place)
		{
			if (!a_place)
				return { { "set", false } };
			json out = PlaceJson(*a_place);
			out["set"] = true;
			return out;
		}

		json Locate(const MapLogic::Request& a_request)
		{
			const auto* location = RequireLocation(a_request.location);
			json        out = PlaceJson(LocationPlace(location, a_request.location));
			out["location"] = FormJson(location);
			out["radius"] = location->worldLocRadius;
			return out;
		}

		json SetWaypointAction(const MapLogic::Request& a_request)
		{
			auto* player = RequirePlayer();
			Place target;
			if (a_request.position) {
				const auto& pos = *a_request.position;
				target = { RE::NiPoint3{ pos[0], pos[1], pos[2] }, ResolveWorldspace(a_request.worldspace, player) };
			} else {
				target = LocationPlace(RequireLocation(a_request.location), a_request.location);
			}

			const auto previous = CurrentWaypoint(player);
			SetWaypoint(player, target.position, target.worldspace);
			const auto current = CurrentWaypoint(player);
			if (!current)
				throw ToolError(500, "the game did not create the map marker");
			return { { "ok", true }, { "waypoint", WaypointJson(current) }, { "previous", WaypointJson(previous) } };
		}

		json ClearWaypointAction()
		{
			auto*      player = RequirePlayer();
			const auto previous = CurrentWaypoint(player);
			if (previous)
				RemoveWaypoint(player);
			return { { "ok", true }, { "cleared", previous.has_value() }, { "previous", WaypointJson(previous) } };
		}

		json Run(const MapLogic::Request& a_request)
		{
			using MapLogic::Action;
			switch (a_request.action) {
			case Action::kLocate:
				return Locate(a_request);
			case Action::kSetWaypoint:
				return SetWaypointAction(a_request);
			case Action::kClearWaypoint:
				return ClearWaypointAction();
			case Action::kGet:
				return WaypointJson(CurrentWaypoint(RequirePlayer()));
			}
			throw ToolError(500, "unhandled map action");
		}
	}

	ToolDescriptor BuildDescriptor()
	{
		ToolDescriptor map;
		map.name = "map";
		map.description =
			"The player's custom map marker (the waypoint on the map and compass) and Location world markers. "
			"action='get' reads the current waypoint { set, x, y, z, worldspace }. 'setWaypoint' places it at a "
			"Location's world marker (location) or at x, y, z in a worldspace (default: the player's current "
			"one), exactly as a map click does, and returns the final waypoint plus the previous one. "
			"'clearWaypoint' removes it. 'locate' only reads a Location's world marker position, worldspace and "
			"radius. A Location whose world marker is not loaded answers 404.";
		map.inputSchema = json{
			{ "type", "object" },
			{ "required", json::array({ "action" }) },
			{ "properties", json{
								{ "action", json{ { "type", "string" }, { "enum", json::array({ "get", "locate", "setWaypoint", "clearWaypoint" }) } } },
								{ "location", json{ { "type", "string" }, { "description", "locate/setWaypoint: Location EditorID or FormID, for example 'WhiterunLocation' or '0x18A56'" } } },
								{ "x", json{ { "type", "number" }, { "description", "setWaypoint: world X; give x and y instead of location" } } },
								{ "y", json{ { "type", "number" }, { "description", "setWaypoint: world Y" } } },
								{ "z", json{ { "type", "number" }, { "description", "setWaypoint: world Z; default 0" } } },
								{ "worldspace", json{ { "type", "string" }, { "description", "setWaypoint with x and y: WorldSpace EditorID or FormID; default the player's current worldspace" } } },
							} },
		};
		return map;
	}

	json Handle(const json& a_args, const ToolContext&)
	{
		const auto parsed = MapLogic::Parse(a_args);
		if (!parsed.request)
			throw ToolError(400, parsed.error);
		return MainThread::RunAndWait([request = *parsed.request]() { return Run(request); }, std::chrono::milliseconds(5000));
	}
}
