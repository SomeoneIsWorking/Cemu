#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/LatteTextureVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VKRMemoryManager.h"

// One subresource's copy: an image of that subresource's size and format and
// nothing else, used only as a transfer source and destination.
class LatteTextureShadowVk : public LatteTextureShadow
{
public:
	explicit LatteTextureShadowVk(VKRObjectTexture* image) : m_image(image) {}
	~LatteTextureShadowVk() override
	{
		// Released rather than destroyed: a copy into or out of it may still
		// be in a command buffer the GPU has not finished.
		VulkanRenderer::GetInstance()->ReleaseDestructibleObject(m_image);
	}
	LatteTextureShadowVk(const LatteTextureShadowVk&) = delete;
	LatteTextureShadowVk& operator=(const LatteTextureShadowVk&) = delete;

	VKRObjectTexture* m_image;
	VkImageLayout m_layout{VK_IMAGE_LAYOUT_UNDEFINED};
	uint32 m_width;
	uint32 m_height;
};

std::unique_ptr<LatteTextureShadow> VulkanRenderer::texture_createShadow(LatteTexture* texture, sint32 sliceIndex, sint32 mipIndex)
{
	auto* textureVk = static_cast<LatteTextureVk*>(texture);
	VKRObjectTexture* source = textureVk->GetImageObj();
	sint32 width, height;
	texture->GetEffectiveSize(width, height, mipIndex);

	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	if (texture->dim == Latte::E_DIM::DIM_1D)
		imageInfo.imageType = VK_IMAGE_TYPE_1D;
	else if (texture->Is3DTexture())
		imageInfo.imageType = VK_IMAGE_TYPE_3D;
	else
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = source->m_format;
	imageInfo.extent.width = width;
	imageInfo.extent.height = height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	auto* image = new VKRObjectTexture();
	if (vkCreateImage(m_logicalDevice, &imageInfo, nullptr, &image->m_image) != VK_SUCCESS)
		UnrecoverableError("Failed to create texture shadow image");
	image->m_format = imageInfo.format;
	image->m_flags = imageInfo.flags;
	image->m_imageAspect = source->m_imageAspect;
	image->m_allocation = memoryManager->imageMemoryAllocate(image->m_image);

	auto shadow = std::make_unique<LatteTextureShadowVk>(image);
	shadow->m_width = width;
	shadow->m_height = height;
	return shadow;
}

void VulkanRenderer::texture_copyToShadow(LatteTexture* texture, sint32 sliceIndex, sint32 mipIndex, LatteTextureShadow& shadow)
{
	texture_copyShadow(static_cast<LatteTextureVk*>(texture), sliceIndex, mipIndex, static_cast<LatteTextureShadowVk&>(shadow), true);
}

void VulkanRenderer::texture_copyFromShadow(LatteTexture* texture, sint32 sliceIndex, sint32 mipIndex, LatteTextureShadow& shadow)
{
	texture_copyShadow(static_cast<LatteTextureVk*>(texture), sliceIndex, mipIndex, static_cast<LatteTextureShadowVk&>(shadow), false);
}

void VulkanRenderer::texture_copyShadow(LatteTextureVk* texture, sint32 sliceIndex, sint32 mipIndex, LatteTextureShadowVk& shadow, bool toShadow)
{
	draw_endRenderPass(); // vkCmdCopyImage must be called outside of a renderpass

	VKRObjectTexture* textureObj = texture->GetImageObj();
	textureObj->flagForCurrentCommandBuffer();
	shadow.m_image->flagForCurrentCommandBuffer();

	const bool is3D = texture->Is3DTexture();
	VkImageSubresourceLayers textureLayers{};
	textureLayers.aspectMask = texture->GetImageAspect();
	textureLayers.mipLevel = mipIndex;
	textureLayers.baseArrayLayer = is3D ? 0 : sliceIndex;
	textureLayers.layerCount = 1;
	VkOffset3D textureOffset{0, 0, is3D ? sliceIndex : 0};

	VkImageSubresourceLayers shadowLayers{};
	shadowLayers.aspectMask = textureLayers.aspectMask;
	shadowLayers.mipLevel = 0;
	shadowLayers.baseArrayLayer = 0;
	shadowLayers.layerCount = 1;
	VkImageSubresourceRange shadowRange{};
	shadowRange.aspectMask = shadowLayers.aspectMask;
	shadowRange.baseMipLevel = 0;
	shadowRange.levelCount = 1;
	shadowRange.baseArrayLayer = 0;
	shadowRange.layerCount = 1;

	// every earlier use of both images has finished before the copy
	barrier_image<SYNC_OP::IMAGE_READ | SYNC_OP::IMAGE_WRITE | SYNC_OP::ANY_TRANSFER, SYNC_OP::ANY_TRANSFER>(texture, textureLayers, VK_IMAGE_LAYOUT_GENERAL);
	barrier_image<SYNC_OP::ANY_TRANSFER, SYNC_OP::ANY_TRANSFER>(shadow.m_image->m_image, shadowRange, shadow.m_layout, VK_IMAGE_LAYOUT_GENERAL);
	shadow.m_layout = VK_IMAGE_LAYOUT_GENERAL;

	VkImageCopy region{};
	region.extent.width = shadow.m_width;
	region.extent.height = shadow.m_height;
	region.extent.depth = 1;
	if (toShadow)
	{
		region.srcSubresource = textureLayers;
		region.srcOffset = textureOffset;
		region.dstSubresource = shadowLayers;
		vkCmdCopyImage(m_state.currentCommandBuffer, textureObj->m_image, VK_IMAGE_LAYOUT_GENERAL, shadow.m_image->m_image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
	}
	else
	{
		region.srcSubresource = shadowLayers;
		region.dstSubresource = textureLayers;
		region.dstOffset = textureOffset;
		vkCmdCopyImage(m_state.currentCommandBuffer, shadow.m_image->m_image, VK_IMAGE_LAYOUT_GENERAL, textureObj->m_image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
	}

	// the copy has finished before the texture is read or written again
	barrier_image<SYNC_OP::ANY_TRANSFER, SYNC_OP::IMAGE_READ | SYNC_OP::IMAGE_WRITE | SYNC_OP::ANY_TRANSFER>(texture, textureLayers, texture->GetDefaultLayout());
}
