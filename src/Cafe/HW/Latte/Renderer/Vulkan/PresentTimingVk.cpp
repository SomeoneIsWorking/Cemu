#include "Cafe/HW/Latte/Renderer/Vulkan/PresentTimingVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"

using LatteFrameHooks::ShownStage;

std::optional<ShownStage> PresentTimingVk::Query(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, bool deviceSupports)
{
	if (!deviceSupports || vkGetPhysicalDeviceSurfaceCapabilities2KHR == nullptr)
		return std::nullopt;
	VkSurfaceCapabilitiesPresentWait2KHR wait2{};
	wait2.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_WAIT_2_KHR;
	VkSurfaceCapabilitiesPresentId2KHR id2{};
	id2.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR;
	id2.pNext = &wait2;
	VkPresentTimingSurfaceCapabilitiesEXT timing{};
	timing.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT;
	timing.pNext = &id2;
	VkSurfaceCapabilities2KHR capabilities{};
	capabilities.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
	capabilities.pNext = &timing;
	VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo{};
	surfaceInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
	surfaceInfo.surface = surface;
	if (vkGetPhysicalDeviceSurfaceCapabilities2KHR(physicalDevice, &surfaceInfo, &capabilities) != VK_SUCCESS)
		return std::nullopt;
	if (!timing.presentTimingSupported || !id2.presentId2Supported || !wait2.presentWait2Supported)
		return std::nullopt;
	// The latest stage the surface reports: the nearest to what is seen.
	for (ShownStage stage : {ShownStage::FirstPixelVisible, ShownStage::FirstPixelOut, ShownStage::Dequeued})
	{
		if ((timing.presentStageQueries & StageBit(stage)) != 0)
			return stage;
	}
	return std::nullopt;
}

VkSwapchainCreateFlagsKHR PresentTimingVk::CreateFlags()
{
	return VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT | VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR | VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR;
}

VkPresentStageFlagsEXT PresentTimingVk::StageBit(ShownStage stage)
{
	switch (stage)
	{
	case ShownStage::QueueDone:
		return VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT;
	case ShownStage::Dequeued:
		return VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT;
	case ShownStage::FirstPixelOut:
		return VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
	case ShownStage::FirstPixelVisible:
		return VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT;
	}
	return 0;
}

void PresentTimingVk::Start(VkDevice device, VkSwapchainKHR swapchain, ShownStage stage)
{
	if (vkSetSwapchainPresentTimingQueueSizeEXT(device, swapchain, kTimingQueueSize) != VK_SUCCESS)
	{
		cemuLog_log(LogType::Force, "Vulkan: present timing queue could not be sized; shown times are not reported");
		return;
	}
	m_device = device;
	m_swapchain = swapchain;
	m_stage = stage;
	m_pending.clear();
}

void PresentTimingVk::Stop()
{
	m_device = VK_NULL_HANDLE;
	m_swapchain = VK_NULL_HANDLE;
	m_pending.clear();
}

void PresentTimingVk::Chain(VkPresentInfoKHR& presentInfo, uint64_t presentId)
{
	m_chainedId = presentId;
	m_presentId = {};
	m_presentId.sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR;
	m_presentId.swapchainCount = 1;
	m_presentId.pPresentIds = &m_chainedId;
	m_presentId.pNext = presentInfo.pNext;
	m_timingInfo = {};
	m_timingInfo.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT;
	m_timingInfo.presentStageQueries = StageBit(m_stage);
	m_timingsInfo = {};
	m_timingsInfo.sType = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT;
	m_timingsInfo.swapchainCount = 1;
	m_timingsInfo.pTimingInfos = &m_timingInfo;
	m_timingsInfo.pNext = &m_presentId;
	presentInfo.pNext = &m_timingsInfo;
}

void PresentTimingVk::Presented(uint64_t presentId, bool fromRuntime)
{
	m_pending.push_back({presentId, fromRuntime});
}

void PresentTimingVk::WaitForPresent(uint64_t presentId, uint64_t timeout) const
{
	VkPresentWait2InfoKHR info{};
	info.sType = VK_STRUCTURE_TYPE_PRESENT_WAIT_2_INFO_KHR;
	info.presentId = presentId;
	info.timeout = timeout;
	vkWaitForPresent2KHR(m_device, m_swapchain, &info);
}

void PresentTimingVk::Collect()
{
	if (!IsActive())
		return;
	LatteFrameHooks::Observer* observer = LatteFrameHooks::GetObserver();
	VkResult result = VK_INCOMPLETE;
	while (result == VK_INCOMPLETE && !m_pending.empty())
	{
		for (size_t index = 0; index < kReadBatch; ++index)
		{
			m_timings[index] = {};
			m_timings[index].sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT;
			m_timings[index].presentStageCount = 1;
			m_timings[index].pPresentStages = &m_stageTimes[index];
		}
		VkPastPresentationTimingInfoEXT info{};
		info.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT;
		info.swapchain = m_swapchain;
		VkPastPresentationTimingPropertiesEXT properties{};
		properties.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT;
		properties.presentationTimingCount = kReadBatch;
		properties.pPresentationTimings = m_timings.data();
		result = vkGetPastPresentationTimingEXT(m_device, &info, &properties);
		if (result != VK_SUCCESS && result != VK_INCOMPLETE)
		{
			cemuLog_log(LogType::Force, "Vulkan: reading present times failed ({}); shown times are no longer reported", result);
			Stop();
			return;
		}
		for (uint32_t index = 0; index < properties.presentationTimingCount; ++index)
		{
			const VkPastPresentationTimingEXT& timing = m_timings[index];
			// Presents are reported in order; one the engine skipped is
			// dropped with those before it.
			while (!m_pending.empty() && m_pending.front().presentId < timing.presentId)
				m_pending.pop_front();
			if (m_pending.empty() || m_pending.front().presentId != timing.presentId)
				continue;
			bool fromRuntime = m_pending.front().fromRuntime;
			m_pending.pop_front();
			// A stage time of zero is an image that never reached the stage:
			// it was not shown, and the next shown frame's interval covers it.
			if (!timing.reportComplete || timing.presentStageCount == 0 || m_stageTimes[index].time == 0 || observer == nullptr)
				continue;
			observer->OnShown({fromRuntime, m_stage, m_stageTimes[index].time, timing.timeDomainId});
		}
	}
}
