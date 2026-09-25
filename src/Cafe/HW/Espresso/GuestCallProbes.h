#pragma once

#include <cstdint>
#include <span>

// Observes calls into one of the title's functions without changing what
// they do.
//
// A function registered here has its first instruction replaced, once the
// title is linked and before any of its code runs, by a branch to a stub:
// an HLE call that tells the probe the caller's registers, the instruction
// it displaced, and a branch back to the instruction after it. The stub
// touches no register, so any first instruction but a relative branch runs
// from it as it did in place.
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
		// The entry's instruction branches relative to where it stands, so it
		// cannot run from the stub. Left as it was.
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
		// entered by one. Must not change guest state.
		virtual void OnCall(std::span<const uint32_t, 32> gpr, uint32_t returnAddress) = 0;
	};

	// Registers `probe` for the function at guest address `entry`, whose
	// first instruction is expected to be `firstInstruction`. Before the
	// title is linked; the probe must outlive the process's guest execution.
	void Register(uint32_t entry, uint32_t firstInstruction, Probe& probe);

	// The host bytes behind `size` bytes of guest memory at `address`, or
	// null unless all of them are mapped: a probe reads the title's objects
	// through this, and names guest memory as the renderer's draws do.
	const void* GuestBytes(uint32_t address, uint32_t size);

	// Installs every registration. Called by the system once the title's
	// modules are linked, before control passes to it.
	void InstallRegistered();
} // namespace GuestCallProbes
