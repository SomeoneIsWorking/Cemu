#include "Cafe/HW/Espresso/GuestCallProbes.h"

#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cemu/Logging/CemuLogging.h"

#include <deque>
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
			// False for a probe that only wants the link-time moment, which
			// gives the entry straight back once it has had it.
			bool holdsEntry;
			// Where the call continues once the probe has been told; 0 means
			// the instruction after the entry, which is what an observer wants.
			// Anything else sends the call somewhere first, and the stub's own
			// branch is the only way into the trampoline area that arrives.
			uint32_t resume;
		};

		// Written before the title's code runs and only read after, so the
		// guest threads that dispatch read it without a lock.
		//
		// A deque, and indexed rather than ranged, because a probe legitimately
		// learns what it needs to register in OnInstall -- the loader's arena does
		// not exist before the title is linked, so a mod that needs memory from it
		// can only ask once it has been told -- and a vector that reallocates
		// under an append invalidates the loop installing the others. What that
		// cost: one registration added during installation was silently dropped,
		// and a probe that had reported `installed` counted nothing at all, which
		// reads as a title that never calls the function rather than as an
		// instrument that was never wired.
		std::deque<Registration> s_registrations;

		// The HLE call, the displaced instruction, then the count register and an
		// indirect branch to the resume. See WriteStub for why the branch is
		// indirect: a direct one keeps jumping to the host code the target had
		// when the branch was translated.
		constexpr uint32_t kStubInstructions = 6;
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
		//
		// `resume` is reached through the count register rather than by a direct
		// branch, and that is not a style choice. A direct branch is compiled to a
		// jump to the target's *host* code, resolved once when the branch itself
		// was translated; rewriting the target afterwards produces new host code
		// at a new address and leaves the branch jumping to the old one. Nothing
		// invalidates the branch, because nothing was written where the branch
		// is, so the guest runs the payload that was there when the branch was
		// first translated, forever, and reports the count of a block that no
		// longer exists.
		//
		// An indirect branch looks the target up in the recompiler's jump table
		// on every execution, so a rewritten target is reached. This is the same
		// mechanism a stand-in reached through a vtable relies on, which is why
		// that one works and this one did not.
		//
		// `mtctr` does not touch the link register, so a function entered through
		// this stub still returns to its own caller.
		// Encodings lifted from a PowerPC image rather than assembled here, and
		// each checked against a second instruction in the same image:
		//
		//   lis  r12,0x00e0   3d8000e0     ori r12,r12,0x5890   618c5890
		//   mtctr r12         7d8903a6     bctr                4e800420
		//
		// `mtctr` carries its register in bits 21-25, so the form for r12 is
		// 0x7c0903a6 with the register added in; hand-assembling the extended
		// opcode from the ISA manual instead produces 0x7d8003a6, which is a
		// different instruction, and the difference is a register clobbered or
		// not.
		constexpr uint32_t kLoadUpperImmediate = 0x3c000000; // lis rD, uimm
		constexpr uint32_t kOrImmediate = 0x60000000;        // ori rA, rS, uimm
		constexpr uint32_t kMoveToCounter = 0x7c0903a6;      // mtctr rS
		constexpr uint32_t kBranchCount = 0x4e800420;        // bctr
		// The register the address is built in. Volatile under the EABI, so
		// clobbering it is allowed between a call and its return -- but the
		// displaced instruction runs while it holds the address, so a probe whose
		// displaced instruction names this register would find it changed. Every
		// probe in this product displaces a link-register save, which does not.
		constexpr uint32_t kAddressRegister = 12;

		void WriteStub(uint32_t stubAddress, HLEIDX hleIndex, uint32_t displaced, uint32_t resume)
		{
			const uint32_t registerShift = kAddressRegister << 21;
			const uint32_t instructions[kStubInstructions] = {
				(1u << 26) | static_cast<uint32_t>(hleIndex),
				displaced,
				// lis r12, resume
				kLoadUpperImmediate | registerShift | ((resume >> 16) & 0xffff),
				// ori r12, r12, resume
				kOrImmediate | registerShift | (kAddressRegister << 16) | (resume & 0xffff),
				kMoveToCounter | registerShift,
				kBranchCount,
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
			const uint32_t resume =
				registration.resume != 0 ? registration.resume : registration.entry + 4;
			// Only the branch into the stub has to be in reach. The branch out of
			// it is indirect and its target is an address built at run time, so it
			// is not subject to a branch's displacement at all -- which is what lets
			// a probe's resume be anywhere in the guest, including the loader's
			// arena, where a relative branch from the title's own code would not
			// reach.
			if (!WithinRelativeBranch(registration.entry, stubAddress))
			{
				return Installation::NoCodeSpace;
			}
			registration.stubAddress = stubAddress;
			WriteStub(stubAddress, hleIndex, registration.firstInstruction, resume);
			memory_writeU32(registration.entry, RelativeBranch(registration.entry, stubAddress));
			return Installation::Installed;
		}
	} // namespace

	void Register(uint32_t entry, uint32_t firstInstruction, Probe& probe, bool holdsEntry,
	              uint32_t resume)
	{
		Registration registration = {};
		registration.entry = entry;
		registration.firstInstruction = firstInstruction;
		registration.probe = &probe;
		registration.holdsEntry = holdsEntry;
		registration.resume = resume;
		s_registrations.push_back(registration);
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
		// Indexed, re-reading the size each turn, and never holding a reference
		// across a callback: a probe legitimately registers more from inside
		// OnInstall -- the loader's arena does not exist before the title is
		// linked, so a mod that needs memory from it can only ask once it has
		// been told -- and everything this loop touches is fetched again by index
		// afterwards. A vector reallocating under that append is what used to
		// drop the registration silently; a deque was not enough on its own,
		// because what the callback changes is not only the container.
		//
		// The index only moves on past an element that stays, so a refused one
		// leaves no hole to step into.
		for (size_t index = 0; index < s_registrations.size();)
		{
			// A registration with no probe is refused here rather than dispatched
			// later. The table is filled through a function-pointer seam, so a
			// caller that hands over a null probe gets past every check the
			// language can make and arrives as a reference whose address is zero;
			// nothing here can see that as anything but a good reference. Left in
			// the table it is a null vtable read inside a guest thread seconds
			// after the title started, with the wiring mistake nowhere in the
			// evidence -- measured, and it took a debugger to name -- so it is
			// turned away at the door, the entry is left alone, and the address
			// is named.
			if (s_registrations[index].probe == nullptr)
			{
				cemuLog_log(LogType::Force,
				            "[probes] refused a registration on {:#010x}: no probe to call",
				            s_registrations[index].entry);
				s_registrations.erase(s_registrations.begin() + index);
				continue;
			}
			Probe* probe = s_registrations[index].probe;
			const Installation installation = Install(s_registrations[index], hleIndex);
			// The entry is given back *before* the probe is told, not after. A
			// probe that only wanted the moment exists so that a registration made
			// from inside its callback -- the standing probe on the same function,
			// which has to hold the entry to redirect the call -- can take it in
			// this same pass. Releasing afterwards would undo that probe's branch
			// and leave a gate wired, reported as installed, and never entered.
			if (!s_registrations[index].holdsEntry)
			{
				// A probe that only wanted the moment gives the entry straight
				// back, so a registration later in this same loop -- a gate
				// holding the same address, say -- can still take it. Its stub is
				// never entered again and costs nothing.
				if (s_registrations[index].stubAddress != 0)
				{
					memory_writeU32(s_registrations[index].entry,
					                 s_registrations[index].firstInstruction);
					s_registrations[index].stubAddress = 0;
				}
			}
			probe->OnInstall(installation);
			index++;
		}
	}
} // namespace GuestCallProbes
