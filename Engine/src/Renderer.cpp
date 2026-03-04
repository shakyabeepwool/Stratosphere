#include <stdexcept>
#include <iostream>
#include <cstring>
#include "Engine/Renderer.h"
#include "Engine/VulkanContext.h"
#include "Engine/SwapChain.h"
#include "utils/ImageUtils.h"

namespace Engine
{
    static VkFormat findSupportedFormat(
        VkPhysicalDevice phys,
        const std::vector<VkFormat> &candidates,
        VkImageTiling tiling,
        VkFormatFeatureFlags features)
    {
        for (VkFormat format : candidates)
        {
            VkFormatProperties props{};
            vkGetPhysicalDeviceFormatProperties(phys, format, &props);

            if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features)
                return format;
            if (tiling == VK_IMAGE_TILING_OPTIMAL && (props.optimalTilingFeatures & features) == features)
                return format;
        }
        return VK_FORMAT_UNDEFINED;
    }

    static VkFormat findDepthFormat(VkPhysicalDevice phys)
    {
        // Prefer D32; fall back to common packed depth/stencil formats.
        return findSupportedFormat(
            phys,
            {
                VK_FORMAT_D32_SFLOAT,
                VK_FORMAT_D32_SFLOAT_S8_UINT,
                VK_FORMAT_D24_UNORM_S8_UINT,
            },
            VK_IMAGE_TILING_OPTIMAL,
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
    }

    static VkImageAspectFlags depthAspectFlags(VkFormat fmt)
    {
        VkImageAspectFlags flags = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (fmt == VK_FORMAT_D32_SFLOAT_S8_UINT || fmt == VK_FORMAT_D24_UNORM_S8_UINT)
            flags |= VK_IMAGE_ASPECT_STENCIL_BIT;
        return flags;
    }

    Renderer::Renderer(VulkanContext *ctx, SwapChain *swapchain, uint32_t maxFramesInFlight)
        : m_ctx(ctx), m_swapchain(swapchain), m_maxFrames(maxFramesInFlight)
    {
        if (!m_ctx || !m_swapchain)
        {
            throw std::runtime_error("Renderer: VulkanContext and Swapchain must be non-null");
        }

        // Extract commonly used handles (adjust if your context uses getters)
        m_device = m_ctx->GetDevice();
        m_graphicsQueue = m_ctx->GetGraphicsQueue();
        m_presentQueue = m_ctx->GetPresentQueue();

        m_swapchainImageFormat = m_swapchain->GetImageFormat();
        m_extent = m_swapchain->GetExtent();
    }

    Renderer::~Renderer()
    {
        // Ensure cleanup was called
        if (m_initialized)
        {
            try
            {
                cleanup();
            }
            catch (...)
            {
                // Avoid throwing from destructor
            }
        }
    }

    void Renderer::init(VkExtent2D extent)
    {
        if (m_initialized)
            return;

        // If a non-zero extent is provided, override the current extent
        if (extent.width > 0 && extent.height > 0)
        {
            m_extent = extent;
        }

        // prepare per-frame slots
        m_frames.resize(m_maxFrames);

        // swapchain-dependent
        m_swapchainImageFormat = m_swapchain->GetImageFormat();
        m_extent = m_swapchain->GetExtent();

        createDepthResources();
        createMainRenderPass();
        createFramebuffers();
        createSyncObjects();
        createCommandPoolsAndBuffers();
        createTimestampQueryPool();

        // notify registered passes so they can create pipelines/resources that depend on renderpass/framebuffers
        for (auto &p : m_passes)
        {
            if (p)
                p->onCreate(*m_ctx, m_mainRenderPass, m_framebuffers);
        }

        m_initialized = true;
    }

    void Renderer::init() // Overloaded init function to be used without passing extent
    {
        if (m_initialized)
            return;

        // prepare per-frame slots
        m_frames.resize(m_maxFrames);

        // swapchain-dependent
        m_swapchainImageFormat = m_swapchain->GetImageFormat();
        m_extent = m_swapchain->GetExtent();

        createDepthResources();
        createMainRenderPass();
        createFramebuffers();
        createSyncObjects();
        createCommandPoolsAndBuffers();
        createTimestampQueryPool();

        // notify registered passes so they can create pipelines/resources that depend on renderpass/framebuffers
        for (auto &p : m_passes)
        {
            if (p)
                p->onCreate(*m_ctx, m_mainRenderPass, m_framebuffers);
        }

        m_initialized = true;
    }

    void Renderer::cleanup()
    {
        if (!m_initialized)
            return;

        // Wait for GPU to finish using resources before destroying them
        vkDeviceWaitIdle(m_device);

        // Notify passes to destroy their device-owned resources (pipelines, descriptors, etc.)
        for (auto &p : m_passes)
        {
            if (p)
                p->onDestroy(*m_ctx);
        }

        destroyTimestampQueryPool();
        destroyCommandPoolsAndBuffers();
        destroySyncObjects();

        // Destroy framebuffers
        for (auto fb : m_framebuffers)
        {
            if (fb != VK_NULL_HANDLE)
            {
                vkDestroyFramebuffer(m_device, fb, nullptr);
            }
        }
        m_framebuffers.clear();

        destroyDepthResources();

        // Destroy main render pass
        if (m_mainRenderPass != VK_NULL_HANDLE)
        {
            vkDestroyRenderPass(m_device, m_mainRenderPass, nullptr);
            m_mainRenderPass = VK_NULL_HANDLE;
        }

        m_initialized = false;
    }

    void Renderer::registerPass(std::shared_ptr<RenderPassModule> pass)
    {
        if (!pass)
            return;
        m_passes.push_back(pass);
        if (m_initialized)
        {
            // Immediately call onCreate so the pass can create pipelines that depend on renderpass/framebuffers.
            pass->onCreate(*m_ctx, m_mainRenderPass, m_framebuffers);
        }
    }

    void Renderer::createMainRenderPass()
    {
        if (m_depthFormat == VK_FORMAT_UNDEFINED)
        {
            m_depthFormat = findDepthFormat(m_ctx->GetPhysicalDevice());
        }

        // Color attachment tied to swapchain image format
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = m_swapchainImageFormat;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        // Depth attachment (one image per swapchain image)
        VkAttachmentDescription depthAttachment{};
        depthAttachment.format = m_depthFormat;
        depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorAttachmentRef{};
        colorAttachmentRef.attachment = 0;
        colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference depthAttachmentRef{};
        depthAttachmentRef.attachment = 1;
        depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorAttachmentRef;
        subpass.pDepthStencilAttachment = &depthAttachmentRef;

        // Subpass dependency from external -> subpass 0
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpInfo{};
        rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        VkAttachmentDescription attachments[2] = {colorAttachment, depthAttachment};
        rpInfo.attachmentCount = 2;
        rpInfo.pAttachments = attachments;
        rpInfo.subpassCount = 1;
        rpInfo.pSubpasses = &subpass;
        rpInfo.dependencyCount = 1;
        rpInfo.pDependencies = &dependency;

        if (vkCreateRenderPass(m_device, &rpInfo, nullptr, &m_mainRenderPass) != VK_SUCCESS)
        {
            throw std::runtime_error("Renderer::createMainRenderPass - failed to create render pass");
        }
    }

    void Renderer::createFramebuffers()
    {
        const auto &imageViews = m_swapchain->GetImageViews();
        m_framebuffers.resize(imageViews.size());

        if (m_depthImageViews.size() != imageViews.size())
        {
            throw std::runtime_error("Renderer::createFramebuffers - depth resources not initialized or size mismatch");
        }

        for (size_t i = 0; i < imageViews.size(); ++i)
        {
            VkImageView attachments[2] = {imageViews[i], m_depthImageViews[i]};

            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass = m_mainRenderPass;
            fbInfo.attachmentCount = 2;
            fbInfo.pAttachments = attachments;
            fbInfo.width = m_extent.width;
            fbInfo.height = m_extent.height;
            fbInfo.layers = 1;

            if (vkCreateFramebuffer(m_device, &fbInfo, nullptr, &m_framebuffers[i]) != VK_SUCCESS)
            {
                throw std::runtime_error("Renderer::createFramebuffers - failed to create framebuffer");
            }
        }
    }

    void Renderer::createDepthResources()
    {
        destroyDepthResources();

        m_depthFormat = findDepthFormat(m_ctx->GetPhysicalDevice());
        if (m_depthFormat == VK_FORMAT_UNDEFINED)
        {
            throw std::runtime_error("Renderer::createDepthResources - failed to find supported depth format");
        }

        const auto &imageViews = m_swapchain->GetImageViews();
        m_depthImages.resize(imageViews.size(), VK_NULL_HANDLE);
        m_depthMemories.resize(imageViews.size(), VK_NULL_HANDLE);
        m_depthImageViews.resize(imageViews.size(), VK_NULL_HANDLE);

        for (size_t i = 0; i < imageViews.size(); ++i)
        {
            VkResult r = CreateImage2D(
                m_device,
                m_ctx->GetPhysicalDevice(),
                m_extent.width,
                m_extent.height,
                m_depthFormat,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                m_depthImages[i],
                m_depthMemories[i]);
            if (r != VK_SUCCESS)
            {
                throw std::runtime_error("Renderer::createDepthResources - failed to create depth image");
            }

            r = CreateImageView2D(
                m_device,
                m_depthImages[i],
                m_depthFormat,
                depthAspectFlags(m_depthFormat),
                m_depthImageViews[i]);
            if (r != VK_SUCCESS)
            {
                throw std::runtime_error("Renderer::createDepthResources - failed to create depth image view");
            }
        }
    }

    void Renderer::destroyDepthResources()
    {
        for (auto &iv : m_depthImageViews)
        {
            if (iv != VK_NULL_HANDLE)
            {
                vkDestroyImageView(m_device, iv, nullptr);
                iv = VK_NULL_HANDLE;
            }
        }
        for (auto &img : m_depthImages)
        {
            if (img != VK_NULL_HANDLE)
            {
                vkDestroyImage(m_device, img, nullptr);
                img = VK_NULL_HANDLE;
            }
        }
        for (auto &mem : m_depthMemories)
        {
            if (mem != VK_NULL_HANDLE)
            {
                vkFreeMemory(m_device, mem, nullptr);
                mem = VK_NULL_HANDLE;
            }
        }
        m_depthImageViews.clear();
        m_depthImages.clear();
        m_depthMemories.clear();
    }

    void Renderer::recreateSwapchainDependent()
    {
        vkDeviceWaitIdle(m_device);

        // Destroy pass-owned resources that depend on renderpass/framebuffers
        for (auto &p : m_passes)
        {
            if (p)
                p->onDestroy(*m_ctx);
        }

        for (auto fb : m_framebuffers)
        {
            if (fb != VK_NULL_HANDLE)
                vkDestroyFramebuffer(m_device, fb, nullptr);
        }
        m_framebuffers.clear();

        destroyDepthResources();

        if (m_mainRenderPass != VK_NULL_HANDLE)
        {
            vkDestroyRenderPass(m_device, m_mainRenderPass, nullptr);
            m_mainRenderPass = VK_NULL_HANDLE;
        }

        // Swapchain itself has been recreated by caller.
        m_swapchainImageFormat = m_swapchain->GetImageFormat();
        m_extent = m_swapchain->GetExtent();

        createDepthResources();
        createMainRenderPass();
        createFramebuffers();

        for (auto &p : m_passes)
        {
            if (!p)
                continue;
            p->onResize(*m_ctx, m_extent);
            p->onCreate(*m_ctx, m_mainRenderPass, m_framebuffers);
        }
    }

    void Renderer::createSyncObjects()
    {
        // For now we create sync objects per frame in flight to support only single threaded rendering
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // start signaled so the first frame can be submitted immediately

        for (uint32_t i = 0; i < m_maxFrames; ++i)
        {
            FrameContext &f = m_frames[i];

            if (vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &f.imageAcquiredSemaphore) != VK_SUCCESS ||
                vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &f.renderFinishedSemaphore) != VK_SUCCESS)
            {
                throw std::runtime_error("Renderer::createSyncObjects - failed to create semaphores");
            }

            if (vkCreateFence(m_device, &fenceInfo, nullptr, &f.inFlightFence) != VK_SUCCESS)
            {
                throw std::runtime_error("Renderer::createSyncObjects - failed to create fence");
            }
        }
    }

    void Renderer::createCommandPoolsAndBuffers()
    {
        // For now we create one command pool and buffer per frame in flight to support only single threaded rendering
        for (uint32_t i = 0; i < m_maxFrames; ++i)
        {
            FrameContext &f = m_frames[i];

            VkCommandPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            poolInfo.queueFamilyIndex = m_ctx->GetGraphicsQueueFamilyIndex();

            if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &f.commandPool) != VK_SUCCESS)
            {
                throw std::runtime_error("Renderer::createCommandPoolsAndBuffers - failed to create command pool");
            }

            VkCommandBufferAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocInfo.commandPool = f.commandPool;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1;

            if (vkAllocateCommandBuffers(m_device, &allocInfo, &f.commandBuffer) != VK_SUCCESS)
            {
                throw std::runtime_error("Renderer::createCommandPoolsAndBuffers - failed to allocate command buffer");
            }
        }
    }

    void Renderer::drawFrame()
    {
        if (!m_initialized)
            return;

        FrameContext &frame = m_frames[m_currentFrame];
        frame.frameIndex = m_currentFrame;

        // Wait for previous frame to finish
        VkResult r = vkWaitForFences(m_device, 1, &frame.inFlightFence, VK_TRUE, UINT64_MAX);
        if (r != VK_SUCCESS)
        {
            fprintf(stderr, "vkWaitForFences failed: %d\n", r);
            // Recover strategy: mark device lost or try a soft return
            return;
        }

        // Acquire next image
        uint32_t imageIndex = 0;
        VkResult acquireRes = vkAcquireNextImageKHR(
            m_device,
            m_swapchain->GetSwapchain(),
            UINT64_MAX,
            frame.imageAcquiredSemaphore,
            VK_NULL_HANDLE,
            &imageIndex);

        if (acquireRes == VK_ERROR_OUT_OF_DATE_KHR)
        {
            // Window resized or swapchain invalid -> recreate and skip this frame
            m_swapchain->Recreate(m_extent);
            // Framebuffers/renderpass depend on swapchain.
            recreateSwapchainDependent();
            return; // IMPORTANT: we did NOT reset the fence, so next frame’s wait will pass.
        }
        if (acquireRes != VK_SUCCESS && acquireRes != VK_SUBOPTIMAL_KHR)
        {
            fprintf(stderr, "vkAcquireNextImageKHR failed: %d\n", acquireRes);
            return; // Do not reset the fence on failure paths
        }

        // Record command buffer
        vkResetCommandBuffer(frame.commandBuffer, 0);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);

        // GPU timestamp: reset queries for this frame
        const uint32_t startQuery = m_currentFrame * 2;
        const uint32_t endQuery = startQuery + 1;
        if (m_timestampsSupported && m_timestampQueryPool != VK_NULL_HANDLE)
        {
            vkCmdResetQueryPool(frame.commandBuffer, m_timestampQueryPool, startQuery, 2);
            // Write start timestamp (at top of pipe for earliest possible time)
            vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_timestampQueryPool, startQuery);
        }

        // Begin render pass
        VkClearValue clears[2]{};
        clears[0].color = {{0.02f, 0.02f, 0.04f, 1.0f}};
        clears[1].depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo rpBegin{};
        rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass = m_mainRenderPass;
        rpBegin.framebuffer = m_framebuffers[imageIndex];
        rpBegin.renderArea.offset = {0, 0};
        rpBegin.renderArea.extent = m_extent;
        rpBegin.clearValueCount = 2;
        rpBegin.pClearValues = clears;

        vkCmdBeginRenderPass(frame.commandBuffer, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

        // Let modules record draw commands
        for (auto &p : m_passes)
        {
            if (p)
                p->record(frame, frame.commandBuffer);
        }

        // Render ImGui if callback is set
        if (m_imguiRenderCallback)
        {
            m_imguiRenderCallback(frame.commandBuffer);
        }

        vkCmdEndRenderPass(frame.commandBuffer);

        // GPU timestamp: write end timestamp (at bottom of pipe for latest possible time)
        if (m_timestampsSupported && m_timestampQueryPool != VK_NULL_HANDLE)
        {
            vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_timestampQueryPool, endQuery);
        }

        vkEndCommandBuffer(frame.commandBuffer);

        // Submit to graphics queue
        VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT};
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &frame.imageAcquiredSemaphore;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &frame.commandBuffer;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &frame.renderFinishedSemaphore;

        vkResetFences(m_device, 1, &frame.inFlightFence);
        vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, frame.inFlightFence);

        // Read GPU timestamp results from the PREVIOUS frame (which has definitely completed due to fence wait above)
        // We read from the previous frame's queries since the current frame hasn't finished yet
        if (m_timestampsSupported && m_timestampQueryPool != VK_NULL_HANDLE && m_maxFrames > 1)
        {
            // Calculate the previous frame's query indices
            const uint32_t prevFrame = (m_currentFrame + m_maxFrames - 1) % m_maxFrames;
            const uint32_t prevStartQuery = prevFrame * 2;
            uint64_t timestamps[2] = {0, 0};

            VkResult queryResult = vkGetQueryPoolResults(
                m_device,
                m_timestampQueryPool,
                prevStartQuery,
                2, // Query count
                sizeof(timestamps),
                timestamps,
                sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT);

            if (queryResult == VK_SUCCESS && timestamps[1] > timestamps[0])
            {
                // Calculate GPU time in milliseconds
                // timestampPeriod is in nanoseconds per tick
                const uint64_t ticksDelta = timestamps[1] - timestamps[0];
                const float nanoseconds = static_cast<float>(ticksDelta) * m_timestampPeriod;
                m_gpuTimeMs = nanoseconds / 1000000.0f; // Convert ns to ms
            }
        }

        // Present
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &frame.renderFinishedSemaphore;
        VkSwapchainKHR swapchains[] = {m_swapchain->GetSwapchain()};
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapchains;
        presentInfo.pImageIndices = &imageIndex;

        vkQueuePresentKHR(m_presentQueue, &presentInfo);
        // Advance frame index
        m_currentFrame = (m_currentFrame + 1) % m_maxFrames;
    }

    bool Renderer::waitForCurrentFrameFence()
    {
        if (!m_initialized)
            return false;
        if (m_frames.empty())
            return false;
        if (m_currentFrame >= m_frames.size())
            return false;

        FrameContext &frame = m_frames[m_currentFrame];
        VkResult r = vkWaitForFences(m_device, 1, &frame.inFlightFence, VK_TRUE, UINT64_MAX);
        return r == VK_SUCCESS;
    }

    void Renderer::destroySyncObjects()
    {
        for (auto &f : m_frames)
        {
            if (f.imageAcquiredSemaphore != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(m_device, f.imageAcquiredSemaphore, nullptr);
                f.imageAcquiredSemaphore = VK_NULL_HANDLE;
            }
            if (f.renderFinishedSemaphore != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(m_device, f.renderFinishedSemaphore, nullptr);
                f.renderFinishedSemaphore = VK_NULL_HANDLE;
            }
            if (f.inFlightFence != VK_NULL_HANDLE)
            {
                vkDestroyFence(m_device, f.inFlightFence, nullptr);
                f.inFlightFence = VK_NULL_HANDLE;
            }
        }
    }

    void Renderer::destroyCommandPoolsAndBuffers()
    {
        for (auto &f : m_frames)
        {
            if (f.commandPool != VK_NULL_HANDLE)
            {
                vkDestroyCommandPool(m_device, f.commandPool, nullptr);
                f.commandPool = VK_NULL_HANDLE;
                f.commandBuffer = VK_NULL_HANDLE;
            }
        }
    }

    void Renderer::createTimestampQueryPool()
    {
        destroyTimestampQueryPool();

        // Check if the device supports timestamps
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_ctx->GetPhysicalDevice(), &props);

        if (props.limits.timestampComputeAndGraphics == VK_FALSE)
        {
            m_timestampsSupported = false;
            return;
        }

        m_timestampPeriod = props.limits.timestampPeriod; // Nanoseconds per tick
        m_timestampsSupported = true;

        // Create a query pool with 2 queries per frame (start and end)
        // We use 2 * maxFrames to have per-frame queries
        VkQueryPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        poolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        poolInfo.queryCount = m_maxFrames * 2; // 2 timestamps per frame

        if (vkCreateQueryPool(m_device, &poolInfo, nullptr, &m_timestampQueryPool) != VK_SUCCESS)
        {
            m_timestampsSupported = false;
            m_timestampQueryPool = VK_NULL_HANDLE;
        }
    }

    void Renderer::destroyTimestampQueryPool()
    {
        if (m_timestampQueryPool != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(m_device, m_timestampQueryPool, nullptr);
            m_timestampQueryPool = VK_NULL_HANDLE;
        }
        m_timestampsSupported = false;
    }
}