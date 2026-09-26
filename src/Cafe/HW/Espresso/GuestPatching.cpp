#include "Cafe/HW/Espresso/GuestPatching.h"

#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/OS/RPL/rpl.h"

#include <cstring>

namespace GuestPatching
{
	namespace
	{
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
		return memory_getVirtualOffsetFromPointer(block);
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
		// Whatever the guest had compiled from these bytes is stale now.
		PPCRecompiler_invalidateRange(guestAddress, guestAddress + sizeInBytes);
		return true;
	}
} // namespace GuestPatching
