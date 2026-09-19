#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"

#include "Cafe/HW/Latte/Renderer/Renderer.h"

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

	bool RequestFrameCapture(CaptureCallback callback)
	{
		// Before a renderer exists there is nothing to present and nothing to
		// capture. Refusing here is the difference between "no renderer yet"
		// and "armed, and the image never came".
		if (!callback || g_renderer == nullptr)
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
