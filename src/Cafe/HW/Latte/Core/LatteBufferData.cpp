#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Core/Latte.h"

#include <atomic>
#include "Cafe/HW/Latte/Core/LatteDraw.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/GameProfile/GameProfile.h"

#include "Cafe/HW/Latte/Core/LatteBufferCache.h"
#ifdef ENABLE_VULKAN
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#endif

template<int vectorLen>
void rectGenerate4thVertex(uint32be* output, uint32be* input0, uint32be* input1, uint32be* input2)
{
	float* v = (float*)output;

	for (sint32 i = 0; i < vectorLen; i++)
		output[vectorLen * 0 + i] = _swapEndianU32(input0[i]);
	for (sint32 i = 0; i < vectorLen; i++)
		output[vectorLen * 1 + i] = _swapEndianU32(input1[i]);
	for (sint32 i = 0; i < vectorLen; i++)
		output[vectorLen * 2 + i] = _swapEndianU32(input2[i]);

	float minX = std::min(v[vectorLen * 0 + 0], std::min(v[vectorLen * 1 + 0], v[vectorLen * 2 + 0]));
	float maxX = std::max(v[vectorLen * 0 + 0], std::max(v[vectorLen * 1 + 0], v[vectorLen * 2 + 0]));
	float minY = std::min(v[vectorLen * 0 + 1], std::min(v[vectorLen * 1 + 1], v[vectorLen * 2 + 1]));;
	float maxY = std::max(v[vectorLen * 0 + 1], std::max(v[vectorLen * 1 + 1], v[vectorLen * 2 + 1]));;

	float totalX = minX;
	totalX += maxY;
	float halfX = totalX / 2.0f;

	float totalY = minY;
	totalY += maxY;
	float halfY = totalY / 2.0f;

	sint32 countX =
		((v[vectorLen * 0 + 0] < halfX) ? 1 : 0) +
		((v[vectorLen * 1 + 0] < halfX) ? 1 : 0) +
		((v[vectorLen * 2 + 0] < halfX) ? 1 : 0);

	sint32 countY =
		((v[vectorLen * 0 + 1] < halfY) ? 1 : 0) +
		((v[vectorLen * 1 + 1] < halfY) ? 1 : 0) +
		((v[vectorLen * 2 + 1] < halfY) ? 1 : 0);

	if (countX < 2)
		v[vectorLen * 3 + 0] = minX;
	else
		v[vectorLen * 3 + 0] = maxX;
	if (countY < 2)
		v[vectorLen * 3 + 1] = minY;
	else
		v[vectorLen * 3 + 1] = maxY;

	if (vectorLen >= 3)
		v[vectorLen * 3 + 2] = v[vectorLen * 0 + 2]; // z from v0
	if (vectorLen >= 4)
		v[vectorLen * 3 + 3] = v[vectorLen * 0 + 3]; // w from v0

	// order of rectangle vertices is
	// v0 v1
	// v2 v3

	for (sint32 f = 0; f < vectorLen*4; f++)
		output[f] = _swapEndianU32(output[f]);
}

uint32 LatteBufferCache_getUniformBlockRegisterOffset(LatteConst::ShaderType shaderType)
{
	switch (shaderType)
	{
	case LatteConst::ShaderType::Vertex:
		return mmSQ_VTX_UNIFORM_BLOCK_START;
	case LatteConst::ShaderType::Pixel:
		return mmSQ_PS_UNIFORM_BLOCK_START;
	case LatteConst::ShaderType::Geometry:
		return mmSQ_GS_UNIFORM_BLOCK_START;
	default:
		UNREACHABLE;
	}
}

namespace
{
	// The guest address the title passed for each uniform block slot, beside the physical one the
	// register holds. **Bounded, lock-free, and written by the CPU while the render thread reads
	// it**: the guest sets a block on its own thread and the draw reads it on the render thread, so
	// a plain array would be a data race and a lock would be a frame-time spike. A relaxed store
	// per slot and an acquire read of the same slot is enough, because each word is independent
	// and a reader that sees a stale address reads a block the title set earlier rather than a
	// torn value.
	//
	// 16 slots per stage, three stages, from RegDefines.h's `mmSQ_*_UNIFORM_BLOCK_END - START + 1`
	// divided by the 7 dwords a slot occupies. The size is checked against those defines at the
	// point of use rather than repeated here.
	constexpr uint32 kUniformBlockSlotsPerStage = 16;
	constexpr uint32 kUniformBlockStages = 3;
	std::atomic<uint32> g_uniformBlockGuestAddress[kUniformBlockStages][kUniformBlockSlotsPerStage];

