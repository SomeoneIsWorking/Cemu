#include "Cafe/HW/Espresso/GuestCallProbes.h"

#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/OS/RPL/rpl.h"

#include <vector>

namespace GuestCallProbes
{
	namespace
	{
		struct Registration
		{
			uint32_t entry;
			uint32_t firstInstruction;
			Probe* probe;
			// The stub's first instruction, the HLE call; 0 until installed.
			uint32_t stubAddress;
		};

		// Written before the title's code runs and only read after, so the
		// guest threads that dispatch read it without a lock.
		std::vector<Registration> s_registrations;

		constexpr uint32_t kStubInstructions = 3;
		// A relative branch reaches 32 MiB either side of where it stands.
		constexpr int64_t kRelativeBranchReach = 0x02000000;
		constexpr uint32_t kPrimaryBranchConditional = 16;
		constexpr uint32_t kPrimaryBranch = 18;

		void Dispatch(PPCInterpreter_t* cpu)
		{
			for (const Registration& registration : s_registrations)
			{
				if (registration.stubAddress == cpu->instructionPointer)
				{
					registration.probe->OnCall(std::span<const uint32_t, 32>(cpu->gpr, 32), cpu->spr.LR);
					break;
				}
			}
			PPCInterpreter_nextInstruction(cpu);
		}

		bool WithinRelativeBranch(uint32_t from, uint32_t to)
		{
			int64_t displacement = static_cast<int64_t>(to) - static_cast<int64_t>(from);
			return displacement >= -kRelativeBranchReach && displacement < kRelativeBranchReach;
		}

		// `b to`, standing at `from`.
		uint32_t RelativeBranch(uint32_t from, uint32_t to)
		{
			return (kPrimaryBranch << 26) | ((to - from) & 0x03FFFFFCu);
		}

		// The HLE call, the displaced instruction, then a branch back into the
		// function: no register is touched, so the function runs as if called
		// directly.
		void WriteStub(uint32_t stubAddress, HLEIDX hleIndex, uint32_t displaced, uint32_t resume)
		{
			uint32_t back = stubAddress + (kStubInstructions - 1) * 4;
			const uint32_t instructions[kStubInstructions] = {
				(1u << 26) | static_cast<uint32_t>(hleIndex),
				displaced,
				RelativeBranch(back, resume),
			};
			for (uint32_t i = 0; i < kStubInstructions; i++)
			{
				memory_writeU32(stubAddress + i * 4, instructions[i]);
			}
		}

		Installation Install(Registration& registration, HLEIDX hleIndex)
		{
			if (memory_readU32(registration.entry) != registration.firstInstruction)
			{
				return Installation::EntryHeldOther;
			}
			uint32_t primary = registration.firstInstruction >> 26;
			if (primary == kPrimaryBranch || primary == kPrimaryBranchConditional)
			{
				return Installation::EntryNotRelocatable;
			}
			uint8* stub = RPLLoader_AllocateTrampolineCodeSpace(kStubInstructions * 4);
			if (stub == nullptr)
			{
				return Installation::NoCodeSpace;
			}
			uint32_t stubAddress = memory_getVirtualOffsetFromPointer(stub);
			uint32_t resume = registration.entry + 4;
			if (!WithinRelativeBranch(registration.entry, stubAddress) ||
				!WithinRelativeBranch(stubAddress + (kStubInstructions - 1) * 4, resume))
			{
				return Installation::NoCodeSpace;
			}
			registration.stubAddress = stubAddress;
			WriteStub(stubAddress, hleIndex, registration.firstInstruction, resume);
			memory_writeU32(registration.entry, RelativeBranch(registration.entry, stubAddress));
			return Installation::Installed;
		}
	} // namespace

	void Register(uint32_t entry, uint32_t firstInstruction, Probe& probe)
	{
		s_registrations.push_back({entry, firstInstruction, &probe, 0});
	}

	const void* GuestBytes(uint32_t address, uint32_t size)
	{
		if (!memory_isAddressRangeAccessible(address, size))
		{
			return nullptr;
		}
		return memory_getPointerFromVirtualOffset(address);
	}

	void InstallRegistered()
	{
		if (s_registrations.empty())
		{
			return;
		}
		HLEIDX hleIndex = PPCInterpreter_registerHLECall(Dispatch, "GuestCallProbes::Dispatch");
		for (Registration& registration : s_registrations)
		{
			registration.probe->OnInstall(Install(registration, hleIndex));
		}
	}
} // namespace GuestCallProbes
