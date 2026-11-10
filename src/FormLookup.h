#pragma once

#include <string>

namespace RE
{
	class TESForm;
}

namespace dvb::FormLookup
{
	/// Resolves a form argument: an explicit 0x.. FormID, else an EditorID (tried first so an all-hex
	/// EditorID isn't misread as a FormID), else a bare hex FormID. The hex must be the whole string
	/// and fit 32 bits, so "14G" or an overflow can't truncate to a valid FormID and resolve the
	/// wrong form. Null when nothing matches.
	RE::TESForm* LookupFormArg(const std::string& a_arg);
}
