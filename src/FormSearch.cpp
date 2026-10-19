#include "FormSearch.h"

#include "AssetInfo.h"
#include "FormSearchLogic.h"
#include "ToolRegistry.h"

#include <RE/A/Actor.h>
#include <RE/B/BGSHeadPart.h>
#include <RE/B/BGSListForm.h>
#include <RE/T/TESFile.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESIdleForm.h>
#include <RE/T/TESNPC.h>
#include <RE/T/TESObjectARMA.h>
#include <RE/T/TESObjectARMO.h>

#include <algorithm>
#include <vector>

namespace dvb::FormSearch
{
	namespace
	{
		using FormSearchLogic::ContainsCI;

		struct TypeEntry
		{
			const char*  name;
			RE::FormType type;
		};
		constexpr TypeEntry kTypes[]{
			{ "Armor", RE::FormType::Armor },
			{ "ArmorAddon", RE::FormType::Armature },
			{ "HeadPart", RE::FormType::HeadPart },
			{ "Idle", RE::FormType::Idle },
			{ "NPC", RE::FormType::NPC },
			{ "Weapon", RE::FormType::Weapon },
			{ "Misc", RE::FormType::Misc },
		};

		std::string Hex(std::uint32_t a_value, int a_width = 8)
		{
			return std::format("0x{:0{}X}", a_value, a_width);
		}

		std::string Str(const char* a_text)
		{
			return a_text ? a_text : "";
		}

		json Identity(RE::TESForm* a_form)
		{
			json out{ { "formId", Hex(a_form->GetFormID()) }, { "formType", Str(RE::FormTypeToString(a_form->GetFormType()).data()) } };
			if (const auto* file = a_form->GetFile(0)) {
				out["plugin"] = std::string(file->GetFilename());
				out["localId"] = Hex(a_form->GetLocalFormID(), 6);
			}
			out["editorId"] = Str(a_form->GetFormEditorID());
			out["name"] = Str(a_form->GetName());
			return out;
		}

		json Brief(RE::TESForm* a_form)
		{
			return a_form ? Identity(a_form) : json(nullptr);
		}

		std::string PluginOf(RE::TESForm* a_form)
		{
			const auto* file = a_form->GetFile(0);
			return file ? std::string(file->GetFilename()) : std::string{};
		}

		std::string ModelOf(RE::TESModel& a_model)
		{
			return Str(a_model.GetModel());
		}

		std::vector<std::string> ModelPaths(RE::TESForm* a_form)
		{
			std::vector<std::string> paths;
			auto                     add = [&](std::string a_path) {
				if (!a_path.empty())
					paths.push_back(std::move(a_path));
			};
			if (auto* part = a_form->As<RE::BGSHeadPart>()) {
				add(ModelOf(*part));
			} else if (auto* addon = a_form->As<RE::TESObjectARMA>()) {
				add(ModelOf(addon->bipedModels[RE::SEXES::kMale]));
				add(ModelOf(addon->bipedModels[RE::SEXES::kFemale]));
			} else if (auto* armor = a_form->As<RE::TESObjectARMO>()) {
				for (auto* addon2 : armor->armorAddons)
					if (addon2) {
						add(ModelOf(addon2->bipedModels[RE::SEXES::kMale]));
						add(ModelOf(addon2->bipedModels[RE::SEXES::kFemale]));
					}
			}
			return paths;
		}

		std::optional<std::uint32_t> SlotMaskOf(RE::TESForm* a_form)
		{
			if (auto* armor = a_form->As<RE::TESObjectARMO>())
				return armor->GetSlotMask().underlying();
			if (auto* addon = a_form->As<RE::TESObjectARMA>())
				return addon->GetSlotMask().underlying();
			return std::nullopt;
		}

