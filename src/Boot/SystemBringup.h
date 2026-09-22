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
} // namespace SystemBringup
