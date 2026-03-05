#include "Engine/MeshRenderPassModule.h"
#include "Engine/Pipeline.h"
#include "Engine/VulkanContext.h"
#include "Engine/SwapChain.h"
#include "Engine/Camera.h"
#include "utils/ImageUtils.h"
#include "assets/AssetManager.h"
#include <stdexcept>
#include <iostream>
#include <cstring>
#include <algorithm>
#include <unordered_set>
#include <glm/gtc/matrix_transform.hpp>

namespace Engine
{
    // ============================================================================
    // Constructor / Destructor
    // ============================================================================

    MeshRenderPassModule::~MeshRenderPassModule()
    {
        // Resources cleaned up in onDestroy
    }

    // ============================================================================
    // Public setters
    // ============================================================================

    void MeshRenderPassModule::setCamera(Camera *camera)
    {
        m_camera = camera;
    }

    void MeshRenderPassModule::setAssetManager(AssetManager *assetManager)
    {
        m_assetManager = assetManager;
    }

    void MeshRenderPassModule::setModelHandle(ModelHandle model)
    {
        m_modelHandle = model;
    }

    // ============================================================================
    // Lifecycle: onCreate
    // ============================================================================

    void MeshRenderPassModule::onCreate(VulkanContext &ctx, VkRenderPass pass, const std::vector<VkFramebuffer> &fbs)
    {
        (void)fbs;

        m_device = ctx.GetDevice();
        m_phys = ctx.GetPhysicalDevice();
        m_extent = ctx.GetSwapChain()->GetExtent();

        // Create persistent upload command pool
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = ctx.GetGraphicsQueueFamilyIndex();
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        VkResult pr = vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_uploadPool);
        if (pr != VK_SUCCESS)
        {
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
            fprintf(stderr, "MeshRenderPassModule: failed to create upload pool\n");
#endif
            m_uploadPool = VK_NULL_HANDLE;
        }

        // Count materials
        size_t materialCount = 0;
        if (m_assetManager && m_modelHandle.isValid())
        {
            if (ModelAsset *model = m_assetManager->getModel(m_modelHandle))
            {
                materialCount = model->primitives.size();
            }
        }

