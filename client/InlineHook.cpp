#include "InlineHook.h"
#include "pch.h"

#include <cstring>
#include <windows.h>

namespace wo_native
{
	static const size_t kAbsoluteJumpSize = 14;

	static void WriteAbsoluteJump(uint8_t *at, void *destination)
	{
		at[0] = 0xFF;
		at[1] = 0x25;
		at[2] = 0x00;
		at[3] = 0x00;
		at[4] = 0x00;
		at[5] = 0x00;
		memcpy(at + 6, &destination, sizeof(destination));
	}

	bool InlineHook::Install(void *target, void *detour, const std::vector<uint8_t> &expectedPrologue)
	{
		if (installed_)
			return true;

		if (!target || !detour || expectedPrologue.size() < kAbsoluteJumpSize)
		{
			error_ = "invalid arguments";
			return false;
		}

		uint8_t *code = static_cast<uint8_t *>(target);

		if (memcmp(code, expectedPrologue.data(), expectedPrologue.size()) != 0)
		{
			error_ = "prologue mismatch";
			return false;
		}

		const size_t stolen = expectedPrologue.size();

		trampoline_ = VirtualAlloc(nullptr, stolen + kAbsoluteJumpSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
		if (!trampoline_)
		{
			error_ = "trampoline allocation failed";
			return false;
		}

		uint8_t *tramp = static_cast<uint8_t *>(trampoline_);
		memcpy(tramp, code, stolen);
		WriteAbsoluteJump(tramp + stolen, code + stolen);

		DWORD previous = 0;
		if (!VirtualProtect(code, stolen, PAGE_EXECUTE_READWRITE, &previous))
		{
			VirtualFree(trampoline_, 0, MEM_RELEASE);
			trampoline_ = nullptr;
			error_ = "VirtualProtect failed";
			return false;
		}

		original_.assign(code, code + stolen);

		WriteAbsoluteJump(code, detour);
		memset(code + kAbsoluteJumpSize, 0x90, stolen - kAbsoluteJumpSize);

		VirtualProtect(code, stolen, previous, &previous);
		FlushInstructionCache(GetCurrentProcess(), code, stolen);

		target_ = target;
		installed_ = true;
		return true;
	}

	void InlineHook::Remove()
	{
		if (!installed_)
			return;

		uint8_t *code = static_cast<uint8_t *>(target_);
		DWORD previous = 0;

		if (VirtualProtect(code, original_.size(), PAGE_EXECUTE_READWRITE, &previous))
		{
			memcpy(code, original_.data(), original_.size());
			VirtualProtect(code, original_.size(), previous, &previous);
			FlushInstructionCache(GetCurrentProcess(), code, original_.size());
		}

		if (trampoline_)
		{
			VirtualFree(trampoline_, 0, MEM_RELEASE);
			trampoline_ = nullptr;
		}

		installed_ = false;
	}
} // namespace wo_native
