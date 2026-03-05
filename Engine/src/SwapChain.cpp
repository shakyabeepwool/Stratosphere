#include "Engine/SwapChain.h"
#include <GLFW/glfw3.h> // only if you need glfw helpers elsewhere; not required here
#include <stdexcept>
#include <iostream>
#include <algorithm>

namespace Engine
{
    SwapChain::SwapChain(VkDevice device,
                         VkPhysicalDevice physicalDevice,
                         VkSurfaceKHR surface,
                         const Engine::QueueFamilyIndices &indices,
                         VkExtent2D initialExtent)
        : m_Device(device), m_PhysicalDevice(physicalDevice), m_Surface(surface), m_QueueIndices(indices), m_InitialExtent(initialExtent), m_Extent(initialExtent)
    {
    }

    SwapChain::~SwapChain()
    {
        Cleanup();
    }

    void SwapChain::Init()
    {
        // Query support details
        VkSurfaceCapabilitiesKHR capabilities{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_PhysicalDevice, m_Surface, &capabilities);

        uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_PhysicalDevice, m_Surface, &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        if (formatCount)
            vkGetPhysicalDeviceSurfaceFormatsKHR(m_PhysicalDevice, m_Surface, &formatCount, formats.data());

        uint32_t presentCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_PhysicalDevice, m_Surface, &presentCount, nullptr);
        std::vector<VkPresentModeKHR> presentModes(presentCount);
        if (presentCount)
            vkGetPhysicalDeviceSurfacePresentModesKHR(m_PhysicalDevice, m_Surface, &presentCount, presentModes.data());

        VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(formats);
        VkPresentModeKHR presentMode = chooseSwapPresentMode(presentModes);
        VkExtent2D extent = chooseSwapExtent(capabilities);

        uint32_t imageCount = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount)
        {
            imageCount = capabilities.maxImageCount;
        }

        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = m_Surface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        uint32_t queueFamilyIndicesArr[2];
        if (m_QueueIndices.graphicsFamily.value() != m_QueueIndices.presentFamily.value())
        {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            queueFamilyIndicesArr[0] = m_QueueIndices.graphicsFamily.value();
            queueFamilyIndicesArr[1] = m_QueueIndices.presentFamily.value();
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = queueFamilyIndicesArr;
        }
        else
        {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            createInfo.queueFamilyIndexCount = 0;
            createInfo.pQueueFamilyIndices = nullptr;
        }

        createInfo.preTransform = capabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        createInfo.oldSwapchain = m_Swapchain; // allow recreation

        if (vkCreateSwapchainKHR(m_Device, &createInfo, nullptr, &m_Swapchain) != VK_SUCCESS)
        {
            throw std::runtime_error("SwapChain::Init - failed to create swapchain");
        }

        // retrieve images
        uint32_t actualCount = 0;
        vkGetSwapchainImagesKHR(m_Device, m_Swapchain, &actualCount, nullptr);
        m_Images.resize(actualCount);
        vkGetSwapchainImagesKHR(m_Device, m_Swapchain, &actualCount, m_Images.data());

        m_ImageFormat = surfaceFormat.format;
        m_Extent = extent;

        // create image views for use in framebuffers
        createImageViews();

        // If replacing an old swapchain, cleanup the old one (vkDestroySwapchainKHR must be handled by caller or here if oldSwapchain used)
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        std::cout << "SwapChain initialized: images=" << m_Images.size() << " format=" << m_ImageFormat << "\n";
#endif
    }

    void SwapChain::Cleanup()
    {
        // destroy image views
        for (auto iv : m_ImageViews)
        {
            if (iv != VK_NULL_HANDLE)
                vkDestroyImageView(m_Device, iv, nullptr);
        }
        m_ImageViews.clear();

        // destroy swapchain
        if (m_Swapchain != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(m_Device, m_Swapchain, nullptr);
            m_Swapchain = VK_NULL_HANDLE;
        }
    }

    void SwapChain::Recreate(VkExtent2D newExtent)
    {
        // caller should ensure device idle or use fences; here we simply cleanup and init
        Cleanup();
        m_Extent = newExtent;
        Init();
    }

    // helpers
    VkSurfaceFormatKHR SwapChain::chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR> &available) const
    {
        for (const auto &av : available)
        {
            if (av.format == VK_FORMAT_B8G8R8A8_SRGB && av.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                return av;
        }
        return available.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : available[0];
    }

    VkPresentModeKHR SwapChain::chooseSwapPresentMode(const std::vector<VkPresentModeKHR> &available) const
    {
        for (const auto &av : available)
        {
            if (av == VK_PRESENT_MODE_MAILBOX_KHR)
                return av;
        }
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D SwapChain::chooseSwapExtent(const VkSurfaceCapabilitiesKHR &capabilities) const
    {
        if (capabilities.currentExtent.width != UINT32_MAX)
        {
            return capabilities.currentExtent;
        }
        else
        {
            VkExtent2D actual = m_InitialExtent;
            actual.width = std::max(capabilities.minImageExtent.width, std::min(capabilities.maxImageExtent.width, actual.width));
            actual.height = std::max(capabilities.minImageExtent.height, std::min(capabilities.maxImageExtent.height, actual.height));
            return actual;
        }
    }

    void SwapChain::createImageViews()
    {
        m_ImageViews.resize(m_Images.size());
        for (size_t i = 0; i < m_Images.size(); ++i)
        {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = m_Images[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = m_ImageFormat;
            viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
            viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
            viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
            viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;

            if (vkCreateImageView(m_Device, &viewInfo, nullptr, &m_ImageViews[i]) != VK_SUCCESS)
            {
                throw std::runtime_error("SwapChain::createImageViews - failed to create image view");
            }
        }
    }

} // namespace Engine