#pragma once

#include "Cafe/HW/Latte/Core/LatteCaptureWindow.h"

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
//
// Each record also carries the guest physical address of every uniform block
// the draw sourced. Values alone cannot say which actor a transform belongs
// to: draw order is not stable, because objects enter and leave between
// frames, and matching by position swaps identity whenever two actors pass
// close. The address is the engine's own storage for that object.
class LatteUniformCapture
{
  public:
	static LatteUniformCapture& GetInstance();

	bool IsRecording();

	void RecordDraw(uint32 shaderStageIndex, const LatteDecompilerShader* shader,
					const float* uniformData, uint32 uniformRangeSize);
	void NotifyFrameEnd();

  private:
	LatteUniformCapture() = default;

	bool OpenFile();
	void Finish(const char* reason);
	uint32 CollectBufferSources(const LatteDecompilerShader* shader, uint32* sources);

	static constexpr uint32 kDefaultFrames = 4;
	static constexpr uint32 kMaxDrawsPerFrame = 8192;
	static constexpr uint64 kMaxBytes = 64ull * 1024ull * 1024ull;
	static constexpr uint32 kRecordMagic = 0x32494E55; // 'UNI2'
	// Bounds the per-draw address list. Its only job is to stop a corrupt
	// shader from writing an unbounded record; real draws use a handful.
	static constexpr uint32 kMaxBufferGroups = 16;

	LatteCaptureWindow m_window{"UNIFORM_CAPTURE", LogType::UniformCapture, kDefaultFrames};
	FILE* m_file{nullptr};
	bool m_openFailed{false};
	uint32 m_drawsThisFrame{0};
	uint64 m_drawsSeen{0};
	uint64 m_drawsWritten{0};
	uint64 m_bytesWritten{0};
	uint64 m_drawsSkippedOverBudget{0};
	uint64 m_drawsWithoutSources{0};
	uint64 m_bufferGroupsDropped{0};
};
