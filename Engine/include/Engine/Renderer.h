#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <memory>
#include <functional>
#include "Structs/FrameContextStruct.h"

namespace Engine
{
    class VulkanContext;
    class SwapChain;
    class RenderPassModule;
    // Renderer: owns the main on-screen VkRenderPass, per-swapchain VkFramebuffer objects,
    // and per-frame command pools/buffers and synchronization objects. It calls registered
    // RenderPassModule::record() while the main render pass is active.

    class Renderer
    {
    public:
        Renderer(VulkanContext *ctx, SwapChain *swapchain, uint32_t maxFramesInFlight = 2);
        ~Renderer();

        // Initialize renderer resources. Must be called after swapchain is created/available.
        // Creates the main render pass, framebuffers, per-frame sync objects and command pools.
        void init(VkExtent2D extent);
        void init();

        // Destroy all renderer resources. Waits for device idle internally.
        void cleanup();

        // Per-frame draw: acquire, record main render pass, submit, present
        void drawFrame();

        // Register a RenderPassModule to be invoked each frame. If init() was already called,
        // the module's onCreate(...) will be invoked immediately so it can allocate resources.
        void registerPass(std::shared_ptr<RenderPassModule> pass);

        // Create the main render pass that targets the swapchain (implementation helper).
        void createMainRenderPass();

        // Create framebuffers for each swapchain image view (implementation helper).
        void createFramebuffers();

        VkRenderPass getMainRenderPass() const { return m_mainRenderPass; }
        VkExtent2D getExtent() const { return m_extent; }

        // Index of the frame slot that will be used by the next drawFrame() call.
        uint32_t getCurrentFrameIndex() const { return m_currentFrame; }

        // Number of per-frame slots (frames in flight) owned by the renderer.
        uint32_t getMaxFramesInFlight() const { return m_maxFrames; }

        // Wait for the current frame slot to become available (its in-flight fence is signaled).
        // Useful when preparing per-frame uploads *before* calling drawFrame().
        bool waitForCurrentFrameFence();

        // Set a callback for rendering ImGui (called after all render pass modules)
        using ImGuiRenderCallback = std::function<void(VkCommandBuffer)>;
        void setImGuiRenderCallback(ImGuiRenderCallback callback) { m_imguiRenderCallback = callback; }

        // Get the last measured GPU frame time in milliseconds
        float getGpuTimeMs() const { return m_gpuTimeMs; }

    private:
        VulkanContext *m_ctx = nullptr;
        SwapChain *m_swapchain = nullptr;
        VkDevice m_device = VK_NULL_HANDLE;
        VkQueue m_graphicsQueue = VK_NULL_HANDLE;
        VkQueue m_presentQueue = VK_NULL_HANDLE;

        uint32_t m_maxFrames = 2;
        bool m_initialized = false;

        // Main render-pass and per-swapchain framebuffers
        VkRenderPass m_mainRenderPass = VK_NULL_HANDLE;
        std::vector<VkFramebuffer> m_framebuffers;
        VkExtent2D m_extent{};
        VkFormat m_swapchainImageFormat = VK_FORMAT_UNDEFINED;

        // Depth attachment resources (one per swapchain image)
        VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
        std::vector<VkImage> m_depthImages;
        std::vector<VkDeviceMemory> m_depthMemories;
        std::vector<VkImageView> m_depthImageViews;

        std::vector<FrameContext> m_frames;
        uint32_t m_currentFrame = 0;

        // Registered render-pass modules that will record into the main render pass.
        std::vector<std::shared_ptr<RenderPassModule>> m_passes;

        // Optional ImGui render callback
        ImGuiRenderCallback m_imguiRenderCallback;

        // GPU timestamp query support
        VkQueryPool m_timestampQueryPool = VK_NULL_HANDLE;
        float m_timestampPeriod = 1.0f; // Nanoseconds per timestamp tick
        float m_gpuTimeMs = 0.0f;       // Last measured GPU time in milliseconds
        bool m_timestampsSupported = false;

    private:
        // Create semaphores and fences for each frame slot (called during init).
        void createSyncObjects();

        // Create per-frame command pools and allocate one primary command buffer per frame.
        void createCommandPoolsAndBuffers();

        // Destroy helpers
        void destroySyncObjects();
        void destroyCommandPoolsAndBuffers();

        // Depth helpers
        void createDepthResources();
        void destroyDepthResources();

        // Swapchain-dependent recreate helper
        void recreateSwapchainDependent();

        // GPU timestamp helpers
        void createTimestampQueryPool();
        void destroyTimestampQueryPool();
    };

    class RenderPassModule
    {
    public:
        virtual ~RenderPassModule() = default;

        // Called after the main render pass and framebuffers are created
        virtual void onCreate(VulkanContext &ctx, VkRenderPass pass, const std::vector<VkFramebuffer> &fbs) = 0;

        // Record drawing commands for this pass into the provided command buffer
        virtual void record(FrameContext &frameCtx, VkCommandBuffer cmd) = 0;

        // Called when swapchain/extent changes
        virtual void onResize(VulkanContext &ctx, VkExtent2D newExtent) = 0;

        // Called to destroy any device resources owned by this module (pipelines, layouts, shaders, descriptors, etc.)
        virtual void onDestroy(VulkanContext &ctx) = 0;
    };
}