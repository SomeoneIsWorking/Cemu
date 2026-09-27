#include "Cafe/HW/Espresso/GuestPatching.h"

#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/OS/RPL/rpl.h"

#include <cstring>
#include <vector>

namespace GuestPatching
{
	namespace
	{
		// What AllocateCode has handed out and nothing has been written to yet,
		// so a write into one of those blocks is recognised as the first write
		// to code no recompiled function covers. A block leaves this list the
		// moment it is written: the second write to a block the guest has run
		// from is not fresh, and must invalidate like any other.
		std::vector<std::pair<uint32_t, uint32_t>>& FreshBlocks()
		{
			static std::vector<std::pair<uint32_t, uint32_t>> blocks;
			return blocks;
		}

		// Every block AllocateCode has handed out, fresh or not. A block leaves the
		// fresh list the moment it is written, so this is the only list that still
		// knows a block exists once anything has been written to it -- and it is
		// the list that says whether an address is a range the recompiler was told
		// about, which is what a direct branch into it needs.
		std::vector<std::pair<uint32_t, uint32_t>>& RegisteredBlocks()
		{
			static std::vector<std::pair<uint32_t, uint32_t>> blocks;
			return blocks;
		}

		void ConsumeFresh(uint32_t guestAddress)
		{
			auto& blocks = FreshBlocks();
			for (auto block = blocks.begin(); block != blocks.end();) {
				if (guestAddress >= block->first && guestAddress < block->first + block->second) {
					block = blocks.erase(block);
				} else {
					++block;
				}
			}
		}

		uint8* MappedRange(uint32_t guestAddress, uint32_t sizeInBytes)
		{
			if (sizeInBytes == 0)
			{
				return nullptr;
			}
			if (!memory_isAddressRangeAccessible(guestAddress, sizeInBytes))
			{
				return nullptr;
			}
			return memory_getPointerFromVirtualOffset(guestAddress);
		}

		uint32_t SwapBytes(uint32_t value)
		{
			return ((value & 0x000000ffu) << 24) | ((value & 0x0000ff00u) << 8) |
			       ((value & 0x00ff0000u) >> 8) | ((value & 0xff000000u) >> 24);
		}

		// The bytes as they lie. Internal on purpose: a word crossing this
		// boundary goes through SwapBytes, so nothing can read one in the host's
		// order by reaching for the bytes instead.
		bool ReadBytes(uint32_t guestAddress, void* bytes, uint32_t sizeInBytes)
		{
			if (sizeInBytes == 0)
			{
				return true;
			}
			const uint8* source = MappedRange(guestAddress, sizeInBytes);
			if (source == nullptr)
			{
				return false;
			}
			memcpy(bytes, source, sizeInBytes);
			return true;
		}
	} // namespace

	uint32_t AllocateCode(uint32_t sizeInBytes)
	{
		if (sizeInBytes == 0)
		{
			return 0;
		}
		uint8* block = RPLLoader_AllocateTrampolineCodeSpace(static_cast<sint32>(sizeInBytes));
		if (block == nullptr)
		{
			return 0;
		}
		const uint32_t address = memory_getVirtualOffsetFromPointer(block);
		// Registered with the recompiler, which is what makes "may execute from"
		// true of this memory rather than true of the title's own code only. A
		// direct branch to an address finds its code by jumping there, but an
		// indirect one looks the target up in the recompiler's jump table, and a
		// block that was never registered is not in it -- so a stand-in reached
		// through a vtable would never run, however sound its instructions are.
		// The first allocation in a 4 MiB region pays for that region's table
		// and the rest of them are free.
		PPCRecompiler_allocateRange(address, sizeInBytes);
		FreshBlocks().emplace_back(address, sizeInBytes);
		RegisteredBlocks().emplace_back(address, sizeInBytes);
		return address;
	}

	uint32_t AllocateData(uint32_t sizeInBytes)
	{
		if (sizeInBytes == 0)
		{
			return 0;
		}
		// The emulator's system area: mapped before any title runs, writable, and
		// not part of the guest's own address space, so nothing the title does can
		// reach it. Alignment 4 is what a word of state needs.
		const uint32_t address = (uint32_t)coreinit_allocFromSysArea(sizeInBytes, 4);
		if (address == 0)
		{
			return 0;
		}
		// Zeroed, because the host reads these before the guest has written them
		// on some paths, and a count taken from whatever was there is not a count.
		for (uint32_t offset = 0; offset < sizeInBytes; offset += 4)
		{
			memory_writeU32(address + offset, 0);
		}
		return address;
	}

