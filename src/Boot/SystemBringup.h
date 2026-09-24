#pragma once

// Bringing the emulated system up: crypto, timing, configuration, audio,
// input, graphic packs, the Cafe system itself, and the title and save lists.
//
// This lives here rather than in an entry point because every front end needs
// exactly this sequence in exactly this order, and a second copy of it drifts
// the moment one of them gains a step.
namespace SystemBringup
{
	void Run();

	// Ends a process with the given exit code. After Run, it first stops what
	// can be stopped -- the title list's scan worker, the Cafe system's IOSU
	// services and the input manager's update thread -- and flushes the log,
	// then leaves without running static destructors: the legacy IOSU threads
	// are detached loops with no way to stop them, and destroying the globals
	// they wait on hangs the exit. The wx front end ends the same way, with
	// _Exit from its OnExit. Without Run it simply exits.
	[[noreturn]] void Exit(int code);
} // namespace SystemBringup
