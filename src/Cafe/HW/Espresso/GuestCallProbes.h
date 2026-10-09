#pragma once

#include <cstdint>
#include <optional>
#include <span>

// Observes calls into one of the title's functions without changing what
// they do.
//
// A function registered here has its first instruction replaced, once the
// title is linked and before any of its code runs, by a branch to a stub:
// an HLE call that tells the probe the caller's registers, the instruction
// it displaced, and a branch back to the instruction after it. The branch
// back takes the count register and one volatile register the displaced
// instruction does not name, so any first instruction but a relative branch
// runs from the stub as it did in place.
// Nothing else is patched, so a registration whose entry does not hold the
// instruction it names, or holds one that cannot run elsewhere, is refused
// and left alone. With nothing registered this is a no-op and the build
// behaves as upstream.
namespace GuestCallProbes
{
	// How a registration ended when the title was linked.
	enum class Installation
	{
		// Calls into the function now pass through the probe.
		Installed,
		// The entry held another instruction: another executable, or another
		// revision of it. Left as it was.
		EntryHeldOther,
		// The entry's instruction branches relative to where it stands, or names
		// every register the stub could branch back through, so it cannot run
		// from the stub. Left as it was.
		EntryNotRelocatable,
		// No code space for the stub within a branch's reach of the function.
		NoCodeSpace,
	};

	class Probe
	{
	  public:
		virtual ~Probe() = default;
		// Once, on the thread that links the title, before its code runs.
		virtual void OnInstall(Installation installation) = 0;
		// Each call into the function, on the calling guest thread, before
		// its first instruction runs; `gpr` are the caller's integer
		// registers and `returnAddress` the link register -- the instruction
		// after the call that entered it, naming the call site when it was
		// entered by one. May write guest data (GuestPatching::WriteDataWords),
		// never code or registers.
		virtual void OnCall(std::span<const uint32_t, 32> gpr, uint32_t returnAddress) = 0;
	};

	// Registers `probe` for the function at guest address `entry`, whose
	// first instruction is expected to be `firstInstruction`. Before the
	// title is linked; the probe must outlive the process's guest execution.
	//
	// `holdsEntry` says whether the probe wants to keep seeing calls. A probe
	// that only wants the *moment* the title was linked -- to take memory from
	// the loader's arena, which does not exist before then -- passes false, and
	// its instruction is put straight back once the probe has been told.
	//
	// That is not a small distinction. A probe that holds an entry takes it:
	// any other registration for the same address is refused for the rest of
	// the run with EntryHeldOther, and there is no way to take it back. So a
	// momentary observer on a hot function silently disables every standing
	// probe on that function, and the only symptom is a count of zero, which
	// reads as a call that never happens.
	//
	// `resume` is where execution continues once the probe has been told: 0, the
	// default, is the instruction after the entry, which is what an observer wants.
	// Anything else sends the call somewhere else first, and the probe's own stub
	// branch is the only way into the emulator's trampoline area that is known to
	// arrive -- a branch written by a host into a guest function's interior does
	// not, whatever kind of branch it is, because the recompiler has to turn it
	// into a jump to a host address it never translated.
	//
	// That is what makes a gate reachable at all. A gate is a block in the
	// trampoline area that the guest is sent to, decides whether the call
	// proceeds, and either continues at `resume` or returns; with the resume
	// fixed at entry + 4 there is nowhere to send it.
	void Register(uint32_t entry, uint32_t firstInstruction, Probe& probe, bool holdsEntry = true,
	              uint32_t resume = 0);

	// The volatile register the stub builds its branch back in for a displaced
	// instruction: one the instruction names in none of its register fields,
	// or none. The displaced word runs first and may set one (the g3d block
	// commit opens with `or r12,r3,r3`).
	constexpr std::optional<uint32_t> ScratchRegister(uint32_t displaced)
	{
		constexpr uint32_t candidates[] = {12, 11, 0};
		constexpr uint32_t fieldShifts[] = {21, 16, 11};
		for (uint32_t candidate : candidates)
		{
			bool named = false;
			for (uint32_t shift : fieldShifts)
			{
				named = named || ((displaced >> shift) & 0x1f) == candidate;
			}
			if (!named)
			{
				return candidate;
			}
		}
		return std::nullopt;
	}

	// The host bytes behind `size` bytes of guest memory at `address`, or
	// null unless all of them are mapped: a probe reads the title's objects
	// through this, and names guest memory as the renderer's draws do.
	const void* GuestBytes(uint32_t address, uint32_t size);

	// Installs every registration. Called by the system once the title's
	// modules are linked, before control passes to it.
	void InstallRegistered();
} // namespace GuestCallProbes
