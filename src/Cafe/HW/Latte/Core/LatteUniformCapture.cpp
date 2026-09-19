#include "Cafe/HW/Latte/Core/LatteUniformCapture.h"

#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cemu/Logging/CemuLogging.h"
#include "config/ActiveSettings.h"

#include <charconv>
#include <cstdlib>
#include <string_view>

namespace
{
	// Reads one unsigned override, refusing rather than silently keeping the
	// default when the value is present but unusable: a typo that quietly captures
	// the wrong window costs a whole run to discover.
	uint32 ReadFrameOverride(const char* name, uint32 fallback)
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
			cemuLog_log(LogType::Force, "UniformCapture: {} is set to \"{}\", which is not a whole number of frames; using {}",
						name, text, fallback);
			return fallback;
		}
		return value;
	}
} // namespace

LatteUniformCapture& LatteUniformCapture::GetInstance()
{
	static LatteUniformCapture s_instance;
	return s_instance;
}

void LatteUniformCapture::Arm()
{
	m_armed = true;
	if (!cemuLog_isLoggingEnabled(LogType::UniformCapture))
	{
		return;
	}
	m_enabled = true;
	m_startFrame = ReadFrameOverride("CEMU_UNIFORM_CAPTURE_START_FRAME", 0);
	m_frameCount = ReadFrameOverride("CEMU_UNIFORM_CAPTURE_FRAMES", kDefaultFrames);
	if (m_frameCount == 0)
	{
		cemuLog_log(LogType::Force, "UniformCapture: a window of zero frames captures nothing; using {}",
					kDefaultFrames);
		m_frameCount = kDefaultFrames;
	}
	cemuLog_log(LogType::Force, "UniformCapture: waiting for frame {}, then capturing {} frames",
				m_startFrame, m_frameCount);
	if (m_startFrame == 0)
	{
		m_recording = OpenFile();
	}
}

bool LatteUniformCapture::OpenFile()
{
	const fs::path path = ActiveSettings::GetUserDataPath("uniform-capture.bin");
	m_file = fopen(path.string().c_str(), "wb");
	if (m_file == nullptr)
	{
		cemuLog_log(LogType::Force, "UniformCapture: cannot open {} for writing; nothing was captured",
					path.string());
		m_enabled = false;
		return false;
	}
	cemuLog_log(LogType::Force, "UniformCapture: recording from frame {} to {}", m_frameIndex, path.string());
	return true;
}

void LatteUniformCapture::Finish(const char* reason)
{
	m_recording = false;
	m_finished = true;
	if (m_file != nullptr)
	{
		fclose(m_file);
		m_file = nullptr;
	}
	// Reported with denominators and unconditionally, including the zero case:
	// a capture that silently produced nothing cannot be told from one that was
	// never switched on.
	cemuLog_log(LogType::Force,
				"UniformCapture: {} at frame {}. Draws seen {}, written {}, skipped over budget "
				"{}, bytes {}. Draws with no uniform block source {}, buffer groups dropped over "
				"the per-draw cap {}",
				reason, m_frameIndex, m_drawsSeen, m_drawsWritten, m_drawsSkippedOverBudget,
				m_bytesWritten, m_drawsWithoutSources, m_bufferGroupsDropped);
}

void LatteUniformCapture::RecordDraw(uint32 shaderStageIndex, const LatteDecompilerShader* shader,
									 const float* uniformData, uint32 uniformRangeSize)
{
	if (!m_recording)
	{
		return;
	}
	m_drawsSeen++;
	if (uniformData == nullptr || uniformRangeSize == 0)
	{
		return;
	}
	if (m_drawsThisFrame >= kMaxDrawsPerFrame || m_bytesWritten >= kMaxBytes)
	{
		m_drawsSkippedOverBudget++;
		return;
	}
	uint32 sources[kMaxBufferGroups * 2];
	const uint32 sourceCount = CollectBufferSources(shader, sources);
	// One self-describing record: the layout fields are what let a reader find
	// the register and remapped blocks inside the payload without guessing,
	// and the source addresses are what give a draw an identity across frames.
	const uint32 header[] = {
		kRecordMagic,
		m_frameIndex,
		shaderStageIndex,
		uniformRangeSize,
		static_cast<uint32>(shader->uniform.loc_uniformRegister),
		static_cast<uint32>(shader->uniform.count_uniformRegister),
		static_cast<uint32>(shader->uniform.loc_remapped),
		sourceCount,
	};
	const uint64 hashes[] = {shader->baseHash, shader->auxHash};
	fwrite(header, sizeof(header), 1, m_file);
	fwrite(hashes, sizeof(hashes), 1, m_file);
	fwrite(sources, sizeof(uint32) * 2, sourceCount, m_file);
	fwrite(uniformData, uniformRangeSize, 1, m_file);
	m_drawsThisFrame++;
	m_drawsWritten++;
	m_bytesWritten += sizeof(header) + sizeof(hashes) + sourceCount * sizeof(uint32) * 2 + uniformRangeSize;
	if (sourceCount == 0)
	{
		m_drawsWithoutSources++;
	}
}

uint32 LatteUniformCapture::CollectBufferSources(const LatteDecompilerShader* shader,
												 uint32* sources)
{
	const uint32 registerOffset =
		LatteBufferCache_getUniformBlockRegisterOffset(shader->shaderType);
	uint32 count = 0;
	for (const auto& group : shader->list_remappedUniformEntries_bufferGroups)
	{
		if (count >= kMaxBufferGroups)
		{
			m_bufferGroupsDropped++;
			break;
		}
		sources[count * 2 + 0] = group.bufferId;
		sources[count * 2 + 1] =
			LatteGPUState.contextRegister[registerOffset + group.kcacheBankIdOffset / 4];
		count++;
	}
	return count;
}

void LatteUniformCapture::NotifyFrameEnd()
{
	if (!m_enabled || m_finished)
	{
		return;
	}
	if (m_recording)
	{
		// Flushed per frame: a run that is stopped before the budget is reached
		// must still leave a readable file rather than whatever happened to be
		// in the buffer.
		fflush(m_file);
	}
	m_frameIndex++;
	m_drawsThisFrame = 0;
	if (!m_recording)
	{
		if (m_frameIndex < m_startFrame)
		{
			if (m_frameIndex % kWaitingReportInterval == 0)
			{
				cemuLog_log(LogType::Force, "UniformCapture: at frame {} of {} before capture starts",
							m_frameIndex, m_startFrame);
			}
			return;
		}
		m_recording = OpenFile();
		if (!m_recording)
		{
			return;
		}
	}
	if (m_frameIndex >= m_startFrame + m_frameCount)
	{
		Finish("reached its frame budget");
	}
}
