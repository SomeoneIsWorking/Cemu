#pragma once

#include <cstdint>
#include <span>

// Observes calls the title makes through a function pointer it keeps in
// memory -- a vtable slot -- without changing what they do.
//
// A slot registered here is pointed, once the title is linked and before any
// of its code runs, at a stub of four guest instructions after an HLE call:
// the call tells the probe the caller's registers, then the stub branches to
// the target the slot held. Nothing else is patched, so a registration whose
// slot does not hold the target it names is refused and left alone. With
// nothing registered this is a no-op and the build behaves as upstream.
namespace GuestCallProbes
{
	// How a registration ended when the title was linked.
	enum class Installation
	{
		// The slot now calls through the probe.
		Installed,
		// The slot held another target: another executable, or another
		// revision of it. Left as it was.
		SlotHeldOther,
		// No code space for the stub, or every HLE index taken.
		NoCodeSpace,
	};

	class Probe
	{
	public:
		virtual ~Probe() = default;
		// Once, on the thread that links the title, before its code runs.
		virtual void OnInstall(Installation installation) = 0;
		// Each call through the slot, on the calling guest thread, before
		// the target runs; `gpr` are the caller's integer registers. Must not
		// change guest state.
		virtual void OnCall(std::span<const uint32_t, 32> gpr) = 0;
	};

	// Registers `probe` for the slot at guest address `slotAddress`, expected
	// to hold `expectedTarget`. Before the title is linked; the probe must
	// outlive the process's guest execution.
	void Register(uint32_t slotAddress, uint32_t expectedTarget, Probe& probe);

	// The host bytes behind `size` bytes of guest memory at `address`, or
	// null unless all of them are mapped: a probe reads the title's objects
	// through this, and names guest memory as the renderer's draws do.
	const void* GuestBytes(uint32_t address, uint32_t size);

	// Installs every registration. Called by the system once the title's
	// modules are linked, before control passes to it.
	void InstallRegistered();
}
