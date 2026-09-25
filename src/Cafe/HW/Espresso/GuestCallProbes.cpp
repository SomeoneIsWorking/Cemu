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

		constexpr uint32_t kStubInstructions = 6;
		constexpr uint32_t kGpr12 = 12;
		// An absolute branch reaches the first 32 MiB of the address space.
		constexpr uint32_t kAbsoluteBranchReach = 0x02000000;
		constexpr uint32_t kPrimaryBranchConditional = 16;
		constexpr uint32_t kPrimaryBranch = 18;

		void Dispatch(PPCInterpreter_t* cpu)
		{
			for (const Registration& registration : s_registrations)
			{
				if (registration.stubAddress == cpu->instructionPointer)
				{
					registration.probe->OnCall(std::span<const uint32_t, 32>(cpu->gpr, 32));
					break;
				}
			}
			PPCInterpreter_nextInstruction(cpu);
		}

		// The HLE call, the displaced instruction, then
		// `lis r12, hi; ori r12, r12, lo; mtctr r12; bctr` back into the
		// function: r12 and ctr are volatile across a call, so a function
		// entered by one reads neither before writing it.
		void WriteStub(uint32_t stubAddress, HLEIDX hleIndex, uint32_t displaced, uint32_t resume)
		{
			const uint32_t instructions[kStubInstructions] = {
				(1u << 26) | static_cast<uint32_t>(hleIndex),
				displaced,
				(15u << 26) | (kGpr12 << 21) | (resume >> 16),
				(24u << 26) | (kGpr12 << 21) | (kGpr12 << 16) | (resume & 0xFFFFu),
				0x7D8903A6u,
				0x4E800420u,
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
			if (stubAddress + kStubInstructions * 4 > kAbsoluteBranchReach)
			{
				return Installation::NoCodeSpace;
			}
			registration.stubAddress = stubAddress;
			WriteStub(stubAddress, hleIndex, registration.firstInstruction, registration.entry + 4);
			// `ba stub`
			memory_writeU32(registration.entry, (kPrimaryBranch << 26) | stubAddress | 2u);
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