        // Create resources
        createDescriptorLayouts();
        createDescriptorPool(materialCount);
        createCameraUBO();
        createDummyTexture(ctx);
        createPipelinesForModel(ctx, pass);
        createMaterialDescriptors();
    }

    // ============================================================================
    // Lifecycle: record
    // ============================================================================

    void MeshRenderPassModule::record(FrameContext &frameCtx, VkCommandBuffer cmd)
    {
        (void)frameCtx;

        if (!m_enabled || !m_camera || !m_assetManager)
            return;
        if (m_extent.width == 0 || m_extent.height == 0)
            return;
        if (!m_modelHandle.isValid())
            return;

        ModelAsset *model = m_assetManager->getModel(m_modelHandle);
        if (!model || model->primitives.empty())
            return;

        // Update camera UBO
        updateCameraUBO();

        // Set viewport and scissor
        VkViewport vp{};
        vp.x = 0.0f;
        vp.y = 0.0f;
        vp.width = static_cast<float>(m_extent.width);
        vp.height = static_cast<float>(m_extent.height);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &vp);

        VkRect2D sc{};
        sc.offset = {0, 0};
        sc.extent = m_extent;
        vkCmdSetScissor(cmd, 0, 1, &sc);

        // Bind camera descriptor (set 0)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                                0, 1, &m_cameraDescriptorSet, 0, nullptr);

        // Draw each primitive
        for (size_t i = 0; i < model->primitives.size(); ++i)
        {
            drawNode(cmd, i);
        }
    }

    // ============================================================================
    // Lifecycle: onResize
    // ============================================================================

    void MeshRenderPassModule::onResize(VulkanContext &ctx, VkExtent2D newExtent)
    {
        (void)ctx;
        m_extent = newExtent;
    }

    // ============================================================================
    // Lifecycle: onDestroy
    // ============================================================================

    void MeshRenderPassModule::onDestroy(VulkanContext &ctx)
    {
        (void)ctx;
        cleanupResources();
    }

    // ============================================================================
    // Draw single node (primitive)
    // ============================================================================

    void MeshRenderPassModule::drawNode(VkCommandBuffer cmd, size_t primitiveIndex)
    {
        if (!m_assetManager || !m_modelHandle.isValid())
            return;

        ModelAsset *model = m_assetManager->getModel(m_modelHandle);
        if (!model || primitiveIndex >= model->primitives.size())
            return;

        const auto &prim = model->primitives[primitiveIndex];
        MeshAsset *mesh = m_assetManager->getMesh(prim.mesh);
        if (!mesh)
            return;

        VkBuffer vb = mesh->getVertexBuffer();
        VkBuffer ib = mesh->getIndexBuffer();
        if (vb == VK_NULL_HANDLE || ib == VK_NULL_HANDLE)
            return;

        // Get mesh vertex stride
        uint32_t meshStride = mesh->getVertexStride();
        if (meshStride == 0)
            meshStride = 32; // fallback

        // Bind appropriate pipeline for this vertex stride
        auto it = m_pipelines.find(meshStride);
        if (it == m_pipelines.end())
        {
            // Try fallback to first available pipeline
            if (!m_pipelines.empty())
                it = m_pipelines.begin();
            else
                return;
        }
        it->second.bind(cmd);

        // Bind material descriptor (set 1)
        if (primitiveIndex < m_materialDescriptorSets.size())
        {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                                    1, 1, &m_materialDescriptorSets[primitiveIndex], 0, nullptr);
        }

        // Bind vertex and index buffers
        VkDeviceSize vbOffset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &vbOffset);
        vkCmdBindIndexBuffer(cmd, ib, 0, mesh->getIndexType());

        // Compute model matrix (auto-center and auto-scale from AABB)
        glm::mat4 modelMat = computeModelMatrix(mesh);
        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(glm::mat4), &modelMat);

        // Draw
        vkCmdDrawIndexed(cmd, prim.indexCount, 1, prim.firstIndex, prim.vertexOffset, 0);
    }

    // ============================================================================
    // Descriptor layouts
    // ============================================================================

    void MeshRenderPassModule::createDescriptorLayouts()
    {
        // Set 0: Camera UBO
        VkDescriptorSetLayoutBinding camBinding{};
        camBinding.binding = 0;
        camBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        camBinding.descriptorCount = 1;
        camBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo camLayoutInfo{};
        camLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        camLayoutInfo.bindingCount = 1;
        camLayoutInfo.pBindings = &camBinding;

        VkResult r = vkCreateDescriptorSetLayout(m_device, &camLayoutInfo, nullptr, &m_cameraSetLayout);
        if (r != VK_SUCCESS)
        {
            throw std::runtime_error("MeshRenderPassModule: failed to create camera descriptor set layout");
        }

        // Set 1: Material UBO + 5 textures
        std::vector<VkDescriptorSetLayoutBinding> matBindings(6);

        matBindings[0].binding = 0;
        matBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        matBindings[0].descriptorCount = 1;
        matBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        for (int i = 1; i <= 5; ++i)
        {
            matBindings[i].binding = i;
            matBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            matBindings[i].descriptorCount = 1;
            matBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }

        VkDescriptorSetLayoutCreateInfo matLayoutInfo{};
        matLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        matLayoutInfo.bindingCount = static_cast<uint32_t>(matBindings.size());
        matLayoutInfo.pBindings = matBindings.data();

        r = vkCreateDescriptorSetLayout(m_device, &matLayoutInfo, nullptr, &m_materialSetLayout);
        if (r != VK_SUCCESS)
        {
            throw std::runtime_error("MeshRenderPassModule: failed to create material descriptor set layout");
        }
    }

    // ============================================================================
    // Descriptor pool
    // ============================================================================

    void MeshRenderPassModule::createDescriptorPool(size_t materialCount)
    {
        if (materialCount == 0)
            materialCount = 1;

        std::vector<VkDescriptorPoolSize> poolSizes(2);
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[0].descriptorCount = static_cast<uint32_t>(1 + materialCount);
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[1].descriptorCount = static_cast<uint32_t>(materialCount * 5);

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        poolInfo.maxSets = static_cast<uint32_t>(1 + materialCount);

        VkResult r = vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool);
        if (r != VK_SUCCESS)
        {
            throw std::runtime_error("MeshRenderPassModule: failed to create descriptor pool");
        }
    }

    // ============================================================================
    // Camera UBO
    // ============================================================================

    void MeshRenderPassModule::createCameraUBO()
    {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = sizeof(glm::mat4) * 2 + sizeof(glm::vec4);
        bufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkResult r = vkCreateBuffer(m_device, &bufInfo, nullptr, &m_cameraUBO);
        if (r != VK_SUCCESS)
        {
            throw std::runtime_error("MeshRenderPassModule: failed to create camera UBO");
        }

        VkMemoryRequirements memReq;
        vkGetBufferMemoryRequirements(m_device, m_cameraUBO, &memReq);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReq.memoryTypeBits,
                                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        r = vkAllocateMemory(m_device, &allocInfo, nullptr, &m_cameraUBOMemory);
        if (r != VK_SUCCESS)
        {
            vkDestroyBuffer(m_device, m_cameraUBO, nullptr);
            m_cameraUBO = VK_NULL_HANDLE;
            throw std::runtime_error("MeshRenderPassModule: failed to allocate camera UBO memory");
        }

        r = vkBindBufferMemory(m_device, m_cameraUBO, m_cameraUBOMemory, 0);
        if (r != VK_SUCCESS)
        {
            vkFreeMemory(m_device, m_cameraUBOMemory, nullptr);
            vkDestroyBuffer(m_device, m_cameraUBO, nullptr);
            throw std::runtime_error("MeshRenderPassModule: failed to bind camera UBO memory");
        }

        // Persistently map the memory
        r = vkMapMemory(m_device, m_cameraUBOMemory, 0, VK_WHOLE_SIZE, 0, &m_cameraUBOMapped);
        if (r != VK_SUCCESS)
        {
            throw std::runtime_error("MeshRenderPassModule: failed to map camera UBO memory");
        }

        // Allocate descriptor set
        VkDescriptorSetAllocateInfo allocDesc{};
        allocDesc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocDesc.descriptorPool = m_descriptorPool;
        allocDesc.descriptorSetCount = 1;
        allocDesc.pSetLayouts = &m_cameraSetLayout;

        r = vkAllocateDescriptorSets(m_device, &allocDesc, &m_cameraDescriptorSet);
        if (r != VK_SUCCESS)
        {
            throw std::runtime_error("MeshRenderPassModule: failed to allocate camera descriptor set");
        }

        // Write descriptor
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = m_cameraUBO;
        bufferInfo.offset = 0;
        bufferInfo.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_cameraDescriptorSet;
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &bufferInfo;

        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }

    // ============================================================================
    // Update camera UBO
    // ============================================================================

    void MeshRenderPassModule::updateCameraUBO()
    {
        if (!m_camera || !m_cameraUBOMapped)
            return;

        struct CameraUBO
        {
            glm::mat4 view;
            glm::mat4 proj;
            glm::vec3 cameraPos;
            float _pad;
        } ubo;

        ubo.view = m_camera->GetViewMatrix();
        ubo.proj = m_camera->GetProjectionMatrix();
        ubo.cameraPos = m_camera->GetPosition();
        ubo._pad = 0.0f;

        std::memcpy(m_cameraUBOMapped, &ubo, sizeof(CameraUBO));
    }

    // ============================================================================
    // Dummy texture
    // ============================================================================

    void MeshRenderPassModule::createDummyTexture(VulkanContext &ctx)
    {
        // Create 1x1 white texture
        VkImageCreateInfo imgInfo{};
        imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.imageType = VK_IMAGE_TYPE_2D;
        imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imgInfo.extent = {1, 1, 1};
        imgInfo.mipLevels = 1;
        imgInfo.arrayLayers = 1;
        imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.tiling = VK_IMAGE_TILING_LINEAR;
        imgInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        imgInfo.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;

        VkResult r = vkCreateImage(m_device, &imgInfo, nullptr, &m_dummyImage);
        if (r != VK_SUCCESS)
        {
            throw std::runtime_error("MeshRenderPassModule: failed to create dummy image");
        }

        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(m_device, m_dummyImage, &memReq);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReq.memoryTypeBits,
                                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        r = vkAllocateMemory(m_device, &allocInfo, nullptr, &m_dummyImageMemory);
        if (r != VK_SUCCESS)
        {
            throw std::runtime_error("MeshRenderPassModule: failed to allocate dummy image memory");
        }

        vkBindImageMemory(m_device, m_dummyImage, m_dummyImageMemory, 0);

        // Write white pixel
        void *p = nullptr;
        vkMapMemory(m_device, m_dummyImageMemory, 0, VK_WHOLE_SIZE, 0, &p);
        uint8_t white[4] = {255, 255, 255, 255};
        std::memcpy(p, white, 4);
        vkUnmapMemory(m_device, m_dummyImageMemory);

        // Transition layout using upload pool
        if (m_uploadPool != VK_NULL_HANDLE)
        {
            Engine::UploadContext upload{};
            if (Engine::BeginUploadContext(upload, m_device, m_phys, m_uploadPool, ctx.GetGraphicsQueue()))
            {
                VkImageMemoryBarrier barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                barrier.oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
                barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = m_dummyImage;
                barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.baseMipLevel = 0;
                barrier.subresourceRange.levelCount = 1;
                barrier.subresourceRange.baseArrayLayer = 0;
                barrier.subresourceRange.layerCount = 1;

                vkCmdPipelineBarrier(upload.cmd,
                                     VK_PIPELINE_STAGE_HOST_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &barrier);

                Engine::EndSubmitAndWait(upload);
            }
        }

        // Create image view
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_dummyImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        r = vkCreateImageView(m_device, &viewInfo, nullptr, &m_dummyImageView);
        if (r != VK_SUCCESS)
        {
            throw std::runtime_error("MeshRenderPassModule: failed to create dummy image view");
        }

        // Create sampler
        VkSamplerCreateInfo sampInfo{};
        sampInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampInfo.magFilter = VK_FILTER_LINEAR;
        sampInfo.minFilter = VK_FILTER_LINEAR;
        sampInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampInfo.anisotropyEnable = VK_FALSE;
        sampInfo.maxAnisotropy = 1.0f;
        sampInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_WHITE;
        sampInfo.unnormalizedCoordinates = VK_FALSE;
        sampInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

        r = vkCreateSampler(m_device, &sampInfo, nullptr, &m_dummySampler);
        if (r != VK_SUCCESS)
        {
            throw std::runtime_error("MeshRenderPassModule: failed to create dummy sampler");
        }
    }

    // ============================================================================
    // Create pipelines for all vertex strides in model
    // ============================================================================

    void MeshRenderPassModule::createPipelinesForModel(VulkanContext &ctx, VkRenderPass pass)
    {
        // Collect unique vertex strides from model
        std::unordered_set<uint32_t> strides;

        if (m_assetManager && m_modelHandle.isValid())
        {
            if (ModelAsset *model = m_assetManager->getModel(m_modelHandle))
            {
                for (const auto &prim : model->primitives)
                {
                    MeshAsset *mesh = m_assetManager->getMesh(prim.mesh);
                    if (mesh)
                    {
                        uint32_t stride = mesh->getVertexStride();
                        if (stride > 0)
                            strides.insert(stride);
                    }
                }
            }
        }

        // Fallback to standard stride if none found
        if (strides.empty())
            strides.insert(32);

        // Create pipeline layout (shared by all pipelines)
        VkDescriptorSetLayout setLayouts[] = {m_cameraSetLayout, m_materialSetLayout};

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(glm::mat4);

        VkPipelineLayoutCreateInfo plInfo{};
        plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.setLayoutCount = 2;
        plInfo.pSetLayouts = setLayouts;
        plInfo.pushConstantRangeCount = 1;
        plInfo.pPushConstantRanges = &pushRange;

        VkResult r = vkCreatePipelineLayout(ctx.GetDevice(), &plInfo, nullptr, &m_pipelineLayout);
        if (r != VK_SUCCESS)
        {
            throw std::runtime_error("MeshRenderPassModule: failed to create pipeline layout");
        }

        // Load shaders once
        VkShaderModule vert = Pipeline::createShaderModuleFromFile(ctx.GetDevice(),
                                                                   "C:\\Users\\user\\Desktop\\Stratosphere\\Engine\\shaders\\mesh.vert.spv");
        VkShaderModule frag = Pipeline::createShaderModuleFromFile(ctx.GetDevice(),
                                                                   "C:\\Users\\user\\Desktop\\Stratosphere\\Engine\\shaders\\mesh.frag.spv");

        if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE)
        {
            throw std::runtime_error("MeshRenderPassModule: failed to load shader modules");
        }

        // Create pipeline for each stride
        for (uint32_t stride : strides)
        {
            PipelineCreateInfo pci{};
            pci.device = ctx.GetDevice();
            pci.renderPass = pass;
            pci.subpass = 0;

            // Shader stages
            VkPipelineShaderStageCreateInfo vs{};
            vs.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            vs.stage = VK_SHADER_STAGE_VERTEX_BIT;
            vs.module = vert;
            vs.pName = "main";

            VkPipelineShaderStageCreateInfo fs{};
            fs.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            fs.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            fs.module = frag;
            fs.pName = "main";
            pci.shaderStages = {vs, fs};

            pci.pipelineLayout = m_pipelineLayout;

            // Vertex input with stride
            VkVertexInputBindingDescription bindingDesc{};
            bindingDesc.binding = 0;
            bindingDesc.stride = stride;
            bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

            // Attributes (pos/normal/uv at standard offsets)
            VkVertexInputAttributeDescription attrs[3]{};
            attrs[0].location = 0;
            attrs[0].binding = 0;
            attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
            attrs[0].offset = 0;

            attrs[1].location = 1;
            attrs[1].binding = 0;
            attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
            attrs[1].offset = 12;

            attrs[2].location = 2;
            attrs[2].binding = 0;
            attrs[2].format = VK_FORMAT_R32G32_SFLOAT;
            attrs[2].offset = 24;

            VkPipelineVertexInputStateCreateInfo vi{};
            vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            vi.vertexBindingDescriptionCount = 1;
            vi.pVertexBindingDescriptions = &bindingDesc;
            vi.vertexAttributeDescriptionCount = 3;
            vi.pVertexAttributeDescriptions = attrs;
            pci.vertexInput = vi;
            pci.vertexInputProvided = true;

            // Input assembly
            VkPipelineInputAssemblyStateCreateInfo ia{};
            ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            ia.primitiveRestartEnable = VK_FALSE;
            pci.inputAssembly = ia;
            pci.inputAssemblyProvided = true;

            // Rasterization
            VkPipelineRasterizationStateCreateInfo rs{};
            rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rs.depthClampEnable = VK_FALSE;
            rs.rasterizerDiscardEnable = VK_FALSE;
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.lineWidth = 1.0f;
            rs.cullMode = VK_CULL_MODE_BACK_BIT;
            rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rs.depthBiasEnable = VK_FALSE;
            pci.rasterization = rs;
            pci.rasterizationProvided = true;

            // Depth/stencil
            VkPipelineDepthStencilStateCreateInfo ds{};
            ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            ds.depthTestEnable = VK_TRUE;
            ds.depthWriteEnable = VK_TRUE;
            ds.depthCompareOp = VK_COMPARE_OP_LESS;
            ds.depthBoundsTestEnable = VK_FALSE;
            ds.stencilTestEnable = VK_FALSE;
            pci.depthStencil = ds;
            pci.depthStencilProvided = true;

            pci.dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

            // Create pipeline in map
            auto &pipelineRef = m_pipelines[stride];
            r = pipelineRef.create(pci);
            if (r != VK_SUCCESS)
            {
                fprintf(stderr, "MeshRenderPassModule: failed to create pipeline for stride %u\n", stride);
            }
        }

        vkDestroyShaderModule(ctx.GetDevice(), vert, nullptr);
        vkDestroyShaderModule(ctx.GetDevice(), frag, nullptr);
    }

    // ============================================================================
    // Create material descriptors
    // ============================================================================

    void MeshRenderPassModule::createMaterialDescriptors()
    {
        m_materialDescriptorSets.clear();
        m_materialUBOs.clear();
        m_materialUBOMemories.clear();

        if (!m_modelHandle.isValid() || !m_assetManager)
            return;

        ModelAsset *model = m_assetManager->getModel(m_modelHandle);
        if (!model || model->primitives.empty())
            return;

        std::vector<VkDescriptorSetLayout> layouts(model->primitives.size(), m_materialSetLayout);
        std::vector<VkDescriptorSet> descriptorSets(model->primitives.size());

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_descriptorPool;
        allocInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
        allocInfo.pSetLayouts = layouts.data();

        VkResult r = vkAllocateDescriptorSets(m_device, &allocInfo, descriptorSets.data());
        if (r != VK_SUCCESS)
            return;

        for (size_t i = 0; i < model->primitives.size(); ++i)
        {
            const auto &prim = model->primitives[i];
            MaterialAsset *mat = m_assetManager->getMaterial(prim.material);

            // Create material UBO
            VkBuffer ubo = VK_NULL_HANDLE;
            VkDeviceMemory uboMem = VK_NULL_HANDLE;

            VkBufferCreateInfo bufInfo{};
            bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufInfo.size = sizeof(float) * 8;
            bufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            vkCreateBuffer(m_device, &bufInfo, nullptr, &ubo);

            VkMemoryRequirements memReq;
            vkGetBufferMemoryRequirements(m_device, ubo, &memReq);

            VkMemoryAllocateInfo memAllocInfo{};
            memAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            memAllocInfo.allocationSize = memReq.size;
            memAllocInfo.memoryTypeIndex = findMemoryType(memReq.memoryTypeBits,
                                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vkAllocateMemory(m_device, &memAllocInfo, nullptr, &uboMem);
            vkBindBufferMemory(m_device, ubo, uboMem, 0);

            // Fill UBO
            struct MaterialUBO
            {
                float baseColorFactor[4];
                float metallicFactor;
                float roughnessFactor;
                float pad[2];
            } uboData;

            if (mat)
            {
                std::memcpy(uboData.baseColorFactor, mat->baseColorFactor, sizeof(float) * 4);
                uboData.metallicFactor = mat->metallicFactor;
                uboData.roughnessFactor = mat->roughnessFactor;
            }
            else
            {
                uboData.baseColorFactor[0] = 1.0f;
                uboData.baseColorFactor[1] = 1.0f;
                uboData.baseColorFactor[2] = 1.0f;
                uboData.baseColorFactor[3] = 1.0f;
                uboData.metallicFactor = 0.0f;
                uboData.roughnessFactor = 0.5f;
            }
            uboData.pad[0] = 0.0f;
            uboData.pad[1] = 0.0f;

            void *data = nullptr;
            vkMapMemory(m_device, uboMem, 0, sizeof(MaterialUBO), 0, &data);
            std::memcpy(data, &uboData, sizeof(MaterialUBO));
            vkUnmapMemory(m_device, uboMem);

            m_materialUBOs.push_back(ubo);
            m_materialUBOMemories.push_back(uboMem);

            // Update descriptor set
            std::vector<VkWriteDescriptorSet> writes;

            VkDescriptorBufferInfo matUBOInfo{};
            matUBOInfo.buffer = ubo;
            matUBOInfo.offset = 0;
            matUBOInfo.range = sizeof(MaterialUBO);

            VkWriteDescriptorSet matUBOWrite{};
            matUBOWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            matUBOWrite.dstSet = descriptorSets[i];
            matUBOWrite.dstBinding = 0;
            matUBOWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            matUBOWrite.descriptorCount = 1;
            matUBOWrite.pBufferInfo = &matUBOInfo;
            writes.push_back(matUBOWrite);

            // Textures
            std::vector<VkDescriptorImageInfo> imageInfos(5);
            TextureHandle texHandles[5] = {
                mat ? mat->baseColorTexture : TextureHandle{},
                mat ? mat->normalTexture : TextureHandle{},
                mat ? mat->metallicRoughnessTexture : TextureHandle{},
                mat ? mat->occlusionTexture : TextureHandle{},
                mat ? mat->emissiveTexture : TextureHandle{}};

            for (int t = 0; t < 5; ++t)
            {
                imageInfos[t].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                TextureAsset *tex = texHandles[t].isValid() ? m_assetManager->getTexture(texHandles[t]) : nullptr;
                if (tex && tex->getView() != VK_NULL_HANDLE)
                {
                    imageInfos[t].imageView = tex->getView();
                    imageInfos[t].sampler = tex->getSampler();
                }
                else
                {
                    imageInfos[t].imageView = m_dummyImageView;
                    imageInfos[t].sampler = m_dummySampler;
                }

                VkWriteDescriptorSet texWrite{};
                texWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                texWrite.dstSet = descriptorSets[i];
                texWrite.dstBinding = 1 + t;
                texWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                texWrite.descriptorCount = 1;
                texWrite.pImageInfo = &imageInfos[t];
                writes.push_back(texWrite);
            }

            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }

        m_materialDescriptorSets = descriptorSets;
    }

    // ============================================================================
    // Compute model matrix
    // ============================================================================

    glm::mat4 MeshRenderPassModule::computeModelMatrix(MeshAsset *mesh)
    {
        const float *aabbMin = mesh->getAABBMin();
        const float *aabbMax = mesh->getAABBMax();

        if (aabbMin && aabbMax)
        {
            glm::vec3 minV(aabbMin[0], aabbMin[1], aabbMin[2]);
            glm::vec3 maxV(aabbMax[0], aabbMax[1], aabbMax[2]);
            glm::vec3 center = (minV + maxV) * 0.5f;
            glm::vec3 ext = maxV - minV;
            float maxExtent = std::max({ext.x, ext.y, ext.z});

            const float targetSize = 2.0f;
            float scale = (maxExtent > 1e-6f) ? (targetSize / maxExtent) : 1.0f;

            return glm::scale(glm::mat4(1.0f), glm::vec3(scale)) *
                   glm::translate(glm::mat4(1.0f), -center);
        }

        return glm::mat4(1.0f);
    }

    // ============================================================================
    // Cleanup
    // ============================================================================

    void MeshRenderPassModule::cleanupResources()
    {
        // Material UBOs
        for (size_t i = 0; i < m_materialUBOs.size(); ++i)
        {
            if (m_materialUBOs[i] != VK_NULL_HANDLE)
                vkDestroyBuffer(m_device, m_materialUBOs[i], nullptr);
            if (m_materialUBOMemories[i] != VK_NULL_HANDLE)
                vkFreeMemory(m_device, m_materialUBOMemories[i], nullptr);
        }
        m_materialUBOs.clear();
        m_materialUBOMemories.clear();

        // Pipeline layout
        if (m_pipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
            m_pipelineLayout = VK_NULL_HANDLE;
        }

        // Pipelines
        for (auto &kv : m_pipelines)
        {
            kv.second.destroy(m_device);
        }
        m_pipelines.clear();

        // Camera UBO (unmap if mapped)
        if (m_cameraUBOMapped && m_cameraUBOMemory != VK_NULL_HANDLE)
        {
            vkUnmapMemory(m_device, m_cameraUBOMemory);
            m_cameraUBOMapped = nullptr;
        }
        if (m_cameraUBO != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(m_device, m_cameraUBO, nullptr);
            m_cameraUBO = VK_NULL_HANDLE;
        }
        if (m_cameraUBOMemory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_device, m_cameraUBOMemory, nullptr);
            m_cameraUBOMemory = VK_NULL_HANDLE;
        }

        // Descriptor layouts
        if (m_cameraSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(m_device, m_cameraSetLayout, nullptr);
            m_cameraSetLayout = VK_NULL_HANDLE;
        }
        if (m_materialSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(m_device, m_materialSetLayout, nullptr);
            m_materialSetLayout = VK_NULL_HANDLE;
        }

        // Descriptor pool
        if (m_descriptorPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
            m_descriptorPool = VK_NULL_HANDLE;
        }

        // Dummy texture
        if (m_dummySampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(m_device, m_dummySampler, nullptr);
            m_dummySampler = VK_NULL_HANDLE;
        }
        if (m_dummyImageView != VK_NULL_HANDLE)
        {
            vkDestroyImageView(m_device, m_dummyImageView, nullptr);
            m_dummyImageView = VK_NULL_HANDLE;
        }
        if (m_dummyImage != VK_NULL_HANDLE)
        {
            vkDestroyImage(m_device, m_dummyImage, nullptr);
            m_dummyImage = VK_NULL_HANDLE;
        }
        if (m_dummyImageMemory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_device, m_dummyImageMemory, nullptr);
            m_dummyImageMemory = VK_NULL_HANDLE;
        }

        // Upload pool
        if (m_uploadPool != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(m_device, m_uploadPool, nullptr);
            m_uploadPool = VK_NULL_HANDLE;
        }
    }

    // ============================================================================
    // Helper: find memory type
    // ============================================================================

    uint32_t MeshRenderPassModule::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const
    {
        VkPhysicalDeviceMemoryProperties memProps;
        vkGetPhysicalDeviceMemoryProperties(m_phys, &memProps);
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
        {
            if ((typeFilter & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
                return i;
        }
        throw std::runtime_error("MeshRenderPassModule: failed to find suitable memory type");
    }

} // namespace Engine