	uint32 uniformBlockStageIndex(LatteConst::ShaderType shaderType)
	{
		switch (shaderType)
		{
		case LatteConst::ShaderType::Vertex:
			return 0;
		case LatteConst::ShaderType::Pixel:
			return 1;
		case LatteConst::ShaderType::Geometry:
			return 2;
		default:
			UNREACHABLE;
		}
	}
} // namespace

void LatteBufferCache_noteUniformBlockGuestAddress(LatteConst::ShaderType shaderType, uint32_t index,
												   uint32_t guestAddress)
{
	const uint32 stage = uniformBlockStageIndex(shaderType);
	if (index >= kUniformBlockSlotsPerStage)
	{
		return;
	}
	g_uniformBlockGuestAddress[stage][index].store(guestAddress, std::memory_order_relaxed);
}

uint32 LatteBufferCache_collectUniformBlockSources(LatteDecompilerShader* shader, uint32* pairs,
												   uint32* guests, uint32* sizes, uint32 maxPairs,
												   uint32* droppedOverCap)
{
	const uint32 registerOffset = LatteBufferCache_getUniformBlockRegisterOffset(shader->shaderType);
	const uint32 stage = uniformBlockStageIndex(shader->shaderType);
	// **Which slot, and what the slot holds.** The guest indexes these registers by the index it
	// passes to `GX2Set*UniformBlock`; the shader names its groups by `kcacheBankIdOffset`, which
	// is the same slot index times 7 dwords, so the two agree about *which* slot and the pair below
	// is that slot's own contents. Word 0 is the physical address `memory_base + ...` reads, and
	// word 1 is `size - 1` as the guest wrote it.
	uint32 count = 0;
	for (const auto& group : shader->list_remappedUniformEntries_bufferGroups)
	{
		if (count >= maxPairs)
		{
			if (droppedOverCap != nullptr)
				(*droppedOverCap)++;
			break;
		}
		const uint32 slot = group.kcacheBankIdOffset / (7 * 4);
		pairs[count * 2 + 0] = group.bufferId;
		pairs[count * 2 + 1] =
			LatteGPUState.contextRegister[registerOffset + group.kcacheBankIdOffset / 4];
		sizes[count] =
			LatteGPUState.contextRegister[registerOffset + group.kcacheBankIdOffset / 4 + 1];
		if (slot < kUniformBlockSlotsPerStage && guests != nullptr)
		{
			guests[count] = g_uniformBlockGuestAddress[stage][slot].load(std::memory_order_acquire);
		}
		count++;
	}
	return count;
}

bool LatteBufferCache_LoadRemappedUniforms(LatteDecompilerShader* shader, float* uniformData, bool aluConstDirty, uint32 uniformBufferDirtyMask)
{
	bool hasChange = false;
	uint32 shaderAluConst;

	switch (shader->shaderType)
	{
	case LatteConst::ShaderType::Vertex:
		shaderAluConst = 0x400;
		break;
	case LatteConst::ShaderType::Pixel:
		shaderAluConst = 0;
		break;
	case LatteConst::ShaderType::Geometry:
		shaderAluConst = 0; // geometry shader has no ALU const
		break;
	default:
		UNREACHABLE;
	}
	const uint32 shaderUniformRegisterOffset = LatteBufferCache_getUniformBlockRegisterOffset(shader->shaderType);

	// sourced from uniform registers
	if (aluConstDirty)
	{
		uint32* aluConstBase = LatteGPUState.contextRegister + mmSQ_ALU_CONSTANT0_0 + shaderAluConst;
		for (auto it : shader->list_remappedUniformEntries_register)
		{
			uint64* __restrict uniformRegData = (uint64*)(aluConstBase + it.indexOffset / 4);
			uint64* __restrict regDest = (uint64*)((uint8*)uniformData + it.mappedIndexOffset);
			regDest[0] = uniformRegData[0];
			regDest[1] = uniformRegData[1];
		}
		if (!shader->list_remappedUniformEntries_register.empty())
			hasChange = true;
	}
	// sourced from uniform buffers
	if (uniformBufferDirtyMask)
	{
		for (auto& bufferGroup : shader->list_remappedUniformEntries_bufferGroups)
		{
			if ((uniformBufferDirtyMask&(1<<bufferGroup.bufferId)) == 0)
				continue;
			MPTR physicalAddr = LatteGPUState.contextRegister[shaderUniformRegisterOffset + bufferGroup.kcacheBankIdOffset / 4];
			if (physicalAddr)
			{
				uint8* __restrict uniformBase = memory_base + physicalAddr;
				for (auto& it : bufferGroup.entries)
				{
					uint64* __restrict regDest = (uint64*)((uint8*)uniformData + it.mappedIndexOffset);
					uint64* __restrict uniformEntrySrc = (uint64*)(uniformBase + it.indexOffset);
					memcpy(regDest, uniformEntrySrc, 16);
				}
			}
			else
			{
				for (auto& it : bufferGroup.entries)
				{
					uint64* regDest = (uint64*)((uint8*)uniformData + it.mappedIndexOffset);
					regDest[0] = 0;
					regDest[1] = 0;
				}
			}
			hasChange = true;
		}
	}
	return hasChange;
}

