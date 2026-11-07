#include "SaveDelete.h"

namespace dvb::SaveDelete
{
	namespace
	{
		RE::BGSSaveLoadFileEntry* FindEntry(RE::BGSSaveLoadManager& a_mgr, const std::string& a_stem)
		{
			for (auto* entry : a_mgr.saveGameList) {
				if (entry && _stricmp(Stem(entry->fileName.c_str()).c_str(), a_stem.c_str()) == 0)
					return entry;
			}
			return nullptr;
		}
	}

	std::string Stem(std::string a_name)
	{
		constexpr std::string_view ext = ".ess";
		if (a_name.size() > ext.size() && _stricmp(a_name.c_str() + a_name.size() - ext.size(), ext.data()) == 0)
			a_name.resize(a_name.size() - ext.size());
		return a_name;
	}

	Result Delete(const std::string& a_name)
	{
		if (REL::Module::IsVR())
			return Result::kUnsupported;

		auto* mgr = RE::BGSSaveLoadManager::GetSingleton();
		if (!mgr)
			return Result::kNotInList;

		// The list only exists once the load menu (or this call) has built it.
		if (mgr->saveGameList.empty())
			mgr->PopulateSaveList();

		auto* entry = FindEntry(*mgr, Stem(a_name));
		if (!entry)
			return Result::kNotInList;

		using Fn = void(RE::BGSSaveLoadFileEntry*);
		static REL::Relocation<Fn> engineDelete{ RELOCATION_ID(34615, 35535) };
		engineDelete(entry);
		return Result::kDeleted;
	}
}
