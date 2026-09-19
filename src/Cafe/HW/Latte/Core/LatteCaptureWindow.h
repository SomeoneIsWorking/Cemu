#pragma once

#include "Cemu/Logging/CemuLogging.h"

// The span of frames a capture records, and the counting that places it.
//
// Capture cannot start at the first rendered frame: that frame is the boot
// logo, and a measurement taken there describes the logo rather than the game.
// Both captures need the same placement, so they share it here rather than
// keeping two copies that drift.
//
// Frames are counted whether or not they are recorded, because the window
// cannot be placed otherwise, and waiting is reported periodically: a run that
// ended at frame 400 while waiting for frame 3000 has to be distinguishable
// from one where capture was never switched on.
class LatteCaptureWindow
{
  public:
	LatteCaptureWindow(const char* name, LogType logType, uint32 defaultFrames);

	// Arms on first use and reports whether this frame is inside the window.
	// A guard that could only become true after the guarded call had already
	// run would never fire at all.
	bool IsOpen();

	bool IsEnabled() const
	{
		return m_enabled;
	}
	uint32 FrameIndex() const
	{
		return m_frameIndex;
	}
	bool HasFinished() const
	{
		return m_finished;
	}

	// Advances one frame. Returns true when the window has just opened, so the
	// caller can start whatever it defers until then.
	bool AdvanceFrame();

	// Ends the window early; the caller reports its own totals.
	void Close()
	{
		m_finished = true;
	}

  private:
	void Arm();
	static uint32 ReadFrameOverride(const char* name, uint32 fallback);

	static constexpr uint32 kWaitingReportInterval = 600;

	const char* m_name;
	LogType m_logType;
	bool m_armed{false};
	bool m_enabled{false};
	bool m_open{false};
	bool m_finished{false};
	uint32 m_startFrame{0};
	uint32 m_frameCount;
	uint32 m_frameIndex{0};
};