		json DescribeHeadPart(RE::BGSHeadPart* a_part)
		{
			json       out = Identity(a_part);
			const auto type = static_cast<int>(a_part->type.get());
			out["headPartType"] = std::string(FormSearchLogic::HeadPartTypeName(type));
			json sexes = json::array();
			if (a_part->flags.any(RE::BGSHeadPart::Flag::kMale))
				sexes.push_back("male");
			if (a_part->flags.any(RE::BGSHeadPart::Flag::kFemale))
				sexes.push_back("female");
			out["sex"] = std::move(sexes);
			out["playable"] = a_part->flags.any(RE::BGSHeadPart::Flag::kPlayable);
			out["extraPart"] = a_part->flags.any(RE::BGSHeadPart::Flag::kIsExtraPart);
			out["model"] = ModelOf(*a_part);
			json extras = json::array();
			for (auto* extra : a_part->extraParts)
				if (extra)
					extras.push_back(Brief(extra));
			out["extraParts"] = std::move(extras);
			json races = json::array();
			if (a_part->validRaces)
				for (auto* race : a_part->validRaces->forms)
					if (race)
						races.push_back(Str(race->GetFormEditorID()).empty() ? Hex(race->GetFormID()) : Str(race->GetFormEditorID()));
			out["validRaces"] = std::move(races);
			return out;
		}

		json DescribeAddon(RE::TESObjectARMA* a_addon)
		{
			json out = Identity(a_addon);
			out["slotMask"] = Hex(a_addon->GetSlotMask().underlying());
			out["slots"] = FormSearchLogic::SlotsInMask(a_addon->GetSlotMask().underlying());
			out["maleModel"] = ModelOf(a_addon->bipedModels[RE::SEXES::kMale]);
			out["femaleModel"] = ModelOf(a_addon->bipedModels[RE::SEXES::kFemale]);
			return out;
		}

		json DescribeArmor(RE::TESObjectARMO* a_armor)
		{
			json out = Identity(a_armor);
			out["slotMask"] = Hex(a_armor->GetSlotMask().underlying());
			out["slots"] = FormSearchLogic::SlotsInMask(a_armor->GetSlotMask().underlying());
			json addons = json::array();
			for (auto* addon : a_armor->armorAddons)
				if (addon)
					addons.push_back(DescribeAddon(addon));
			out["addons"] = std::move(addons);
			return out;
		}

		json DescribeIdle(RE::TESIdleForm* a_idle, bool a_withAnimation)
		{
			json              out = Identity(a_idle);
			const std::string file = Str(a_idle->animFileName.c_str());
			out["animFile"] = file;
			out["animEvent"] = Str(a_idle->animEventName.c_str());
			out["loopMin"] = a_idle->data.loopMin;
			out["loopMax"] = a_idle->data.loopMax;
			out["replayDelay"] = a_idle->data.replayDelay;
			out["noAttacking"] = a_idle->data.flags.any(RE::IDLE_DATA::Flag::kNoAttacking);
			out["blocking"] = a_idle->data.flags.any(RE::IDLE_DATA::Flag::kBlocking);
			out["parent"] = Brief(a_idle->parentIdle);
			out["previous"] = Brief(a_idle->prevIdle);
			if (a_withAnimation && !file.empty())
				out["animation"] = AssetInfo::IdleAnimation(file, Str(a_idle->animEventName.c_str()));
			return out;
		}

		json Describe(RE::TESForm* a_form)
		{
			if (auto* idle = a_form->As<RE::TESIdleForm>())
				return DescribeIdle(idle, false);
			if (auto* part = a_form->As<RE::BGSHeadPart>())
				return DescribeHeadPart(part);
			if (auto* armor = a_form->As<RE::TESObjectARMO>())
				return DescribeArmor(armor);
			if (auto* addon = a_form->As<RE::TESObjectARMA>())
				return DescribeAddon(addon);
			return Identity(a_form);
		}

