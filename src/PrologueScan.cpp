#include "PrologueScan.h"

namespace dvb
{
	namespace
	{
		// Bytes taken by a ModRM memory/register operand (ModRM + SIB + displacement), or 0 when it
		// is RIP-relative or an absolute disp32, which would point elsewhere once copied.
		std::size_t OperandLength(std::span<const std::uint8_t> a_code, std::size_t a_at)
		{
			if (a_at >= a_code.size())
				return 0;
			const std::uint8_t modrm = a_code[a_at];
			const std::uint8_t mod = modrm >> 6;
			const std::uint8_t rm = modrm & 7;
			if (mod == 3)
				return 1;
			std::size_t len = 1;
			if (rm == 4) {
				if (a_at + 1 >= a_code.size())
					return 0;
				if (mod == 0 && (a_code[a_at + 1] & 7) == 5)
					return 0;
				++len;
			} else if (mod == 0 && rm == 5) {
				return 0;
			}
			if (mod == 1)
				len += 1;
			else if (mod == 2)
				len += 4;
			return len;
		}

		std::size_t InstructionLength(std::span<const std::uint8_t> a_code, std::size_t a_at)
		{
			std::size_t i = a_at;
			bool        rexW = false;
			if (i < a_code.size() && (a_code[i] & 0xF0) == 0x40) {
				rexW = (a_code[i] & 0x08) != 0;
				++i;
			}
			if (i >= a_code.size())
				return 0;
			const std::uint8_t op = a_code[i++];
			std::size_t        len = 0;
			if (op >= 0x50 && op <= 0x57) {
				len = 0;
			} else if (op == 0x89 || op == 0x8B || op == 0x8D) {
				const std::size_t operand = OperandLength(a_code, i);
				if (operand == 0 || (op == 0x8D && (a_code[i] >> 6) == 3))
					return 0;
				len = operand;
			} else if (op == 0x31 || op == 0x33) {
				if (i >= a_code.size() || (a_code[i] >> 6) != 3)
					return 0;
				len = 1;
			} else if (op == 0x83 || op == 0x81) {
				if (i >= a_code.size())
					return 0;
				const std::uint8_t modrm = a_code[i];
				const std::uint8_t ext = (modrm >> 3) & 7;
				if ((modrm >> 6) != 3 || (ext != 0 && ext != 5))
					return 0;
				len = 1 + (op == 0x83 ? 1 : 4);
			} else if (op >= 0xB8 && op <= 0xBF) {
				len = rexW ? 8 : 4;
			} else {
				return 0;
			}
			const std::size_t total = (i - a_at) + len;
			return a_at + total <= a_code.size() ? total : 0;
		}
	}

	std::size_t SafePrologueLength(std::span<const std::uint8_t> a_code, std::size_t a_minBytes)
	{
		std::size_t at = 0;
		while (at < a_minBytes) {
			const std::size_t len = InstructionLength(a_code, at);
			if (len == 0)
				return 0;
			at += len;
		}
		return at;
	}
}
