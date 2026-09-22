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

	bool SubmitPresent(const PresentArguments& present)
	{
		// Re-entering through our own swap would publish a frame boundary the
		// guest never reached. Refusing a nested present is not a limitation
		// to work around; there is no such thing as a present inside a present.
		if (s_inRuntimePresent)
		{
			return false;
		}
		s_inRuntimePresent = true;
		// The command processor reads its stream as big-endian words, so the
		// packet is assembled the same way the guest assembles it rather than
		// in host order.
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
		bool submitted =
			SubmitDisplayList(packet.data(), static_cast<uint32_t>(packet.size() * sizeof(uint32be)));
		s_inRuntimePresent = false;
		return submitted;
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
