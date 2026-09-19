#include "Cafe/HW/Latte/Core/LatteDisplayListCapture.h"

#include "Cemu/Logging/CemuLogging.h"

#include <algorithm>

LatteDisplayListCapture& LatteDisplayListCapture::GetInstance()
{
	static LatteDisplayListCapture s_instance;
	return s_instance;
}

void LatteDisplayListCapture::RecordList(uint32 physicalAddress, uint32 sizeInDWords)
{
	if (m_current.lists >= kMaxListsPerFrame)
	{
		m_listsOverCap++;
		return;
	}
	m_current.lists++;
	m_current.dwords += sizeInDWords;
	m_addressesThisFrame.push_back(physicalAddress);
}

void LatteDisplayListCapture::NotifyFrameEnd()
{
	if (!m_window.IsOpen())
	{
		// Outside the window there is nothing to total, but frames still have
		// to be counted or the window can never open.
		m_window.AdvanceFrame();
		return;
	}
	std::sort(m_addressesThisFrame.begin(), m_addressesThisFrame.end());
	m_addressesThisFrame.erase(
		std::unique(m_addressesThisFrame.begin(), m_addressesThisFrame.end()),
		m_addressesThisFrame.end());
	for (uint32 address : m_addressesThisFrame)
	{
		m_framesPerAddress[address]++;
	}
	m_addressesThisFrame.clear();
	m_frames.push_back(m_current);
	m_current = FrameTotals{};
	m_window.AdvanceFrame();
	if (m_window.HasFinished())
	{
		Report("measured");
	}
}

void LatteDisplayListCapture::Report(const char* reason)
{
	uint64 lists = 0;
	uint64 dwords = 0;
	for (const FrameTotals& frame : m_frames)
	{
		lists += frame.lists;
		dwords += frame.dwords;
	}
	const uint64 frames = m_frames.empty() ? 1 : m_frames.size();
	// Unconditional and with denominators, including the zero case: a
	// measurement that produced nothing must not read like one nobody enabled.
	cemuLog_log(LogType::Force,
				"DisplayListCapture: {} {} frames. Lists {} ({} per frame), dwords {} ({} bytes "
				"per frame), lists over the per-frame cap {}",
				reason, m_frames.size(), lists, lists / frames, dwords, (dwords * 4) / frames,
				m_listsOverCap);
	uint32 reusedAddresses = 0;
	for (const auto& [address, frameCount] : m_framesPerAddress)
	{
		if (frameCount > 1)
		{
			reusedAddresses++;
		}
	}
	// The number that decides whether replay needs a copy. If the guest hands
	// back the same storage every frame, the contents at that address are the
	// next frame's by the time a replay would read them.
	cemuLog_log(LogType::Force,
				"DisplayListCapture: distinct addresses {}, of which reused across frames {}",
				m_framesPerAddress.size(), reusedAddresses);
}
