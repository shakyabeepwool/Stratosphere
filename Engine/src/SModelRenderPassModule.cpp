#include "Engine/SModelRenderPassModule.h"
#include "Engine/VulkanContext.h"
#include "Engine/SwapChain.h"
#include "Engine/PerformanceMonitor.h"
#include "assets/ModelAsset.h"
#include "assets/MeshAsset.h"
#include "assets/MaterialAsset.h"
#include "utils/ImageUtils.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <unordered_set>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace Engine
{

    static void setIdentity(float outM[16])
    {
        std::memset(outM, 0, sizeof(float) * 16);
        outM[0] = 1.0f;
        outM[5] = 1.0f;
        outM[10] = 1.0f;
        outM[15] = 1.0f;
    }

    SModelRenderPassModule::~SModelRenderPassModule()
    {
        // resources freed in onDestroy
    }

    bool SModelRenderPassModule::createMaterialResources(VulkanContext &ctx)
    {
        destroyMaterialResources();

        // Descriptor set layout: baseColor combined sampler
        VkDescriptorSetLayoutBinding baseColorBinding{};
        baseColorBinding.binding = 0;
        baseColorBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        baseColorBinding.descriptorCount = 1;
        baseColorBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo dsl{};
        dsl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsl.bindingCount = 1;
        dsl.pBindings = &baseColorBinding;

        if (vkCreateDescriptorSetLayout(ctx.GetDevice(), &dsl, nullptr, &m_materialSetLayout) != VK_SUCCESS)
        {
            return false;
        }

        // Fallback 1x1 white texture (sRGB)
        {
            VkCommandPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolInfo.queueFamilyIndex = ctx.GetGraphicsQueueFamilyIndex();
            poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

            VkCommandPool uploadPool = VK_NULL_HANDLE;
            if (vkCreateCommandPool(ctx.GetDevice(), &poolInfo, nullptr, &uploadPool) != VK_SUCCESS)
                return false;

            UploadContext upload{};
            if (!BeginUploadContext(upload, ctx.GetDevice(), ctx.GetPhysicalDevice(), uploadPool, ctx.GetGraphicsQueue()))
            {
                vkDestroyCommandPool(ctx.GetDevice(), uploadPool, nullptr);
                return false;
            }

            const uint8_t white[4] = {255, 255, 255, 255};
            const bool ok = m_fallbackWhiteTexture.uploadRGBA8_Deferred(
                upload,
                white,
                1,
                1,
                true,
                VK_SAMPLER_ADDRESS_MODE_REPEAT,
                VK_SAMPLER_ADDRESS_MODE_REPEAT,
                VK_FILTER_LINEAR,
                VK_FILTER_LINEAR,
                VK_SAMPLER_MIPMAP_MODE_NEAREST,
                1.0f);

            const bool submitted = ok && EndSubmitAndWait(upload);
            vkDestroyCommandPool(ctx.GetDevice(), uploadPool, nullptr);

            if (!submitted)
                return false;
        }

        // Descriptor pool (size tuned to current model if available)
        uint32_t uniqueMatCount = 32;
        if (m_assets && m_model.isValid())
        {
            if (ModelAsset *model = m_assets->getModel(m_model))
            {
                std::unordered_set<uint64_t> unique;
                unique.reserve(model->primitives.size());
                for (const auto &p : model->primitives)
                {
                    if (p.material.isValid())
                        unique.insert(p.material.id);
                }
                if (!unique.empty())
                    uniqueMatCount = static_cast<uint32_t>(unique.size());
            }
        }

        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = uniqueMatCount;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = uniqueMatCount;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;

        if (vkCreateDescriptorPool(ctx.GetDevice(), &poolInfo, nullptr, &m_materialPool) != VK_SUCCESS)
            return false;

        m_materialSetCache.clear();
        return true;
    }

    void SModelRenderPassModule::destroyMaterialResources()
    {
        m_materialSetCache.clear();

        if (m_materialPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(m_device, m_materialPool, nullptr);
            m_materialPool = VK_NULL_HANDLE;
        }

        if (m_materialSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(m_device, m_materialSetLayout, nullptr);
            m_materialSetLayout = VK_NULL_HANDLE;
        }

        if (m_device != VK_NULL_HANDLE && m_fallbackWhiteTexture.isValid())
        {
            m_fallbackWhiteTexture.destroy(m_device);
        }
    }

    VkDescriptorSet SModelRenderPassModule::getOrCreateMaterialSet(MaterialHandle h, const MaterialAsset *mat)
    {
        if (!h.isValid() || !mat)
            return VK_NULL_HANDLE;
        if (m_materialPool == VK_NULL_HANDLE || m_materialSetLayout == VK_NULL_HANDLE)
            return VK_NULL_HANDLE;

        auto it = m_materialSetCache.find(h.id);
        if (it != m_materialSetCache.end())
            return it->second;

        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = m_materialPool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &m_materialSetLayout;

        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(m_device, &alloc, &set) != VK_SUCCESS)
            return VK_NULL_HANDLE;

        VkImageView view = m_fallbackWhiteTexture.getView();
        VkSampler sampler = m_fallbackWhiteTexture.getSampler();

        if (mat->baseColorTexture.isValid() && m_assets)
        {
            if (TextureAsset *tex = m_assets->getTexture(mat->baseColorTexture))
            {
                if (tex->getView() != VK_NULL_HANDLE && tex->getSampler() != VK_NULL_HANDLE)
                {
                    view = tex->getView();
                    sampler = tex->getSampler();
                }
            }
        }

        VkDescriptorImageInfo di{};
        di.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        di.imageView = view;
        di.sampler = sampler;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = 0;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &di;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);

        m_materialSetCache.emplace(h.id, set);
        return set;
    }

    void SModelRenderPassModule::setModelMatrix(const float *m16)
    {
        if (!m16)
        {
            setIdentity(m_pc.model);
            return;
        }
        std::memcpy(m_pc.model, m16, sizeof(float) * 16);
    }

    void SModelRenderPassModule::prepareFrameUploads(
        uint32_t frameIndex,
        const glm::mat4 *instanceWorlds, uint32_t instanceCount,
        const glm::mat4 *nodePalette, uint32_t nodeCount,
        const glm::mat4 *jointPalette, uint32_t jointStride,
        const UploadRange *dirtyWorldRanges, uint32_t dirtyWorldRangeCount,
        const UploadRange *dirtyPoseRanges, uint32_t dirtyPoseRangeCount,
        bool forceFullUpload)
    {
        if (!m_enabled)
            return;
        if (m_device == VK_NULL_HANDLE)
            return;
        if (m_cameraFrames.empty() || m_instanceFrames.empty())
            return;

        // Update camera UBO (persistent mapping).
        {
            const uint32_t camIndex = frameIndex % static_cast<uint32_t>(m_cameraFrames.size());
            CameraFrame &camFrame = m_cameraFrames[camIndex];
            if (camFrame.mapped)
            {
                CameraUBO ubo{};
                const float aspect = (m_extent.height > 0) ? (static_cast<float>(m_extent.width) / static_cast<float>(m_extent.height)) : 1.0f;
                if (m_camera)
                {
                    m_camera->SetAspect(aspect);
                    ubo.view = m_camera->GetViewMatrix();
                    ubo.proj = m_camera->GetProjectionMatrix();
                }
                else
                {
                    glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 3), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
                    glm::mat4 proj = glm::perspective(glm::radians(60.0f), aspect, 0.1f, 100.0f);
                    proj[1][1] *= -1.0f;
                    ubo.view = view;
                    ubo.proj = proj;
                }

                std::memcpy(camFrame.mapped, &ubo, sizeof(CameraUBO));
            }
        }

        if (instanceCount == 0)
            return;

        const uint32_t safeNodeCount = std::max(1u, nodeCount);
        const uint32_t safeJointStride = std::max(1u, jointStride);

        const uint32_t instIndex = frameIndex % static_cast<uint32_t>(m_instanceFrames.size());
        InstanceFrame &instFrame = m_instanceFrames[instIndex];

        const uint32_t camIndex = frameIndex % static_cast<uint32_t>(m_cameraFrames.size());
        CameraFrame &camFrame = m_cameraFrames[camIndex];

        bool didRealloc = false;

        // Ensure capacities; if any buffer reallocates, force a full upload.
        {
            const uint32_t oldCap = instFrame.capacity;
            if (!ensureInstanceCapacity(instFrame, instanceCount))
                return;
            didRealloc |= (instFrame.capacity != oldCap);
        }

        {
            const uint32_t neededNodeMatrices = instanceCount * safeNodeCount;
            const uint32_t oldCap = camFrame.paletteCapacityMatrices;
            if (!ensurePaletteCapacity(camFrame, neededNodeMatrices))
                return;
            didRealloc |= (camFrame.paletteCapacityMatrices != oldCap);
        }

        {
            const uint32_t neededJointMatrices = instanceCount * safeJointStride;
            const uint32_t oldCap = camFrame.jointPaletteCapacityMatrices;
            if (!ensureJointPaletteCapacity(camFrame, neededJointMatrices))
                return;
            didRealloc |= (camFrame.jointPaletteCapacityMatrices != oldCap);
        }

        const bool doFull = forceFullUpload || didRealloc;

        // Instance worlds (vertex buffer, binding=1).
        if (instFrame.mapped && instanceWorlds)
        {
            if (doFull)
            {
                std::memcpy(instFrame.mapped, instanceWorlds, sizeof(glm::mat4) * static_cast<size_t>(instanceCount));
            }
            else
            {
                for (uint32_t i = 0; i < dirtyWorldRangeCount; ++i)
                {
                    const UploadRange r = dirtyWorldRanges[i];
                    if (r.instanceCount == 0)
                        continue;
                    if (r.firstInstance >= instanceCount)
                        continue;
                    const uint32_t end = std::min(instanceCount, r.firstInstance + r.instanceCount);
                    const uint32_t count = end - r.firstInstance;
                    const size_t byteOffset = static_cast<size_t>(r.firstInstance) * sizeof(glm::mat4);
                    const size_t byteCount = static_cast<size_t>(count) * sizeof(glm::mat4);
                    std::memcpy(static_cast<uint8_t *>(instFrame.mapped) + byteOffset, instanceWorlds + r.firstInstance, byteCount);
                }
            }
        }

        // Node palette (SSBO, set=0 binding=1): packed as [instance][node].
        if (camFrame.paletteMapped && nodePalette)
        {
            if (doFull)
            {
                const size_t total = static_cast<size_t>(instanceCount) * static_cast<size_t>(safeNodeCount);
                std::memcpy(camFrame.paletteMapped, nodePalette, sizeof(glm::mat4) * total);
            }
            else
            {
                for (uint32_t i = 0; i < dirtyPoseRangeCount; ++i)
                {
                    const UploadRange r = dirtyPoseRanges[i];
                    if (r.instanceCount == 0)
                        continue;
                    if (r.firstInstance >= instanceCount)
                        continue;
                    const uint32_t end = std::min(instanceCount, r.firstInstance + r.instanceCount);
                    const uint32_t count = end - r.firstInstance;
                    const size_t firstMatrix = static_cast<size_t>(r.firstInstance) * static_cast<size_t>(safeNodeCount);
                    const size_t matrixCount = static_cast<size_t>(count) * static_cast<size_t>(safeNodeCount);
                    const size_t byteOffset = firstMatrix * sizeof(glm::mat4);
                    const size_t byteCount = matrixCount * sizeof(glm::mat4);
                    std::memcpy(static_cast<uint8_t *>(camFrame.paletteMapped) + byteOffset, nodePalette + firstMatrix, byteCount);
                }
            }
        }

        // Joint palette (SSBO, set=0 binding=2): packed as [instance][joint].
        if (camFrame.jointPaletteMapped && jointPalette)
        {
            if (doFull)
            {
                const size_t total = static_cast<size_t>(instanceCount) * static_cast<size_t>(safeJointStride);
                std::memcpy(camFrame.jointPaletteMapped, jointPalette, sizeof(glm::mat4) * total);
            }
            else
            {
                for (uint32_t i = 0; i < dirtyPoseRangeCount; ++i)
                {
                    const UploadRange r = dirtyPoseRanges[i];
                    if (r.instanceCount == 0)
                        continue;
                    if (r.firstInstance >= instanceCount)
                        continue;
                    const uint32_t end = std::min(instanceCount, r.firstInstance + r.instanceCount);
                    const uint32_t count = end - r.firstInstance;
                    const size_t firstMatrix = static_cast<size_t>(r.firstInstance) * static_cast<size_t>(safeJointStride);
                    const size_t matrixCount = static_cast<size_t>(count) * static_cast<size_t>(safeJointStride);
                    const size_t byteOffset = firstMatrix * sizeof(glm::mat4);
                    const size_t byteCount = matrixCount * sizeof(glm::mat4);
                    std::memcpy(static_cast<uint8_t *>(camFrame.jointPaletteMapped) + byteOffset, jointPalette + firstMatrix, byteCount);
                }
            }
        }
    }

    bool SModelRenderPassModule::refreshModelMatrix()
    {
        if (!m_assets || !m_model.isValid())
        {
            setIdentity(m_pc.model);
            return false;
        }

        ModelAsset *model = m_assets->getModel(m_model);
        if (!model)
        {
            setIdentity(m_pc.model);
            return false;
        }

        // Prefer precomputed bounds/scale from AssetManager
        float center[3] = {0.0f, 0.0f, 0.0f};
        float minY = 0.0f;
        float scale = 1.0f;
        bool hasBounds = model->hasBounds;

        if (model->hasBounds)
        {
            center[0] = model->center[0];
            center[1] = model->center[1];
            center[2] = model->center[2];
            minY = model->boundsMin[1];
            scale = model->fitScale;
        }
        else
        {
            // Fallback: compute from meshes now
            float bmin[3] = {0.0f, 0.0f, 0.0f};
            float bmax[3] = {0.0f, 0.0f, 0.0f};
            bool first = true;
            for (const ModelPrimitive &prim : model->primitives)
            {
                MeshAsset *mesh = m_assets->getMesh(prim.mesh);
                if (!mesh)
                    continue;
                const float *mn = mesh->getAABBMin();
                const float *mx = mesh->getAABBMax();
                if (first)
                {
                    std::memcpy(bmin, mn, sizeof(bmin));
                    std::memcpy(bmax, mx, sizeof(bmax));
                    first = false;
                }
                else
                {
                    bmin[0] = std::min(bmin[0], mn[0]);
                    bmin[1] = std::min(bmin[1], mn[1]);
                    bmin[2] = std::min(bmin[2], mn[2]);

                    bmax[0] = std::max(bmax[0], mx[0]);
                    bmax[1] = std::max(bmax[1], mx[1]);
                    bmax[2] = std::max(bmax[2], mx[2]);
                }
            }

            if (!first)
            {
                center[0] = 0.5f * (bmin[0] + bmax[0]);
                center[1] = 0.5f * (bmin[1] + bmax[1]);
                center[2] = 0.5f * (bmin[2] + bmax[2]);

                minY = bmin[1];

                const float sizeX = bmax[0] - bmin[0];
                const float sizeY = bmax[1] - bmin[1];
                const float sizeZ = bmax[2] - bmin[2];
                const float maxExtent = std::max(sizeX, std::max(sizeY, sizeZ));
                const float target = 20.0f;
                const float epsilon = 1e-4f;
                scale = (maxExtent > epsilon) ? (target / maxExtent) : 1.0f;
                hasBounds = true;
            }
        }

        if (!hasBounds)
        {
            setIdentity(m_pc.model);
            return false;
        }

        // Build M = S * T:
        // - center in XZ so the model rotates nicely around its middle
        // - align base (AABB minY) to y=0 so characters sit on the ground
        setIdentity(m_pc.model);
        m_pc.model[0] = scale;
        m_pc.model[5] = scale;
        m_pc.model[10] = scale;
        m_pc.model[12] = -center[0] * scale;
        m_pc.model[13] = -minY * scale;
        m_pc.model[14] = -center[2] * scale;
        return true;
    }

    void SModelRenderPassModule::onCreate(VulkanContext &ctx, VkRenderPass pass, const std::vector<VkFramebuffer> &fbs)
    {
        (void)fbs;
        ++m_resourceGeneration;
        m_device = ctx.GetDevice();
        m_physicalDevice = ctx.GetPhysicalDevice();
        m_extent = ctx.GetSwapChain() ? ctx.GetSwapChain()->GetExtent() : VkExtent2D{};

        // Default model matrix: center/scale from bounds if available
        if (!refreshModelMatrix())
        {
            setIdentity(m_pc.model);
        }

        const size_t frameCount = fbs.size();
        if (!createCameraResources(ctx, frameCount > 0 ? frameCount : 1))
        {
            throw std::runtime_error("SModelRenderPassModule: failed to create camera resources");
        }

        if (!createInstanceResources(ctx, frameCount > 0 ? frameCount : 1))
        {
            throw std::runtime_error("SModelRenderPassModule: failed to create instance resources");
        }

        if (!createMaterialResources(ctx))
        {
            throw std::runtime_error("SModelRenderPassModule: failed to create material resources");
        }

        createPipelines(ctx, pass);
    }

    VkPipelineColorBlendStateCreateInfo SModelRenderPassModule::makeBlendState(bool enableBlend, VkPipelineColorBlendAttachmentState &outAttachment) const
    {
        std::memset(&outAttachment, 0, sizeof(outAttachment));
        outAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        outAttachment.blendEnable = enableBlend ? VK_TRUE : VK_FALSE;
        if (enableBlend)
        {
            // Standard alpha blending
            outAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            outAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            outAttachment.colorBlendOp = VK_BLEND_OP_ADD;
            outAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            outAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            outAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        }

        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.logicOpEnable = VK_FALSE;
        cb.attachmentCount = 1;
        cb.pAttachments = &outAttachment;
        return cb;
    }

    bool SModelRenderPassModule::createCameraResources(VulkanContext &ctx, size_t frameCount)
    {
        destroyCameraResources();

        if (frameCount == 0)
            frameCount = 1;

        VkDescriptorSetLayoutBinding camBinding{};
        camBinding.binding = 0;
        camBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        camBinding.descriptorCount = 1;
        camBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutBinding paletteBinding{};
        paletteBinding.binding = 1;
        paletteBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        paletteBinding.descriptorCount = 1;
        paletteBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutBinding jointPaletteBinding{};
        jointPaletteBinding.binding = 2;
        jointPaletteBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        jointPaletteBinding.descriptorCount = 1;
        jointPaletteBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutCreateInfo dsl{};
        dsl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        VkDescriptorSetLayoutBinding bindings[3] = {camBinding, paletteBinding, jointPaletteBinding};
        dsl.bindingCount = 3;
        dsl.pBindings = bindings;

        if (vkCreateDescriptorSetLayout(ctx.GetDevice(), &dsl, nullptr, &m_cameraSetLayout) != VK_SUCCESS)
        {
            return false;
        }

        // Pool: one uniform buffer descriptor + two storage buffer descriptors per frame
        VkDescriptorPoolSize poolSizes[2]{};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[0].descriptorCount = static_cast<uint32_t>(frameCount);
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSizes[1].descriptorCount = static_cast<uint32_t>(frameCount) * 2u;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = static_cast<uint32_t>(frameCount);
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes;

        if (vkCreateDescriptorPool(ctx.GetDevice(), &poolInfo, nullptr, &m_cameraPool) != VK_SUCCESS)
        {
            return false;
        }

        std::vector<VkDescriptorSetLayout> layouts(frameCount, m_cameraSetLayout);
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_cameraPool;
        allocInfo.descriptorSetCount = static_cast<uint32_t>(frameCount);
        allocInfo.pSetLayouts = layouts.data();

        m_cameraFrames.resize(frameCount);

        std::vector<VkDescriptorSet> sets(frameCount, VK_NULL_HANDLE);
        if (vkAllocateDescriptorSets(ctx.GetDevice(), &allocInfo, sets.data()) != VK_SUCCESS)
        {
            return false;
        }

        auto findMemoryType = [&](uint32_t typeFilter, VkMemoryPropertyFlags properties, uint32_t &typeIndex) -> bool
        {
            VkPhysicalDeviceMemoryProperties memProps{};
            vkGetPhysicalDeviceMemoryProperties(ctx.GetPhysicalDevice(), &memProps);
            for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
            {
                if ((typeFilter & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
                {
                    typeIndex = i;
                    return true;
                }
            }
            return false;
        };

        const VkDeviceSize bufSize = sizeof(CameraUBO);
        for (size_t i = 0; i < frameCount; ++i)
        {
            CameraFrame &cf = m_cameraFrames[i];
            cf.set = sets[i];

            VkBufferCreateInfo binfo{};
            binfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            binfo.size = bufSize;
            binfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            binfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            if (vkCreateBuffer(ctx.GetDevice(), &binfo, nullptr, &cf.buffer) != VK_SUCCESS)
                return false;

            VkMemoryRequirements memReq{};
            vkGetBufferMemoryRequirements(ctx.GetDevice(), cf.buffer, &memReq);

            VkMemoryAllocateInfo mai{};
            mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            mai.allocationSize = memReq.size;
            uint32_t memType = 0;
            if (!findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, memType))
                return false;
            mai.memoryTypeIndex = memType;

            if (vkAllocateMemory(ctx.GetDevice(), &mai, nullptr, &cf.memory) != VK_SUCCESS)
                return false;

            vkBindBufferMemory(ctx.GetDevice(), cf.buffer, cf.memory, 0);

            // Persistently map camera UBO memory.
            cf.mapped = nullptr;
            if (vkMapMemory(ctx.GetDevice(), cf.memory, 0, VK_WHOLE_SIZE, 0, &cf.mapped) != VK_SUCCESS)
                return false;

            // Palette SSBO (host-visible, coherent, persistently mapped)
            constexpr uint32_t kDefaultPaletteCapacityMatrices = 1024;
            cf.paletteCapacityMatrices = kDefaultPaletteCapacityMatrices;

            VkBufferCreateInfo pbinfo{};
            pbinfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            pbinfo.size = static_cast<VkDeviceSize>(cf.paletteCapacityMatrices) * sizeof(glm::mat4);
            pbinfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            pbinfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            if (vkCreateBuffer(ctx.GetDevice(), &pbinfo, nullptr, &cf.paletteBuffer) != VK_SUCCESS)
                return false;

            VkMemoryRequirements pmemReq{};
            vkGetBufferMemoryRequirements(ctx.GetDevice(), cf.paletteBuffer, &pmemReq);

            VkMemoryAllocateInfo pmai{};
            pmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            pmai.allocationSize = pmemReq.size;
            uint32_t pmemType = 0;
            if (!findMemoryType(pmemReq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, pmemType))
                return false;
            pmai.memoryTypeIndex = pmemType;

            if (vkAllocateMemory(ctx.GetDevice(), &pmai, nullptr, &cf.paletteMemory) != VK_SUCCESS)
                return false;

            vkBindBufferMemory(ctx.GetDevice(), cf.paletteBuffer, cf.paletteMemory, 0);

            cf.paletteMapped = nullptr;
            if (vkMapMemory(ctx.GetDevice(), cf.paletteMemory, 0, VK_WHOLE_SIZE, 0, &cf.paletteMapped) != VK_SUCCESS)
                return false;

            // Joint palette SSBO (host-visible, coherent, persistently mapped)
            constexpr uint32_t kDefaultJointPaletteCapacityMatrices = 1024;
            cf.jointPaletteCapacityMatrices = kDefaultJointPaletteCapacityMatrices;

            VkBufferCreateInfo jbinfo{};
            jbinfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            jbinfo.size = static_cast<VkDeviceSize>(cf.jointPaletteCapacityMatrices) * sizeof(glm::mat4);
            jbinfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            jbinfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            if (vkCreateBuffer(ctx.GetDevice(), &jbinfo, nullptr, &cf.jointPaletteBuffer) != VK_SUCCESS)
                return false;

            VkMemoryRequirements jmemReq{};
            vkGetBufferMemoryRequirements(ctx.GetDevice(), cf.jointPaletteBuffer, &jmemReq);

            VkMemoryAllocateInfo jmai{};
            jmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            jmai.allocationSize = jmemReq.size;
            uint32_t jmemType = 0;
            if (!findMemoryType(jmemReq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, jmemType))
                return false;
            jmai.memoryTypeIndex = jmemType;

            if (vkAllocateMemory(ctx.GetDevice(), &jmai, nullptr, &cf.jointPaletteMemory) != VK_SUCCESS)
                return false;

            vkBindBufferMemory(ctx.GetDevice(), cf.jointPaletteBuffer, cf.jointPaletteMemory, 0);

            cf.jointPaletteMapped = nullptr;
            if (vkMapMemory(ctx.GetDevice(), cf.jointPaletteMemory, 0, VK_WHOLE_SIZE, 0, &cf.jointPaletteMapped) != VK_SUCCESS)
                return false;

            VkDescriptorBufferInfo dbi{};
            dbi.buffer = cf.buffer;
            dbi.offset = 0;
            dbi.range = bufSize;

            VkDescriptorBufferInfo pbi{};
            pbi.buffer = cf.paletteBuffer;
            pbi.offset = 0;
            pbi.range = static_cast<VkDeviceSize>(cf.paletteCapacityMatrices) * sizeof(glm::mat4);

            VkDescriptorBufferInfo jbi{};
            jbi.buffer = cf.jointPaletteBuffer;
            jbi.offset = 0;
            jbi.range = static_cast<VkDeviceSize>(cf.jointPaletteCapacityMatrices) * sizeof(glm::mat4);

            VkWriteDescriptorSet writes[3]{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = cf.set;
            writes[0].dstBinding = 0;
            writes[0].dstArrayElement = 0;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].descriptorCount = 1;
            writes[0].pBufferInfo = &dbi;

            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = cf.set;
            writes[1].dstBinding = 1;
            writes[1].dstArrayElement = 0;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[1].descriptorCount = 1;
            writes[1].pBufferInfo = &pbi;

            writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[2].dstSet = cf.set;
            writes[2].dstBinding = 2;
            writes[2].dstArrayElement = 0;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[2].descriptorCount = 1;
            writes[2].pBufferInfo = &jbi;

            vkUpdateDescriptorSets(ctx.GetDevice(), 3, writes, 0, nullptr);
        }

        return true;
    }

    void SModelRenderPassModule::destroyCameraResources()
    {
        for (auto &cf : m_cameraFrames)
        {
            if (cf.mapped && cf.memory != VK_NULL_HANDLE)
            {
                vkUnmapMemory(m_device, cf.memory);
                cf.mapped = nullptr;
            }

            if (cf.paletteMapped && cf.paletteMemory != VK_NULL_HANDLE)
            {
                vkUnmapMemory(m_device, cf.paletteMemory);
                cf.paletteMapped = nullptr;
            }

            if (cf.jointPaletteMapped && cf.jointPaletteMemory != VK_NULL_HANDLE)
            {
                vkUnmapMemory(m_device, cf.jointPaletteMemory);
                cf.jointPaletteMapped = nullptr;
            }

            if (cf.paletteBuffer != VK_NULL_HANDLE)
            {
                vkDestroyBuffer(m_device, cf.paletteBuffer, nullptr);
                cf.paletteBuffer = VK_NULL_HANDLE;
            }
            if (cf.paletteMemory != VK_NULL_HANDLE)
            {
                vkFreeMemory(m_device, cf.paletteMemory, nullptr);
                cf.paletteMemory = VK_NULL_HANDLE;
            }
            cf.paletteCapacityMatrices = 0;

            if (cf.jointPaletteBuffer != VK_NULL_HANDLE)
            {
                vkDestroyBuffer(m_device, cf.jointPaletteBuffer, nullptr);
                cf.jointPaletteBuffer = VK_NULL_HANDLE;
            }
            if (cf.jointPaletteMemory != VK_NULL_HANDLE)
            {
                vkFreeMemory(m_device, cf.jointPaletteMemory, nullptr);
                cf.jointPaletteMemory = VK_NULL_HANDLE;
            }
            cf.jointPaletteCapacityMatrices = 0;

            if (cf.buffer != VK_NULL_HANDLE)
            {
                vkDestroyBuffer(m_device, cf.buffer, nullptr);
                cf.buffer = VK_NULL_HANDLE;
            }
            if (cf.memory != VK_NULL_HANDLE)
            {
                vkFreeMemory(m_device, cf.memory, nullptr);
                cf.memory = VK_NULL_HANDLE;
            }
            cf.set = VK_NULL_HANDLE;
        }
        m_cameraFrames.clear();

        if (m_cameraPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(m_device, m_cameraPool, nullptr);
            m_cameraPool = VK_NULL_HANDLE;
        }
        if (m_cameraSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(m_device, m_cameraSetLayout, nullptr);
            m_cameraSetLayout = VK_NULL_HANDLE;
        }
    }

    bool SModelRenderPassModule::ensurePaletteCapacity(CameraFrame &frame, uint32_t neededMatrices)
    {
        if (neededMatrices <= frame.paletteCapacityMatrices)
            return true;
        if (m_device == VK_NULL_HANDLE || m_physicalDevice == VK_NULL_HANDLE)
            return false;
        if (frame.set == VK_NULL_HANDLE)
            return false;

        uint32_t newCap = std::max<uint32_t>(1u, frame.paletteCapacityMatrices);
        while (newCap < neededMatrices)
            newCap *= 2u;

        if (frame.paletteMapped && frame.paletteMemory != VK_NULL_HANDLE)
        {
            vkUnmapMemory(m_device, frame.paletteMemory);
            frame.paletteMapped = nullptr;
        }
        if (frame.paletteBuffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(m_device, frame.paletteBuffer, nullptr);
            frame.paletteBuffer = VK_NULL_HANDLE;
        }
        if (frame.paletteMemory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_device, frame.paletteMemory, nullptr);
            frame.paletteMemory = VK_NULL_HANDLE;
        }

        auto findMemoryType = [&](uint32_t typeFilter, VkMemoryPropertyFlags properties, uint32_t &typeIndex) -> bool
        {
            VkPhysicalDeviceMemoryProperties memProps{};
            vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);
            for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
            {
                if ((typeFilter & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
                {
                    typeIndex = i;
                    return true;
                }
            }
            return false;
        };

        VkBufferCreateInfo binfo{};
        binfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        binfo.size = static_cast<VkDeviceSize>(newCap) * sizeof(glm::mat4);
        binfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        binfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(m_device, &binfo, nullptr, &frame.paletteBuffer) != VK_SUCCESS)
            return false;

        VkMemoryRequirements memReq{};
        vkGetBufferMemoryRequirements(m_device, frame.paletteBuffer, &memReq);

        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = memReq.size;
        uint32_t memType = 0;
        if (!findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, memType))
            return false;
        mai.memoryTypeIndex = memType;

        if (vkAllocateMemory(m_device, &mai, nullptr, &frame.paletteMemory) != VK_SUCCESS)
            return false;

        vkBindBufferMemory(m_device, frame.paletteBuffer, frame.paletteMemory, 0);

        frame.paletteMapped = nullptr;
        if (vkMapMemory(m_device, frame.paletteMemory, 0, VK_WHOLE_SIZE, 0, &frame.paletteMapped) != VK_SUCCESS)
            return false;

        frame.paletteCapacityMatrices = newCap;

        VkDescriptorBufferInfo pbi{};
        pbi.buffer = frame.paletteBuffer;
        pbi.offset = 0;
        pbi.range = static_cast<VkDeviceSize>(frame.paletteCapacityMatrices) * sizeof(glm::mat4);

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = frame.set;
        write.dstBinding = 1;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &pbi;

        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
        return true;
    }

    bool SModelRenderPassModule::ensureJointPaletteCapacity(CameraFrame &frame, uint32_t neededMatrices)
    {
        if (neededMatrices <= frame.jointPaletteCapacityMatrices)
            return true;
        if (m_device == VK_NULL_HANDLE || m_physicalDevice == VK_NULL_HANDLE)
            return false;
        if (frame.set == VK_NULL_HANDLE)
            return false;

        uint32_t newCap = std::max<uint32_t>(1u, frame.jointPaletteCapacityMatrices);
        while (newCap < neededMatrices)
            newCap *= 2u;

        if (frame.jointPaletteMapped && frame.jointPaletteMemory != VK_NULL_HANDLE)
        {
            vkUnmapMemory(m_device, frame.jointPaletteMemory);
            frame.jointPaletteMapped = nullptr;
        }
        if (frame.jointPaletteBuffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(m_device, frame.jointPaletteBuffer, nullptr);
            frame.jointPaletteBuffer = VK_NULL_HANDLE;
        }
        if (frame.jointPaletteMemory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_device, frame.jointPaletteMemory, nullptr);
            frame.jointPaletteMemory = VK_NULL_HANDLE;
        }

        auto findMemoryType = [&](uint32_t typeFilter, VkMemoryPropertyFlags properties, uint32_t &typeIndex) -> bool
        {
            VkPhysicalDeviceMemoryProperties memProps{};
            vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);
            for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
            {
                if ((typeFilter & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
                {
                    typeIndex = i;
                    return true;
                }
            }
            return false;
        };

        VkBufferCreateInfo binfo{};
        binfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        binfo.size = static_cast<VkDeviceSize>(newCap) * sizeof(glm::mat4);
        binfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        binfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(m_device, &binfo, nullptr, &frame.jointPaletteBuffer) != VK_SUCCESS)
            return false;

        VkMemoryRequirements memReq{};
        vkGetBufferMemoryRequirements(m_device, frame.jointPaletteBuffer, &memReq);

        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = memReq.size;
        uint32_t memType = 0;
        if (!findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, memType))
            return false;
        mai.memoryTypeIndex = memType;

        if (vkAllocateMemory(m_device, &mai, nullptr, &frame.jointPaletteMemory) != VK_SUCCESS)
            return false;

        vkBindBufferMemory(m_device, frame.jointPaletteBuffer, frame.jointPaletteMemory, 0);

        frame.jointPaletteMapped = nullptr;
        if (vkMapMemory(m_device, frame.jointPaletteMemory, 0, VK_WHOLE_SIZE, 0, &frame.jointPaletteMapped) != VK_SUCCESS)
            return false;

        frame.jointPaletteCapacityMatrices = newCap;

        VkDescriptorBufferInfo jbi{};
        jbi.buffer = frame.jointPaletteBuffer;
        jbi.offset = 0;
        jbi.range = static_cast<VkDeviceSize>(frame.jointPaletteCapacityMatrices) * sizeof(glm::mat4);

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = frame.set;
        write.dstBinding = 2;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &jbi;

        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
        return true;
    }

    bool SModelRenderPassModule::createInstanceResources(VulkanContext &ctx, size_t frameCount)
    {
        destroyInstanceResources();

        if (frameCount == 0)
            frameCount = 1;

        auto findMemoryType = [&](uint32_t typeFilter, VkMemoryPropertyFlags properties, uint32_t &typeIndex) -> bool
        {
            VkPhysicalDeviceMemoryProperties memProps{};
            vkGetPhysicalDeviceMemoryProperties(ctx.GetPhysicalDevice(), &memProps);
            for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
            {
                if ((typeFilter & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
                {
                    typeIndex = i;
                    return true;
                }
            }
            return false;
        };

        m_instanceFrames.resize(frameCount);

        // Start with a modest default capacity; grows on demand.
        constexpr uint32_t kDefaultCapacity = 256;
        const VkDeviceSize bufSize = static_cast<VkDeviceSize>(kDefaultCapacity) * sizeof(glm::mat4);

        for (size_t i = 0; i < frameCount; ++i)
        {
            InstanceFrame &fr = m_instanceFrames[i];
            fr.capacity = kDefaultCapacity;

            VkBufferCreateInfo binfo{};
            binfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            binfo.size = bufSize;
            binfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            binfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            if (vkCreateBuffer(ctx.GetDevice(), &binfo, nullptr, &fr.buffer) != VK_SUCCESS)
                return false;

            VkMemoryRequirements memReq{};
            vkGetBufferMemoryRequirements(ctx.GetDevice(), fr.buffer, &memReq);

            VkMemoryAllocateInfo mai{};
            mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            mai.allocationSize = memReq.size;
            uint32_t memType = 0;
            if (!findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, memType))
                return false;
            mai.memoryTypeIndex = memType;

            if (vkAllocateMemory(ctx.GetDevice(), &mai, nullptr, &fr.memory) != VK_SUCCESS)
                return false;

            vkBindBufferMemory(ctx.GetDevice(), fr.buffer, fr.memory, 0);

            fr.mapped = nullptr;
            if (vkMapMemory(ctx.GetDevice(), fr.memory, 0, VK_WHOLE_SIZE, 0, &fr.mapped) != VK_SUCCESS)
                return false;
        }

        return true;
    }

    void SModelRenderPassModule::destroyInstanceResources()
    {
        if (m_device == VK_NULL_HANDLE)
            return;

        for (auto &fr : m_instanceFrames)
        {
            if (fr.mapped && fr.memory != VK_NULL_HANDLE)
            {
                vkUnmapMemory(m_device, fr.memory);
                fr.mapped = nullptr;
            }

            if (fr.buffer != VK_NULL_HANDLE)
            {
                vkDestroyBuffer(m_device, fr.buffer, nullptr);
                fr.buffer = VK_NULL_HANDLE;
            }

            if (fr.memory != VK_NULL_HANDLE)
            {
                vkFreeMemory(m_device, fr.memory, nullptr);
                fr.memory = VK_NULL_HANDLE;
            }

            fr.capacity = 0;
        }
        m_instanceFrames.clear();
    }

    bool SModelRenderPassModule::ensureInstanceCapacity(InstanceFrame &frame, uint32_t needed)
    {
        if (needed <= frame.capacity)
            return true;
        if (m_device == VK_NULL_HANDLE || m_physicalDevice == VK_NULL_HANDLE)
            return false;

        // Grow by doubling.
        uint32_t newCap = std::max<uint32_t>(1u, frame.capacity);
        while (newCap < needed)
            newCap *= 2u;

        if (frame.mapped && frame.memory != VK_NULL_HANDLE)
        {
            vkUnmapMemory(m_device, frame.memory);
            frame.mapped = nullptr;
        }
        if (frame.buffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(m_device, frame.buffer, nullptr);
            frame.buffer = VK_NULL_HANDLE;
        }
        if (frame.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_device, frame.memory, nullptr);
            frame.memory = VK_NULL_HANDLE;
        }

        auto findMemoryType = [&](uint32_t typeFilter, VkMemoryPropertyFlags properties, uint32_t &typeIndex) -> bool
        {
            VkPhysicalDeviceMemoryProperties memProps{};
            vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);
            for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
            {
                if ((typeFilter & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
                {
                    typeIndex = i;
                    return true;
                }
            }
            return false;
        };

        const VkDeviceSize bufSize = static_cast<VkDeviceSize>(newCap) * sizeof(glm::mat4);
        VkBufferCreateInfo binfo{};
        binfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        binfo.size = bufSize;
        binfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        binfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(m_device, &binfo, nullptr, &frame.buffer) != VK_SUCCESS)
            return false;

        VkMemoryRequirements memReq{};
        vkGetBufferMemoryRequirements(m_device, frame.buffer, &memReq);

        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = memReq.size;
        uint32_t memType = 0;
        if (!findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, memType))
            return false;
        mai.memoryTypeIndex = memType;

        if (vkAllocateMemory(m_device, &mai, nullptr, &frame.memory) != VK_SUCCESS)
            return false;

        vkBindBufferMemory(m_device, frame.buffer, frame.memory, 0);

        frame.mapped = nullptr;
        if (vkMapMemory(m_device, frame.memory, 0, VK_WHOLE_SIZE, 0, &frame.mapped) != VK_SUCCESS)
            return false;

        frame.capacity = newCap;
        return true;
    }

    void SModelRenderPassModule::createPipelines(VulkanContext &ctx, VkRenderPass pass)
    {
        if (m_cameraSetLayout == VK_NULL_HANDLE)
        {
            throw std::runtime_error("SModelRenderPassModule: camera descriptor set layout not created");
        }
        if (m_materialSetLayout == VK_NULL_HANDLE)
        {
            throw std::runtime_error("SModelRenderPassModule: material descriptor set layout not created");
        }

        // Shared pipeline layout: camera set + push constants.
        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pcRange.offset = 0;
        pcRange.size = sizeof(PushConstantsModel);

        VkPipelineLayoutCreateInfo plInfo{};
        plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        VkDescriptorSetLayout setLayouts[2] = {m_cameraSetLayout, m_materialSetLayout};
        plInfo.setLayoutCount = 2;
        plInfo.pSetLayouts = setLayouts;
        plInfo.pushConstantRangeCount = 1;
        plInfo.pPushConstantRanges = &pcRange;

        if (vkCreatePipelineLayout(ctx.GetDevice(), &plInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        {
            throw std::runtime_error("SModelRenderPassModule: failed to create pipeline layout");
        }

        // Common pipeline create info
        PipelineCreateInfo pci{};
        pci.device = ctx.GetDevice();
        pci.renderPass = pass;
        pci.subpass = 0;
        pci.pipelineLayout = m_pipelineLayout;

        // Load shader modules
        VkShaderModule vert = Pipeline::createShaderModuleFromFile(pci.device, "shaders/smodel.vert.spv");
        VkShaderModule frag = Pipeline::createShaderModuleFromFile(pci.device, "shaders/smodel.frag.spv");
        if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE)
        {
            throw std::runtime_error("SModelRenderPassModule: failed to load shader modules (smodel.vert/frag.spv)");
        }

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

        // Vertex input:
        //  binding 0: VertexPNTTJW (72 bytes)
        //  binding 1: Instance mat4 (64 bytes), advanced per-instance
        std::array<VkVertexInputBindingDescription, 2> bindingDescs{};
        bindingDescs[0].binding = 0;
        bindingDescs[0].stride = 72;
        bindingDescs[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        bindingDescs[1].binding = 1;
        bindingDescs[1].stride = sizeof(glm::mat4);
        bindingDescs[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

        std::array<VkVertexInputAttributeDescription, 10> attrs{};
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};     // pos
        attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12};    // normal
        attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, 24};       // uv0
        attrs[3] = {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32}; // tangent

        // Skinning inputs
        attrs[4] = {8, 0, VK_FORMAT_R16G16B16A16_UINT, 48};   // joints (u16x4)
        attrs[5] = {9, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 56}; // weights (f32x4)

        // mat4 consumes 4 locations (vec4 columns)
        attrs[6] = {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
        attrs[7] = {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16};
        attrs[8] = {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32};
        attrs[9] = {7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 48};

        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount = static_cast<uint32_t>(bindingDescs.size());
        vi.pVertexBindingDescriptions = bindingDescs.data();
        vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
        vi.pVertexAttributeDescriptions = attrs.data();
        pci.vertexInput = vi;
        pci.vertexInputProvided = true;

        // Input assembly: triangle list
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        ia.primitiveRestartEnable = VK_FALSE;
        pci.inputAssembly = ia;
        pci.inputAssemblyProvided = true;

        // Rasterization: no cull (safe for now; honors doubleSided by default)
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.depthClampEnable = VK_FALSE;
        rs.rasterizerDiscardEnable = VK_FALSE;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.lineWidth = 1.0f;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.depthBiasEnable = VK_FALSE;
        pci.rasterization = rs;
        pci.rasterizationProvided = true;

        // Dynamic viewport/scissor
        pci.dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

        // Depth/stencil (main render pass has a depth attachment)
        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS;
        ds.depthBoundsTestEnable = VK_FALSE;
        ds.stencilTestEnable = VK_FALSE;
        pci.depthStencil = ds;
        pci.depthStencilProvided = true;

        // Pipelines: OPAQUE / MASK / BLEND (mask currently uses same state as opaque)
        VkPipelineColorBlendAttachmentState attOpaque{};
        VkPipelineColorBlendStateCreateInfo cbOpaque = makeBlendState(false, attOpaque);
        pci.colorBlend = cbOpaque;
        pci.colorBlendProvided = true;
        VkResult r0 = m_pipelineOpaque.create(pci);

        VkPipelineColorBlendAttachmentState attMask{};
        VkPipelineColorBlendStateCreateInfo cbMask = makeBlendState(false, attMask);
        pci.colorBlend = cbMask;
        pci.colorBlendProvided = true;
        VkResult r1 = m_pipelineMask.create(pci);

        VkPipelineColorBlendAttachmentState attBlend{};
        VkPipelineColorBlendStateCreateInfo cbBlend = makeBlendState(true, attBlend);
        pci.colorBlend = cbBlend;
        pci.colorBlendProvided = true;

        // Transparent: test depth but don't write
        pci.depthStencil.depthWriteEnable = VK_FALSE;
        VkResult r2 = m_pipelineBlend.create(pci);

        // Cleanup shader modules
        vkDestroyShaderModule(pci.device, vert, nullptr);
        vkDestroyShaderModule(pci.device, frag, nullptr);

        if (r0 != VK_SUCCESS || r1 != VK_SUCCESS || r2 != VK_SUCCESS)
        {
            throw std::runtime_error("SModelRenderPassModule: failed to create one or more pipelines");
        }
    }

    void SModelRenderPassModule::record(FrameContext &frameCtx, VkCommandBuffer cmd)
    {
        if (!m_enabled)
            return;
        if (!m_assets || !m_model.isValid())
            return;
        if (m_extent.width == 0 || m_extent.height == 0)
            return;

        if (m_instanceCount == 0)
            return;

        ModelAsset *model = m_assets->getModel(m_model);
        if (!model || model->primitives.empty())
            return;

        VkViewport vp{0.0f, 0.0f, static_cast<float>(m_extent.width), static_cast<float>(m_extent.height), 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {m_extent.width, m_extent.height}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);

        const uint32_t camIndex = (!m_cameraFrames.empty()) ? (frameCtx.frameIndex % static_cast<uint32_t>(m_cameraFrames.size())) : 0;
        CameraFrame *camFrame = (!m_cameraFrames.empty()) ? &m_cameraFrames[camIndex] : nullptr;

        const uint32_t instIndex = (!m_instanceFrames.empty()) ? (frameCtx.frameIndex % static_cast<uint32_t>(m_instanceFrames.size())) : 0;
        InstanceFrame *instFrame = (!m_instanceFrames.empty()) ? &m_instanceFrames[instIndex] : nullptr;

        const uint32_t instanceCount = m_instanceCount;
        const uint32_t nodeCount = model->nodes.empty() ? 1u : static_cast<uint32_t>(model->nodes.size());
        const uint32_t jointStride = (model->totalJointCount > 0) ? model->totalJointCount : 1u;

        if (!camFrame || camFrame->set == VK_NULL_HANDLE)
            return;
        if (!instFrame || instFrame->buffer == VK_NULL_HANDLE)
            return;
        if (instFrame->capacity < instanceCount)
            return;
        if (camFrame->paletteCapacityMatrices < instanceCount * nodeCount)
            return;
        if (camFrame->jointPaletteCapacityMatrices < instanceCount * jointStride)
            return;

        // Pass ordering like glTF: 0=OPAQUE,1=MASK,2=BLEND
        for (uint32_t pass = 0; pass < 3; ++pass)
        {
            const Pipeline *pipe = nullptr;
            if (pass == 0)
                pipe = &m_pipelineOpaque;
            else if (pass == 1)
                pipe = &m_pipelineMask;
            else
                pipe = &m_pipelineBlend;

            pipe->bind(cmd);

            if (camFrame && camFrame->set != VK_NULL_HANDLE)
            {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &camFrame->set, 0, nullptr);
            }

            if (instFrame && instFrame->buffer != VK_NULL_HANDLE)
            {
                VkDeviceSize instOffset = 0;
                vkCmdBindVertexBuffers(cmd, 1, 1, &instFrame->buffer, &instOffset);
            }

            if (!model->nodes.empty())
            {
                // Draw by nodes: push base model matrix + node index; vertex shader fetches node matrix from palette
                const glm::mat4 baseM = glm::make_mat4(m_pc.model);
                for (uint32_t nodeIndex = 0; nodeIndex < static_cast<uint32_t>(model->nodes.size()); ++nodeIndex)
                {
                    const auto &node = model->nodes[nodeIndex];

                    for (uint32_t k = 0; k < node.primitiveCount; ++k)
                    {
                        const uint32_t primIndex = model->nodePrimitiveIndices[node.firstPrimitiveIndex + k];
                        if (primIndex >= model->primitives.size())
                            continue;
                        const ModelPrimitive &prim = model->primitives[primIndex];

                        MeshAsset *mesh = m_assets->getMesh(prim.mesh);
                        MaterialAsset *mat = m_assets->getMaterial(prim.material);
                        if (!mesh || !mat)
                            continue;
                        if (mat->alphaMode != pass)
                            continue;

                        VkDescriptorSet matSet = getOrCreateMaterialSet(prim.material, mat);
                        if (matSet != VK_NULL_HANDLE)
                        {
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 1, 1, &matSet, 0, nullptr);
                        }

                        PushConstantsModel pc{};
                        std::memcpy(pc.model, glm::value_ptr(baseM), sizeof(pc.model));
                        std::memcpy(pc.baseColorFactor, mat->baseColorFactor, sizeof(pc.baseColorFactor));
                        pc.materialParams[0] = mat->alphaCutoff;
                        pc.materialParams[1] = static_cast<float>(mat->alphaMode);
                        pc.materialParams[2] = 0.0f;
                        pc.materialParams[3] = 0.0f;
                        pc.nodeIndex = nodeIndex;
                        pc.nodeCount = static_cast<uint32_t>(model->nodes.size());

                        // Skinning per-primitive
                        pc.jointPaletteStride = jointStride;
                        if (prim.skinIndex >= 0 && static_cast<uint32_t>(prim.skinIndex) < model->skins.size())
                        {
                            const auto &skin = model->skins[static_cast<uint32_t>(prim.skinIndex)];
                            pc.skinBaseJoint = skin.jointBase;
                            pc.skinJointCount = skin.jointCount;
                        }
                        else
                        {
                            pc.skinBaseJoint = 0;
                            pc.skinJointCount = 0;
                        }
                        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstantsModel), &pc);

                        VkBuffer vb = mesh->getVertexBuffer();
                        VkBuffer ib = mesh->getIndexBuffer();
                        if (vb == VK_NULL_HANDLE || ib == VK_NULL_HANDLE || prim.indexCount == 0)
                            continue;

                        VkDeviceSize vbOffset = 0;
                        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &vbOffset);
                        vkCmdBindIndexBuffer(cmd, ib, 0, mesh->getIndexType());

                        vkCmdDrawIndexed(cmd, prim.indexCount, instanceCount, prim.firstIndex, prim.vertexOffset, 0);
                        DrawCallCounter::increment();
                    }
                }
            }
            else
            {
                // Fallback: draw all primitives with base model matrix
                const glm::mat4 baseM = glm::make_mat4(m_pc.model);
                for (const ModelPrimitive &prim : model->primitives)
                {
                    MeshAsset *mesh = m_assets->getMesh(prim.mesh);
                    MaterialAsset *mat = m_assets->getMaterial(prim.material);
                    if (!mesh || !mat)
                        continue;
                    if (mat->alphaMode != pass)
                        continue;

                    VkDescriptorSet matSet = getOrCreateMaterialSet(prim.material, mat);
                    if (matSet != VK_NULL_HANDLE)
                    {
                        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 1, 1, &matSet, 0, nullptr);
                    }

                    PushConstantsModel pc{};
                    std::memcpy(pc.model, glm::value_ptr(baseM), sizeof(pc.model));
                    std::memcpy(pc.baseColorFactor, mat->baseColorFactor, sizeof(pc.baseColorFactor));
                    pc.materialParams[0] = mat->alphaCutoff;
                    pc.materialParams[1] = static_cast<float>(mat->alphaMode);
                    pc.materialParams[2] = 0.0f;
                    pc.materialParams[3] = 0.0f;
                    pc.nodeIndex = 0;
                    pc.nodeCount = 1;

                    pc.jointPaletteStride = jointStride;
                    if (prim.skinIndex >= 0 && static_cast<uint32_t>(prim.skinIndex) < model->skins.size())
                    {
                        const auto &skin = model->skins[static_cast<uint32_t>(prim.skinIndex)];
                        pc.skinBaseJoint = skin.jointBase;
                        pc.skinJointCount = skin.jointCount;
                    }
                    else
                    {
                        pc.skinBaseJoint = 0;
                        pc.skinJointCount = 0;
                    }
                    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstantsModel), &pc);

                    VkBuffer vb = mesh->getVertexBuffer();
                    VkBuffer ib = mesh->getIndexBuffer();
                    if (vb == VK_NULL_HANDLE || ib == VK_NULL_HANDLE || prim.indexCount == 0)
                        continue;

                    VkDeviceSize vbOffset = 0;
                    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &vbOffset);
                    vkCmdBindIndexBuffer(cmd, ib, 0, mesh->getIndexType());

                    vkCmdDrawIndexed(cmd, prim.indexCount, instanceCount, prim.firstIndex, prim.vertexOffset, 0);
                    DrawCallCounter::increment();
                }
            }
        }
    }

    void SModelRenderPassModule::onResize(VulkanContext &ctx, VkExtent2D newExtent)
    {
        (void)ctx;
        m_extent = newExtent;
    }

    void SModelRenderPassModule::destroyResources()
    {
        if (m_device == VK_NULL_HANDLE)
            return;

        destroyCameraResources();
        destroyInstanceResources();
        destroyMaterialResources();

        m_pipelineOpaque.destroy(m_device);
        m_pipelineMask.destroy(m_device);
        m_pipelineBlend.destroy(m_device);

        if (m_pipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
            m_pipelineLayout = VK_NULL_HANDLE;
        }

        if (m_cameraSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(m_device, m_cameraSetLayout, nullptr);
            m_cameraSetLayout = VK_NULL_HANDLE;
        }
    }

    void SModelRenderPassModule::onDestroy(VulkanContext &ctx)
    {
        (void)ctx;
        destroyResources();
    }

} // namespace Engine
