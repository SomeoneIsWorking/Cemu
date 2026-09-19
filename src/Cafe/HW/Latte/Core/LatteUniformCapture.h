#pragma once

#include <cstdio>

struct LatteDecompilerShader;

// Bounded capture of the assembled shader uniform buffer, written once per draw.
//
// This exists to answer one reverse-engineering question: which uniform slots
// carry the camera and which carry per-actor transforms. Call-shape statistics
// cannot separate them, because both paths write matrices of the same size; the
// values can, since a camera is identical across every draw within a frame and
// an actor transform is not.
//
// It captures the buffer after both uniform modes have converged on it, so a
// title driving uniform registers and uniform blocks is recorded the same way.
class LatteUniformCapture
{
  public:
	static LatteUniformCapture& GetInstance();

	// True only while capture is enabled and still within its budget. This
	// performs the one-time open, because a guard that could only become true
	// after the guarded call had already run would never fire at all -- and a
	// capture that silently never starts looks exactly like one nobody enabled.
	bool IsRecording()
	{
		if (!m_started)
		{
			Begin();
		}
		return m_recording;
	}

	void RecordDraw(uint32 shaderStageIndex, const LatteDecompilerShader* shader,
					const float* uniformData, uint32 uniformRangeSize);
	void NotifyFrameEnd();

  private:
	LatteUniformCapture() = default;

	void Begin();
	void Finish(const char* reason);

	static constexpr uint32 kMaxFrames = 4;
	static constexpr uint32 kMaxDrawsPerFrame = 8192;
	static constexpr uint64 kMaxBytes = 64ull * 1024ull * 1024ull;
	static constexpr uint32 kRecordMagic = 0x554E4946; // 'UNIF'

	bool m_started{false};
	bool m_recording{false};
	FILE* m_file{nullptr};
	uint32 m_frameIndex{0};
	uint32 m_drawsThisFrame{0};
	uint64 m_drawsSeen{0};
	uint64 m_drawsWritten{0};
	uint64 m_bytesWritten{0};
	uint64 m_drawsSkippedOverBudget{0};
};
