#include "FormLookup.h"

namespace dvb::FormLookup
{
	RE::TESForm* LookupFormArg(const std::string& a_arg)
	{
		auto byHex = [](const std::string& s) -> RE::TESForm* {
			std::size_t        consumed = 0;
			unsigned long long id = 0;
			try {
				id = std::stoull(s, &consumed, 16);
			} catch (...) {
				return nullptr;
			}
			if (consumed != s.size() || id > 0xFFFFFFFFull)
				return nullptr;
			return RE::TESForm::LookupByID(static_cast<RE::FormID>(id));
		};
		if (a_arg.size() > 2 && a_arg[0] == '0' && (a_arg[1] == 'x' || a_arg[1] == 'X'))
			return byHex(a_arg.substr(2));
		if (auto* f = RE::TESForm::LookupByEditorID(a_arg))
			return f;
		return byHex(a_arg);
	}
}
