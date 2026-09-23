#pragma once

#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"

class LatteTexture;

// Keeps what the runtime overwrites, so the guest's frame can be put back by
// copying it rather than by drawing it a second time.
//
// A runtime that draws a frame of its own over the guest's -- an in-between
// frame -- leaves every render target it touched holding that frame, and the
// guest's next frame reads some of them back. While the guard is open, the
// first write to each texture subresource copies it aside; closing the guard
// copies every one of them back.
//
// The copies restore texture contents, and only those. The texture cache's
// bookkeeping is left as the runtime's draws left it, which is what drawing
// the guest's frame again would also have left: both issue the same writes to
// the same subresources in the same order. What the copies cannot restore is
// counted instead, and any of it means the caller must restore another way.
namespace LatteGuestStateGuard
{
	void Open();
	LatteFrameHooks::GuestStateRestore Close();
	bool IsOpen();

	// The renderer calls these before it writes, and the texture cache as a
	// texture comes and goes. Each is a no-op while the guard is closed,
	// except that a deleted texture always releases its copies.
	void NoteWrite(LatteTexture* texture, sint32 sliceIndex, sint32 mipIndex);
	void NoteCreated(LatteTexture* texture);
	void NoteDeleted(LatteTexture* texture);
	void NoteStreamoutWrite();
} // namespace LatteGuestStateGuard
