#pragma once

#include <cstdint>

// Writes into the guest's own memory while it runs: code it may execute and
// data it will read back.
//
// A first-party runtime that wants the title to behave differently has nowhere
// else to put the change -- the player's disc image is never written -- so the
// guest's address space is the seam. Everything here goes through the
// emulator's own memory, refuses a range that is not mapped rather than
// writing part of it, and invalidates the recompiler over what it wrote, so a
// patched instruction is the next one the guest executes rather than the last
// one it compiled.
//
// The guest is big-endian and the host is not, which is why words are separate
// from bytes: a word crossing this boundary is a value, not a copy, and a
// caller that byte-copies one gets a byte-swapped address and finds out at the
// worst possible moment. Bytes stay bytes, because a block of guest
// instructions is already in the order the guest will read it.
namespace GuestPatching
{
	// `sizeInBytes` of guest memory the guest may execute from, or 0 when
	// there is none. The block comes from the loader's trampoline area and is
	// never handed back: a patch written into it has to outlive the call that
	// wrote it, and nothing knows when that is.
	//
	// "May execute from" is meant in both senses. The memory is registered with
	// the recompiler, so a call reaching it indirectly -- through a vtable, say
	// -- finds its translated code, and not only a direct branch to the address.
	uint32_t AllocateCode(uint32_t sizeInBytes);

	// The guest's own order, as the guest would load or store the word.
	bool ReadWord(uint32_t guestAddress, uint32_t& value);
	bool WriteWord(uint32_t guestAddress, uint32_t value);
	// True when `guestAddress` is inside a block AllocateCode handed out and
	// nothing has been written to it since, which is what tells a write there
	// from a write over code the guest may already have compiled. A write into a
	// fresh block needs no invalidation and must not take the recompiler's lock
	// to say so; a *second* write to the same block is not fresh, because the
	// guest may have run from it in between.
	bool IsFreshCode(uint32_t guestAddress);

	// A block of guest bytes, as they lie, for a caller that already holds the
	// guest's order -- an instruction block read out of the guest's own image,
	// say. A caller holding a host word array does not: it wants WriteWord.
	// False unless the whole range is mapped, and then nothing is written.
	bool WriteBytes(uint32_t guestAddress, const void* bytes, uint32_t sizeInBytes);
	// How many vblanks a flip takes, which is what paces a title that waits for
	// its own flip: at two a 60 Hz display presents thirty pictures a second, at
	// one it presents sixty, and nothing else about the picture changes.
	//
	// This is the emulator's pacing and not the title's state. The title's own
	// record of what it asked for is a field in the display object, and a caller
	// that wants the two to agree writes that too -- which is a different
	// operation, in guest memory, and is the one that leaves evidence.
	//
	// Refused past the range `GX2SetSwapInterval` accepts, and the current value
	// is returned either way, so a caller can restore what it found.
	uint32_t SetSwapInterval(uint32_t vblanksPerFlip);

	// The vblanks a flip takes now.
	uint32_t SwapInterval();
} // namespace GuestPatching
