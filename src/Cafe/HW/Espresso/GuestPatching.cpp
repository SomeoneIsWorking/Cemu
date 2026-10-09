#include "Cafe/HW/Espresso/GuestPatching.h"

#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cafe/HW/MMU/MMU.h"
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
			for (auto block = blocks.begin(); block != blocks.end();)
			{
				if (guestAddress >= block->first && guestAddress < block->first + block->second)
				{
					block = blocks.erase(block);
				}
				else
				{
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
		// Guest memory, from the loader's trampoline area, because a counter the
		// guest's own instructions read and write has to be somewhere the guest
		// can read and write. This used to hand out the emulator's system area
		// instead, which is host bookkeeping the guest cannot reach at all: a
		// gate that had been unreachable reported no calls and no fault, and the
		// first run that actually reached its own stores died on the first one.
		// A counter that never moved and a counter that faults look the same from
		// outside, which is why the block's address space is stated rather than
		// left to be discovered.
		//
		// Not registered with the recompiler, unlike AllocateCode: this is data,
		// and nothing should branch to it.
		uint8* block = RPLLoader_AllocateTrampolineCodeSpace(static_cast<sint32>(sizeInBytes));
		if (block == nullptr)
		{
			return 0;
		}
		const uint32_t address = memory_getVirtualOffsetFromPointer(block);
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
		for (const auto& block : RegisteredBlocks())
		{
			if (guestAddress >= block.first && guestAddress < block.first + block.second)
			{
				return true;
			}
		}
		return false;
	}

	bool IsFreshCode(uint32_t guestAddress)
	{
		for (const auto& block : FreshBlocks())
		{
			if (guestAddress >= block.first && guestAddress < block.first + block.second)
			{
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

	bool WriteDataWords(uint32_t guestAddress, const uint32_t* values, uint32_t count)
	{
		if (values == nullptr || count == 0)
		{
			return false;
		}
		uint8* target = MappedRange(guestAddress, count * sizeof(uint32_t));
		if (target == nullptr)
		{
			return false;
		}
		for (uint32_t i = 0; i < count; i++)
		{
			const uint32_t stored = SwapBytes(values[i]);
			memcpy(target + i * sizeof(uint32_t), &stored, sizeof(stored));
		}
		return true;
	}

	bool ReadWords(uint32_t guestAddress, uint32_t* values, uint32_t count)
	{
		if (values == nullptr || count == 0)
		{
			return false;
		}
		// A fixed staging buffer rather than a sized one on the stack: this is called
		// from probes on the display thread, where a length a caller chose decides how
		// much stack the frame takes, and a caller that asks for a hundred thousand
		// words should not be able to move the display thread's stack. A pose is
		// twelve words, so this is a hundred and fifty times the size asked for; a
		// longer read is done in as many passes as it takes.
		constexpr uint32_t kStaged = 32;
		uint8 staged[kStaged * sizeof(uint32_t)];
		uint32 done = 0;
		while (done < count)
		{
			const uint32_t batch = (count - done < kStaged) ? (count - done) : kStaged;
			if (!ReadBytes(guestAddress + sizeof(uint32_t) * done, staged, batch * sizeof(uint32_t)))
			{
				return false;
			}
			// Guest order, one word at a time, through the same swap ReadWord uses: a
			// block of words whose halves were exchanged is a structure that reads
			// perfectly well and is the wrong way round.
			for (uint32_t index = 0; index < batch; index++)
			{
				uint32_t stored = 0;
				std::memcpy(&stored, staged + sizeof(uint32_t) * index, sizeof(stored));
				values[done + index] = SwapBytes(stored);
			}
			done += batch;
		}
		return true;
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
		if (!fresh)
		{
			// Whatever the guest had compiled from these bytes is stale now.
			PPCRecompiler_invalidateRange(guestAddress, guestAddress + sizeInBytes);
		}
		if (IsRegisteredCode(guestAddress))
		{
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