		bool Matches(RE::TESForm* a_form, const Query& a_query)
		{
			if (!a_query.plugin.empty() && !ContainsCI(PluginOf(a_form), a_query.plugin))
				return false;
			if (!a_query.name.empty() && !ContainsCI(Str(a_form->GetName()), a_query.name) && !ContainsCI(Str(a_form->GetFormEditorID()), a_query.name))
				return false;
			if (a_query.slotMask) {
				const auto mask = SlotMaskOf(a_form);
				if (!mask || (*mask & *a_query.slotMask) != *a_query.slotMask)
					return false;
			}
			if (a_query.headPartType) {
				auto* part = a_form->As<RE::BGSHeadPart>();
				if (!part || static_cast<int>(part->type.get()) != *a_query.headPartType)
					return false;
			}
			if (!a_query.model.empty()) {
				const auto paths = ModelPaths(a_form);
				if (!std::ranges::any_of(paths, [&](const std::string& p) { return ContainsCI(p, a_query.model); }))
					return false;
			}
			return true;
		}
	}

	const char* SupportedTypes()
	{
		return "Armor, ArmorAddon, HeadPart, Idle, NPC, Weapon, Misc";
	}

	json Find(const Query& a_query)
	{
		const auto* entry = std::ranges::find_if(kTypes, [&](const TypeEntry& e) { return FormSearchLogic::EqualsCI(e.name, a_query.formType); });
		if (entry == std::end(kTypes))
			throw ToolError(400, std::format("unsupported formType '{}' (use {})", a_query.formType, SupportedTypes()));
		const bool isArmorLike = entry->type == RE::FormType::Armor || entry->type == RE::FormType::Armature;
		if (a_query.slotMask && !isArmorLike)
			throw ToolError(400, "'slot' only applies to Armor and ArmorAddon");
		if (a_query.headPartType && entry->type != RE::FormType::HeadPart)
			throw ToolError(400, "'headPartType' only applies to HeadPart");
		if (!a_query.model.empty() && !isArmorLike && entry->type != RE::FormType::HeadPart)
			throw ToolError(400, "'model' only applies to Armor, ArmorAddon and HeadPart");

		std::vector<RE::TESForm*> candidates;
		{
			const auto& [map, lock] = RE::TESForm::GetAllForms();
			const RE::BSReadLockGuard guard{ lock };
			if (map)
				for (const auto& [id, form] : *map)
					if (form && form->Is(entry->type))
						candidates.push_back(form);
		}
		std::ranges::sort(candidates, {}, &RE::TESForm::formID);

		json        hits = json::array();
		std::size_t matched = 0;
		for (auto* form : candidates) {
			if (!Matches(form, a_query))
				continue;
			if (matched >= a_query.offset && hits.size() < a_query.limit)
				hits.push_back(Describe(form));
			++matched;
		}

		return json{
			{ "formType", entry->name },
			{ "total", matched },
			{ "offset", a_query.offset },
			{ "returned", hits.size() },
			{ "truncated", a_query.offset + hits.size() < matched },
			{ "forms", std::move(hits) },
		};
	}

	json DescribeForm(RE::TESForm* a_form)
	{
		json out = a_form->As<RE::TESIdleForm>() ? DescribeIdle(a_form->As<RE::TESIdleForm>(), true) : Describe(a_form);
		out["pluginInfo"] = AssetInfo::Plugin(PluginOf(a_form));
		return out;
	}

	json HeadPartsOf(RE::Actor* a_actor)
	{
		auto* base = a_actor ? a_actor->GetActorBase() : nullptr;
		if (!base)
			throw ToolError(404, "the reference has no actor base");
		json parts = json::array();
		for (int i = 0; i < base->numHeadParts; ++i)
			if (base->headParts && base->headParts[i])
				parts.push_back(DescribeHeadPart(base->headParts[i]));
		return json{
			{ "ref", Hex(a_actor->GetFormID()) },
			{ "base", Brief(base) },
			{ "sharedBase", !base->IsUnique() },
			{ "headParts", std::move(parts) },
		};
	}
}
