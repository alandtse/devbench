#include "test_framework.h"

#include "PrologueScan.h"

#include <array>
#include <cstdint>

using dvb::SafePrologueLength;

TEST_CASE("prologue scan covers whole stack-store instructions")
{
	// mov [rsp+8], rbx ; mov [rsp+10h], rsi ; push rdi
	const std::array<std::uint8_t, 11> code{ 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57 };
	CHECK(SafePrologueLength(code) == 5);
	CHECK(SafePrologueLength(code, 6) == 10);
}

TEST_CASE("prologue scan handles push, sub rsp, and mov rax,rsp forms")
{
	// push rbx ; sub rsp, 20h
	const std::array<std::uint8_t, 6> pushSub{ 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 };
	CHECK(SafePrologueLength(pushSub) == 6);
	// mov rax, rsp ; mov [rax+8], rbx
	const std::array<std::uint8_t, 7> movRax{ 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08 };
	CHECK(SafePrologueLength(movRax) == 7);
	// mov [rsp+18h], r8 ; sub rsp, 438h
	const std::array<std::uint8_t, 12> r8Big{ 0x4C, 0x89, 0x44, 0x24, 0x18, 0x48, 0x81, 0xEC, 0x38, 0x04, 0x00, 0x00 };
	CHECK(SafePrologueLength(r8Big) == 5);
	CHECK(SafePrologueLength(r8Big, 6) == 12);
}

TEST_CASE("prologue scan refuses jumps, RIP-relative operands, and truncated code")
{
	const std::array<std::uint8_t, 5> jmp{ 0xE9, 0x00, 0x10, 0x00, 0x00 };
	CHECK(SafePrologueLength(jmp) == 0);
	const std::array<std::uint8_t, 7> absJmp{ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0x00 };
	CHECK(SafePrologueLength(absJmp) == 0);
	// mov rax, [rip+disp32]
	const std::array<std::uint8_t, 7> ripRel{ 0x48, 0x8B, 0x05, 0x10, 0x20, 0x30, 0x40 };
	CHECK(SafePrologueLength(ripRel) == 0);
	// mov [rsp+8], rbx cut short
	const std::array<std::uint8_t, 4> cut{ 0x48, 0x89, 0x5C, 0x24 };
	CHECK(SafePrologueLength(cut) == 0);
	// call rel32
	const std::array<std::uint8_t, 5> call{ 0xE8, 0x00, 0x00, 0x00, 0x00 };
	CHECK(SafePrologueLength(call) == 0);
}
