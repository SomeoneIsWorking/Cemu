#pragma once

#include "Cafe/HW/Latte/Core/LatteFrameHooks.h"
#include <vulkan/vulkan_core.h>
#include <array>
#include <deque>
#include <optional>

// When each present reached the screen, as the presentation engine reports it
// (VK_EXT_present_timing), handed to the frame observer's OnShown. A present
// returns once its image is queued, so the times the renderer sees are when
// frames were handed over, not when they were shown; with a FIFO queue two
// frames handed over a moment apart can still be shown a refresh apart, or
// not, and only the presentation engine knows which.
//
// A swapchain uses it when its surface reports a stage on the way to the
// screen and supports present ids and waits of the second kind; its presents
// then carry VkPresentId2KHR, and are waited for with vkWaitForPresent2KHR.
class PresentTimingVk
{
  public:
	// Presents whose times may be outstanding at once: more than the
	// swapchain's images and the presents queued ahead of them.
	static constexpr uint32_t kTimingQueueSize = 64;
	// Times read back per call to the presentation engine.
	static constexpr uint32_t kReadBatch = 16;

	// What a surface reports, asked before its swapchain is made. None when
	// the device or the surface reports nothing past the GPU's own work.
	static std::optional<LatteFrameHooks::ShownStage> Query(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, bool deviceSupports);
	// The swapchain flags a swapchain timed at `stage` is created with.
	static VkSwapchainCreateFlagsKHR CreateFlags();

	// Starts timing a swapchain created with CreateFlags; `stage` from Query.
	void Start(VkDevice device, VkSwapchainKHR swapchain, LatteFrameHooks::ShownStage stage);
	// Forgets the swapchain, and every present not yet reported.
	void Stop();
	bool IsActive() const
	{
		return m_swapchain != VK_NULL_HANDLE;
	}

	// Chains the present's id and its timing request onto `presentInfo`.
	// The structures chained are this object's, and are read by the present
	// before it returns.
	void Chain(VkPresentInfoKHR& presentInfo, uint64_t presentId);
	// The present went to the queue: its time will be reported.
	void Presented(uint64_t presentId, bool fromRuntime);
	// Waits for a present chained by Chain to be shown, up to `timeout` ns.
	void WaitForPresent(uint64_t presentId, uint64_t timeout) const;
	// Hands every present whose time has been reported to the observer, in
	// order.
	void Collect();

  private:
	struct Pending
	{
		uint64_t presentId;
		bool fromRuntime;
	};

	static VkPresentStageFlagsEXT StageBit(LatteFrameHooks::ShownStage stage);

	VkDevice m_device = VK_NULL_HANDLE;
	VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
	LatteFrameHooks::ShownStage m_stage = LatteFrameHooks::ShownStage::QueueDone;
	std::deque<Pending> m_pending;

	uint64_t m_chainedId = 0;
	VkPresentId2KHR m_presentId{};
	VkPresentTimingInfoEXT m_timingInfo{};
	VkPresentTimingsInfoEXT m_timingsInfo{};
	std::array<VkPresentStageTimeEXT, kReadBatch> m_stageTimes{};
	std::array<VkPastPresentationTimingEXT, kReadBatch> m_timings{};
};