bool LatteBufferCache_syncGPUUniformBuffers(LatteDecompilerShader* shader, const uint32 uniformBufferRegOffset, LatteConst::ShaderType shaderType, uint32 bufferDirtyMask)
{
	cemu_assert_debug(shader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK);
	bool hasChange = false;
	for(const auto& buf : shader->list_quickBufferList)
	{
		sint32 i = buf.index;
		if ((bufferDirtyMask&(1<<i)) == 0)
			continue;
		hasChange = true;
		MPTR physicalAddr = LatteGPUState.contextRegister[uniformBufferRegOffset + i * 7 + 0];
		uint32 uniformSize = LatteGPUState.contextRegister[uniformBufferRegOffset + i * 7 + 1] + 1;
		if (physicalAddr == MPTR_NULL) [[unlikely]]
		{
			g_renderer->buffer_bindUniformBuffer(shaderType, i, 0, 0);
			continue;
		}
		uniformSize = std::min<uint32>(uniformSize, buf.size);
		uint32 bindOffset = LatteBufferCache_retrieveDataInCache(physicalAddr, uniformSize);
		g_renderer->buffer_bindUniformBuffer(shaderType, i, bindOffset, uniformSize);
	}
	return hasChange;
}

// for detecting when vertex buffer size needs to be extended during incremental rendering
static sint32 s_vtxStateMaxIndex{};
static sint32 s_vtxStateMaxInstance{};

void LatteBufferCache_ProcessQueues()
{
	static uint32 s_syncBufferCounter = 0;

	s_syncBufferCounter++;
	if (s_syncBufferCounter >= 25)
	{
		LatteBufferCache_incrementalCleanup();
		s_syncBufferCounter = 0;
	}
	LatteBufferCache_processDCFlushQueue();
	// process queued deallocations from previous drawcall
	LatteBufferCache_processDeallocations();
}

