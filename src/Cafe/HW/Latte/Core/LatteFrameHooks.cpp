#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"

#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"

#include <array>
#include <vector>

namespace LatteFrameHooks
{
	namespace
	{
		Observer* s_observer = nullptr;
	}

	void SetObserver(Observer* observer)
	{
		s_observer = observer;
	}

	Observer* GetObserver()
	{
		return s_observer;
	}

	namespace
	{
		// Owned by the Latte thread, which is the only thread that submits.
		bool s_inRuntimePresent = false;
	} // namespace

	bool InRuntimePresent()
	{
		return s_inRuntimePresent;
	}

	namespace
	{
		// Owned by the Latte thread, which is the only thread that submits.
		int s_runtimeSubmissionDepth = 0;
		SubmissionSummary s_summary{};
	} // namespace

	bool InRuntimeSubmission()
	{
		return s_runtimeSubmissionDepth > 0;
	}

	RuntimeSubmission::RuntimeSubmission()
	{
		if (s_runtimeSubmissionDepth == 0)
		{
			s_summary = SubmissionSummary{};
		}
		++s_runtimeSubmissionDepth;
	}

	RuntimeSubmission::~RuntimeSubmission()
	{
		--s_runtimeSubmissionDepth;
		// Only the outermost one reports: a nested buffer is part of the same
		// submission and counting it twice would inflate what reached the
		// renderer.
		if (s_runtimeSubmissionDepth == 0 && s_observer != nullptr)
		{
			s_observer->OnRuntimeSubmission(s_summary);
		}
	}

	namespace
	{
		// Owned by the Latte thread, which is the only thread that walks them.
		int s_commandBufferDepth = 0;
	} // namespace

	bool InCommandBuffer()
	{
		return s_commandBufferDepth > 0;
	}

	CommandBufferWalk::CommandBufferWalk()
	{
		++s_commandBufferDepth;
	}

	CommandBufferWalk::~CommandBufferWalk()
	{
		--s_commandBufferDepth;
	}

	void NoteRuntimePacket()
	{
		if (s_runtimeSubmissionDepth > 0)
		{
			++s_summary.packetsProcessed;
		}
	}

	void NoteRuntimeDraw()
	{
		if (s_runtimeSubmissionDepth > 0)
		{
			++s_summary.drawsIssued;
		}
	}

	namespace
	{
		// Which class a packet's guest-visible effect belongs to, or Count
		// when it has none and a replay may execute it.
		WithheldEffect ClassifyGuestVisibleEffect(uint32_t itCode)
		{
			switch (itCode)
			{
			case IT_HLE_COPY_COLORBUFFER_TO_SCANBUFFER:
			case IT_HLE_TRIGGER_SCANBUFFER_SWAP:
			case IT_HLE_REQUEST_SWAP_BUFFERS:
			case IT_HLE_WAIT_FOR_FLIP:
				return WithheldEffect::Presentation;
			case IT_WAIT_REG_MEM:
			case IT_MEM_SEMAPHORE:
				return WithheldEffect::Synchronisation;
			case IT_MEM_WRITE:
			case IT_EVENT_WRITE_EOP:
			case IT_HLE_SAMPLE_TIMER:
			case IT_HLE_BOTTOM_OF_PIPE_CB:
			case IT_STRMOUT_BUFFER_UPDATE:
				return WithheldEffect::GuestMemoryWrite;
			case IT_HLE_BEGIN_OCCLUSION_QUERY:
			case IT_HLE_END_OCCLUSION_QUERY:
				return WithheldEffect::OcclusionQuery;
			default:
				return WithheldEffect::Count;
			}
		}

		bool Withhold(WithheldEffect effect)
		{
			if (s_runtimeSubmissionDepth == 0)
			{
				return false;
			}
			++s_summary.withheld[static_cast<uint32_t>(effect)];
			return true;
		}
	} // namespace

	bool WithholdFromRuntimeSubmission(uint32_t itCode)
	{
		WithheldEffect effect = ClassifyGuestVisibleEffect(itCode);
		if (effect == WithheldEffect::Count)
		{
			return false;
		}
		// The runtime's own present is the one presentation it is allowed:
		// SubmitPresent assembles exactly those two packets.
		if (effect == WithheldEffect::Presentation && s_inRuntimePresent)
		{
			return false;
		}
		return Withhold(effect);
	}

	bool WithholdReadbackFromRuntimeSubmission()
	{
		return Withhold(WithheldEffect::TextureReadback);
	}

	namespace
	{
		// Both presents the runtime makes share this: the guest's own packet
		// encoding, fed through the same command processor, marked as the
		// runtime's so the swap it may contain publishes no frame end.
		bool SubmitRuntimePresentPackets(const PresentArguments& present, bool swap)
		{
			// Re-entering through our own swap would publish a frame boundary
			// the guest never reached. Refusing a nested present is not a
			// limitation to work around; there is no such thing as a present
			// inside a present.
			if (s_inRuntimePresent)
			{
				return false;
			}
			s_inRuntimePresent = true;
			// The command processor reads its stream as big-endian words, so
			// the packet is assembled the same way the guest assembles it
			// rather than in host order.
			std::array<uint32be, 12> packet{};
			packet[0] = pm4HeaderType3(IT_HLE_COPY_COLORBUFFER_TO_SCANBUFFER, 9);
			packet[1] = present.physicalAddress;
			packet[2] = present.width;
			packet[3] = present.height;
			packet[4] = present.pitch;
			packet[5] = present.tileMode;
			packet[6] = present.swizzle;
			packet[7] = present.sliceIndex;
			packet[8] = present.format;
			packet[9] = present.renderTarget;
			packet[10] = pm4HeaderType3(IT_HLE_TRIGGER_SCANBUFFER_SWAP, 1);
			packet[11] = 0; // reserved
			size_t words = swap ? packet.size() : 10;
			bool submitted =
				SubmitDisplayList(packet.data(), static_cast<uint32_t>(words * sizeof(uint32be)));
			s_inRuntimePresent = false;
			return submitted;
		}
	} // namespace

	bool SubmitPresent(const PresentArguments& present)
	{
		return SubmitRuntimePresentPackets(present, true);
	}

	bool SubmitScanBufferCopy(const PresentArguments& present)
	{
		return SubmitRuntimePresentPackets(present, false);
	}

	bool RequestFrameCapture(CaptureCallback callback)
	{
		// Before a renderer exists there is nothing to present and nothing to
		// capture. Refusing here is the difference between "no renderer yet"
		// and "armed, and the image never came".
		if (!callback || g_renderer == nullptr)
		{
			return false;
		}
		// Arming over an outstanding request would replace its callback, and
		// the first capture would simply never arrive. Refusing says so.
		if (g_renderer->IsScreenshotRequested())
		{
			return false;
		}
		// The renderer already captures the presented scan buffer, before any
		// overlay, and hands back raw RGB. Reusing it means a captured frame
		// is the frame a user would see rather than a second path that has to
		// be kept honest separately. The optional return is its notification
		// text; an automated capture wants none.
		g_renderer->RequestScreenshot(
			[callback = std::move(callback)](const std::vector<uint8>& rgb, int width, int height,
											 bool mainWindow) -> std::optional<std::string> {
				FrameImage image{rgb.data(), static_cast<uint32_t>(rgb.size()), width, height,
								 mainWindow};
				callback(image);
				return std::nullopt;
			});
		return true;
	}

} // namespace LatteFrameHooks
