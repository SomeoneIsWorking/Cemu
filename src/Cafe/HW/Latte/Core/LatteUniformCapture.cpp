#include "Cafe/HW/Latte/Core/LatteUniformCapture.h"

#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cemu/Logging/CemuLogging.h"
#include "config/ActiveSettings.h"

LatteUniformCapture& LatteUniformCapture::GetInstance()
{
	static LatteUniformCapture s_instance;
	return s_instance;
}

void LatteUniformCapture::Begin()
{
	m_started = true;
	if (!cemuLog_isLoggingEnabled(LogType::UniformCapture))
	{
		return;
	}
	const fs::path path = ActiveSettings::GetUserDataPath("uniform-capture.bin");
	m_file = fopen(path.string().c_str(), "wb");
	if (m_file == nullptr)
	{
		cemuLog_log(LogType::Force, "UniformCapture: cannot open {} for writing; nothing was captured",
					path.string());
		return;
	}
	m_recording = true;
	cemuLog_log(LogType::Force, "UniformCapture: writing up to {} frames to {}", kMaxFrames,
				path.string());
}

void LatteUniformCapture::Finish(const char* reason)
{
	m_recording = false;
	if (m_file != nullptr)
	{
		fclose(m_file);
		m_file = nullptr;
	}
	// Reported with denominators and unconditionally, including the zero case:
	// a capture that silently produced nothing cannot be told from one that was
	// never switched on.
	cemuLog_log(LogType::Force,
				"UniformCapture: {} after {} frames. Draws seen {}, written {}, skipped over budget "
				"{}, bytes {}",
				reason, m_frameIndex, m_drawsSeen, m_drawsWritten, m_drawsSkippedOverBudget,
				m_bytesWritten);
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
	// One self-describing record: the layout fields are what let a reader find
	// the register and remapped blocks inside the payload without guessing.
	const uint32 header[] = {
		kRecordMagic,
		m_frameIndex,
		shaderStageIndex,
		uniformRangeSize,
		static_cast<uint32>(shader->uniform.loc_uniformRegister),
		static_cast<uint32>(shader->uniform.count_uniformRegister),
		static_cast<uint32>(shader->uniform.loc_remapped),
	};
	const uint64 hashes[] = {shader->baseHash, shader->auxHash};
	fwrite(header, sizeof(header), 1, m_file);
	fwrite(hashes, sizeof(hashes), 1, m_file);
	fwrite(uniformData, uniformRangeSize, 1, m_file);
	m_drawsThisFrame++;
	m_drawsWritten++;
	m_bytesWritten += sizeof(header) + sizeof(hashes) + uniformRangeSize;
}

void LatteUniformCapture::NotifyFrameEnd()
{
	if (!m_recording)
	{
		return;
	}
	// Flushed per frame: a run that is stopped before the budget is reached
	// must still leave a readable file rather than whatever happened to be in
	// the buffer.
	fflush(m_file);
	m_frameIndex++;
	m_drawsThisFrame = 0;
	if (m_frameIndex >= kMaxFrames)
	{
		Finish("reached its frame budget");
	}
}
