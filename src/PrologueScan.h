#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace dvb
{
	/// The length of the shortest run of whole instructions at the start of a_code that covers at
	/// least a_minBytes, when every instruction in it is a position-independent prologue form
	/// (push, register/stack mov/lea, sub/add rsp, xor reg,reg, mov reg,imm) and so can be copied
	/// elsewhere and still run. 0 for anything else: RIP-relative operands, jumps or calls (which
	/// is also what another plugin's hook looks like), or a form it does not recognise.
	std::size_t SafePrologueLength(std::span<const std::uint8_t> a_code, std::size_t a_minBytes = 5);
}
