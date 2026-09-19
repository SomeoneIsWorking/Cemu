#pragma once

#include "Cafe/HW/Latte/Core/LatteCaptureWindow.h"

#include <unordered_map>
#include <vector>

// Bounded measurement of the display lists one frame references.
//
// An interpolated frame has to be rendered from the same draw stream as the
// tick it sits between, which means that stream must be replayable. The guest
// hands it over as IT_INDIRECT_BUFFER: a physical address and a dword count,
// pushed onto the draw pass and consumed in place. Replay therefore needs a
// copy, because the guest reuses that storage for the next frame.
//
// This measures what such a copy would cost and whether the addresses recur,
// before anything is built on the assumption. It records sizes and addresses
// rather than contents: the question is the shape of the stream, and copying
// every dword of every frame to disk would answer it no better while writing
// far more.
class LatteDisplayListCapture
{
  public:
	static LatteDisplayListCapture& GetInstance();

	bool IsRecording()
	{
		return m_window.IsOpen();
	}

	void RecordList(uint32 physicalAddress, uint32 sizeInDWords);
	void NotifyFrameEnd();

  private:
	LatteDisplayListCapture() = default;

	void Report(const char* reason);

	static constexpr uint32 kFramesToMeasure = 8;
	static constexpr uint32 kMaxListsPerFrame = 65536;

	struct FrameTotals
	{
		uint32 lists{0};
		uint64 dwords{0};
	};

	LatteCaptureWindow m_window{"DISPLAY_LIST_CAPTURE", LogType::DisplayListCapture,
								kFramesToMeasure};
	FrameTotals m_current;
	std::vector<FrameTotals> m_frames;
	// How many frames each address appeared in, which is what says whether the
	// guest reuses its display list storage between frames.
	std::unordered_map<uint32, uint32> m_framesPerAddress;
	std::vector<uint32> m_addressesThisFrame;
	uint64 m_listsOverCap{0};
};
