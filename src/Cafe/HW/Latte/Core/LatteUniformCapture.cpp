#include "Cafe/HW/Latte/Core/LatteUniformCapture.h"

#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cemu/Logging/CemuLogging.h"
#include "config/ActiveSettings.h"

#include <charconv>
#include <cstdlib>
#include <string_view>

LatteUniformCapture& LatteUniformCapture::GetInstance()
{
	static LatteUniformCapture s_instance;
	return s_instance;
}

bool LatteUniformCapture::IsRecording()
{
	if (!m_window.IsOpen())
	{
		return false;
	}
	if (m_file == nullptr && !m_openFailed)
	{
		OpenFile();
	}
	return m_file != nullptr;
}

bool LatteUniformCapture::OpenFile()
{
	const fs::path path = ActiveSettings::GetUserDataPath("uniform-capture.bin");
	m_file = fopen(path.string().c_str(), "wb");
	if (m_file == nullptr)
	{
		m_openFailed = true;
		cemuLog_log(LogType::Force,
					"UniformCapture: cannot open {} for writing; nothing was captured",
					path.string());
		return false;
	}
	cemuLog_log(LogType::Force, "UniformCapture: recording from frame {} to {}",
				m_window.FrameIndex(), path.string());
	return true;
}

void LatteUniformCapture::Finish(const char* reason)
{
	m_window.Close();
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
				reason, m_window.FrameIndex(), m_drawsSeen, m_drawsWritten, m_drawsSkippedOverBudget,
				m_bytesWritten, m_drawsWithoutSources, m_bufferGroupsDropped);
}

void LatteUniformCapture::RecordDraw(uint32 shaderStageIndex, const LatteDecompilerShader* shader,
									 const float* uniformData, uint32 uniformRangeSize)
{
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
		m_window.FrameIndex(),
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
	if (m_file != nullptr)
	{
		// Flushed per frame: a run stopped before the budget is reached must
		// still leave a readable file rather than whatever was in the buffer.
		fflush(m_file);
	}
	m_window.AdvanceFrame();
	m_drawsThisFrame = 0;
	if (m_window.HasFinished() && m_file != nullptr)
	{
		Finish("reached its frame budget");
	}
}