	bool IsRegisteredCode(uint32_t guestAddress)
	{
		for (const auto& block : RegisteredBlocks()) {
			if (guestAddress >= block.first && guestAddress < block.first + block.second) {
				return true;
			}
		}
		return false;
	}

	bool IsFreshCode(uint32_t guestAddress)
	{
		for (const auto& block : FreshBlocks()) {
			if (guestAddress >= block.first && guestAddress < block.first + block.second) {
				return true;
			}
		}
		return false;
	}

	uint32_t SetSwapInterval(uint32_t vblanksPerFlip)
	{
		// The shared area belongs to the graphics bring-up and does not exist yet
		// while that is still happening. An accessor that assumed it did turned a
		// control channel asking what the pacing is -- which any client may do at
		// any moment, including seconds before a title has a surface -- into a null
		// dereference inside the GPU state, with a stack trace that pointed at
		// graphics rather than at the question. So it refuses, and the refusal is
		// a value: the interval Latte is documented to start at.
		if (LatteGPUState.sharedArea == nullptr)
		{
			return kSwapIntervalUnknown;
		}
		// The same bound the export checks, so a caller cannot put Latte into a
		// state the title's own API would have refused.
		if (vblanksPerFlip >= 20)
		{
			return LatteGPUState.sharedArea->swapInterval;
		}
		LatteGPUState.sharedArea->swapInterval = vblanksPerFlip;
		return vblanksPerFlip;
	}

	uint32_t SwapInterval()
	{
		if (LatteGPUState.sharedArea == nullptr)
		{
			return kSwapIntervalUnknown;
		}
		return LatteGPUState.sharedArea->swapInterval;
	}

	bool ReadWord(uint32_t guestAddress, uint32_t& value)
	{
		uint32_t stored = 0;
		if (!ReadBytes(guestAddress, &stored, sizeof(stored)))
		{
			return false;
		}
		value = SwapBytes(stored);
		return true;
	}

	bool WriteWord(uint32_t guestAddress, uint32_t value)
	{
		const uint32_t stored = SwapBytes(value);
		return WriteBytes(guestAddress, &stored, sizeof(stored));
	}

	bool WriteBytes(uint32_t guestAddress, const void* bytes, uint32_t sizeInBytes)
	{
		if (sizeInBytes == 0)
		{
			return true;
		}
		uint8* target = MappedRange(guestAddress, sizeInBytes);
		if (target == nullptr)
		{
			return false;
		}
		memcpy(target, bytes, sizeInBytes);
		const bool fresh = IsFreshCode(guestAddress);
		ConsumeFresh(guestAddress);
		if (!fresh) {
			// Whatever the guest had compiled from these bytes is stale now.
			PPCRecompiler_invalidateRange(guestAddress, guestAddress + sizeInBytes);
		}
		if (IsRegisteredCode(guestAddress)) {
			// A range registered for *indirect* calls is not necessarily reachable
			// by a direct branch, and the difference is not a detail.
			//
			// Registering a range teaches the recompiler to look a target up in its
			// jump table when the guest branches to it indirectly -- through a
			// vtable, say -- and a stand-in reached that way runs. A *direct* branch
			// is a different mechanism: the recompiler emits a jump to the target's
			// host code, and if that address has never been visited there is no host
			// code to jump to. Nothing translates on the way, because the branch
			// carries no lookup, so the block is simply never entered.
			//
			// Measured, with a control that could not be argued with: a payload of
			// one word -- a branch straight back to the instruction after the branch
			// site, no state, nothing to keep right -- and the tick's call count went
			// from 180 in six seconds to 0, with the picture rate unchanged. The
			// branch executed no more than the code before it did; the tick simply
			// stopped running.
			//
			// So a write into a registered range has to leave the range translated,
			// and not only when something was stale: the *first* write is exactly the
			// one that leaves it untranslated. This only compiles an address that
			// has no block, so it costs nothing on a rewrite.
			PPCRecompiler_recompileIfUnvisited(guestAddress);
		}
		return true;
	}
} // namespace GuestPatching