// upload vertex and uniform buffers and update bindings
void LatteBufferCache_Sync(uint32 maxIndex, uint32 baseInstance, uint32 instanceCount, uint32 attribBufferDirtyMask, uint32 vsUniformBufferDirtyMask, uint32 psUniformBufferDirtyMask, uint32 gsUniformBufferDirtyMask, uint8& stageUniformModifiedMask, bool isIncremental)
{
	LatteFetchShader* parsedFetchShader = LatteSHRC_GetActiveFetchShader();
	cemu_assert_debug(parsedFetchShader);

	// todo - vertex attribute offsets may eventually be allowed to change between incremental draws, we should set the attrib dirty bits in that case
	if (isIncremental)
	{
		// dont process flush queue and dont process deallocations yet, we are in the middle of a sequence of drawcalls that (most likely) reuse previous bindings
		uint32 maxInstance = baseInstance + instanceCount - 1;
		bool hasBufferChange = attribBufferDirtyMask != 0;
		if ( maxIndex > s_vtxStateMaxIndex )
		{
			attribBufferDirtyMask = 0xFFFFFFFF;
			s_vtxStateMaxIndex = maxIndex;
		}
		if ( maxInstance > s_vtxStateMaxInstance )
		{
			attribBufferDirtyMask = 0xFFFFFFFF;
			s_vtxStateMaxInstance = maxInstance;
		}
		if (hasBufferChange)
		{
			s_vtxStateMaxIndex = maxIndex;
			s_vtxStateMaxInstance = maxInstance;
		}
	}
	else
	{
		LatteBufferCache_ProcessQueues();
		s_vtxStateMaxIndex = maxIndex;
		uint32 maxInstance = baseInstance + instanceCount - 1;
		s_vtxStateMaxInstance = maxInstance;
	}
	attribBufferDirtyMask &= parsedFetchShader->attributeBufferMask;

	// sync and bind dirty vertex buffers
	if (attribBufferDirtyMask != 0)
	{
		uint32* __restrict bufferRegStartPtr = LatteGPUState.contextRegister + mmSQ_VTX_ATTRIBUTE_BLOCK_START;
		for (auto& bufferGroup : parsedFetchShader->bufferGroups)
		{
			uint32 bufferIndex = bufferGroup.attributeBufferIndex;
			if ((attribBufferDirtyMask&(1<<bufferIndex)) == 0)
				continue;
			uint32* __restrict bufferRegs = bufferRegStartPtr + bufferIndex * 7;
			MPTR bufferAddress = bufferRegs[0];
			uint32 bufferStride = (bufferRegs[2] >> 11) & 0xFFFF;

			if (bufferAddress == MPTR_NULL) [[unlikely]]
			{
				g_renderer->buffer_bindVertexBuffer(bufferIndex, 0, 0);
				continue;
			}

			// dont rely on buffer size given by game
			uint32 fixedBufferSize = bufferGroup.getReadSize(bufferStride, maxIndex, baseInstance, instanceCount);


#if BOOST_OS_MACOS && defined(ENABLE_VULKAN)
			if(bufferStride % 4 != 0)
			{
				if (g_renderer->GetType() == RendererAPI::Vulkan)
				{
					if (VulkanRenderer* vkRenderer = VulkanRenderer::GetInstance())
					{
						auto fixedBuffer = vkRenderer->buffer_genStrideWorkaroundVertexBuffer(bufferAddress, fixedBufferSize, bufferStride);
						vkRenderer->buffer_bindVertexStrideWorkaroundBuffer(fixedBuffer.first, fixedBuffer.second, bufferIndex, fixedBufferSize);
						continue;
					}
				}
			}
#endif

			uint32 bindOffset = LatteBufferCache_retrieveDataInCache(bufferAddress, fixedBufferSize);
			g_renderer->buffer_bindVertexBuffer(bufferIndex, bindOffset, fixedBufferSize);
		}
	}
	// sync uniform buffers
	LatteDecompilerShader* vertexShader = LatteSHRC_GetActiveVertexShader();
	LatteDecompilerShader* geometryShader = LatteSHRC_GetActiveGeometryShader();
	LatteDecompilerShader* pixelShader = LatteSHRC_GetActivePixelShader();
	// todo - if we AND the shader uniform buffer mask and the dirty mask we can completely skip calling syncGPUUniformBuffers if no relevant buffer was updated
	if (vertexShader && vertexShader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK)
	{
		if (LatteBufferCache_syncGPUUniformBuffers(vertexShader, mmSQ_VTX_UNIFORM_BLOCK_START, LatteConst::ShaderType::Vertex, vsUniformBufferDirtyMask))
			stageUniformModifiedMask |= (1<<VulkanRendererConst::SHADER_STAGE_INDEX_VERTEX);
	}
	if (pixelShader && pixelShader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK)
	{
		if (LatteBufferCache_syncGPUUniformBuffers(pixelShader, mmSQ_PS_UNIFORM_BLOCK_START, LatteConst::ShaderType::Pixel, psUniformBufferDirtyMask))
			stageUniformModifiedMask |= (1<<VulkanRendererConst::SHADER_STAGE_INDEX_FRAGMENT); // todo - move this enum to Latte?
	}
	if (geometryShader && geometryShader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK)
	{
		if ( LatteBufferCache_syncGPUUniformBuffers(geometryShader, mmSQ_GS_UNIFORM_BLOCK_START, LatteConst::ShaderType::Geometry, gsUniformBufferDirtyMask) )
			stageUniformModifiedMask |= (1<<VulkanRendererConst::SHADER_STAGE_INDEX_GEOMETRY);
	}
}
