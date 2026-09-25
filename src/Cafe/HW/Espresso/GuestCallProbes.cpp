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
			uint32_t slotAddress;
			uint32_t expectedTarget;
			Probe* probe;
			// The stub's first instruction, the HLE call; 0 until installed.
			uint32_t stubAddress;
		};

		// Written before the title's code runs and only read after, so the
		// guest threads that dispatch read it without a lock.
		std::vector<Registration> s_registrations;

		constexpr uint32_t kStubInstructions = 5;
		constexpr uint32_t kGpr12 = 12;

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

		// The HLE call, then `lis r12, hi; ori r12, r12, lo; mtctr r12; bctr`:
		// r12 and ctr are volatile across a call, so the target sees the
		// caller's every argument.
		void WriteStub(uint32_t stubAddress, HLEIDX hleIndex, uint32_t target)
		{
			const uint32_t instructions[kStubInstructions] = {
				(1u << 26) | static_cast<uint32_t>(hleIndex),
				(15u << 26) | (kGpr12 << 21) | (target >> 16),
				(24u << 26) | (kGpr12 << 21) | (kGpr12 << 16) | (target & 0xFFFFu),
				0x7D8903A6u,
				0x4E800420u,
			};
			for (uint32_t i = 0; i < kStubInstructions; i++)
			{
				memory_writeU32(stubAddress + i * 4, instructions[i]);
			}
		}
	}

	void Register(uint32_t slotAddress, uint32_t expectedTarget, Probe& probe)
	{
		s_registrations.push_back({slotAddress, expectedTarget, &probe, 0});
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
			if (memory_readU32(registration.slotAddress) != registration.expectedTarget)
			{
				registration.probe->OnInstall(Installation::SlotHeldOther);
				continue;
			}
			uint8* stub = RPLLoader_AllocateTrampolineCodeSpace(kStubInstructions * 4);
			if (stub == nullptr)
			{
				registration.probe->OnInstall(Installation::NoCodeSpace);
				continue;
			}
			registration.stubAddress = memory_getVirtualOffsetFromPointer(stub);
			WriteStub(registration.stubAddress, hleIndex, registration.expectedTarget);
			memory_writeU32(registration.slotAddress, registration.stubAddress);
			registration.probe->OnInstall(Installation::Installed);
		}
	}
}
