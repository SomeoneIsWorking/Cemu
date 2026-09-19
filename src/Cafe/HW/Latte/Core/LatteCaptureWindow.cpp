#include "Cafe/HW/Latte/Core/LatteCaptureWindow.h"

#include "Cemu/Logging/CemuLogging.h"

#include <charconv>
#include <cstdlib>
#include <string>
#include <string_view>

LatteCaptureWindow::LatteCaptureWindow(const char* name, LogType logType, uint32 defaultFrames)
	: m_name(name), m_logType(logType), m_frameCount(defaultFrames)
{
}

uint32 LatteCaptureWindow::ReadFrameOverride(const char* name, uint32 fallback)
{
	const char* raw = getenv(name);
	if (raw == nullptr)
	{
		return fallback;
	}
	const std::string_view text{raw};
	uint32 value = 0;
	const auto* end = text.data() + text.size();
	const auto parsed = std::from_chars(text.data(), end, value);
	if (parsed.ec != std::errc{} || parsed.ptr != end)
	{
		// Refused rather than silently kept: a typo that quietly captures the
		// wrong window costs a whole run to discover.
		cemuLog_log(LogType::Force,
					"{} is set to \"{}\", which is not a whole number of frames; using {}", name,
					text, fallback);
		return fallback;
	}
	return value;
}

void LatteCaptureWindow::Arm()
{
	m_armed = true;
	if (!cemuLog_isLoggingEnabled(m_logType))
	{
		return;
	}
	m_enabled = true;
	const std::string prefix = std::string("CEMU_") + m_name;
	m_startFrame = ReadFrameOverride((prefix + "_START_FRAME").c_str(), 0);
	m_frameCount = ReadFrameOverride((prefix + "_FRAMES").c_str(), m_frameCount);
	if (m_frameCount == 0)
	{
		cemuLog_log(LogType::Force, "{}: a window of zero frames captures nothing; using 1",
					m_name);
		m_frameCount = 1;
	}
	cemuLog_log(LogType::Force, "{}: waiting for frame {}, then capturing {} frames", m_name,
				m_startFrame, m_frameCount);
	m_open = m_startFrame == 0;
}

bool LatteCaptureWindow::IsOpen()
{
	if (!m_armed)
	{
		Arm();
	}
	return m_open && !m_finished;
}

bool LatteCaptureWindow::AdvanceFrame()
{
	if (!m_enabled || m_finished)
	{
		return false;
	}
	m_frameIndex++;
	if (!m_open)
	{
		if (m_frameIndex < m_startFrame)
		{
			if (m_frameIndex % kWaitingReportInterval == 0)
			{
				cemuLog_log(LogType::Force, "{}: at frame {} of {} before capture starts", m_name,
							m_frameIndex, m_startFrame);
			}
			return false;
		}
		m_open = true;
		return true;
	}
	if (m_frameIndex >= m_startFrame + m_frameCount)
	{
		m_finished = true;
	}
	return false;
}
