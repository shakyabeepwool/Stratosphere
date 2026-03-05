#include "assets/AssetManager.h"
#include "utils/ImageUtils.h" // UploadContext

// Needed for glm/gtx/* headers (matrix_decompose)
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <utility>
#include <cstring>
#include <algorithm>
#include <unordered_set>
#include <functional>
#include <iostream>

#include <fstream>
#include <iterator>

const float TARGET = 10.0f; // Target size of models after scaling
namespace Engine
{
    static ModelAsset::NodeTRS DecomposeTRS(const glm::mat4 &m)
    {
        ModelAsset::NodeTRS out{};
        glm::vec3 skew;
        glm::vec4 perspective;
        glm::decompose(m, out.s, out.r, out.t, skew, perspective);
        out.r = glm::normalize(out.r);
        return out;
    }

    // ------------------------------------------------------------
    // Helpers: map smodel enum ints -> Vulkan settings
    // ------------------------------------------------------------
    static VkSamplerAddressMode toVkWrap(uint32_t wrap)
    {
        // Your .smodel uses: 0=Repeat,1=Clamp,2=Mirror
        switch (wrap)
        {
        default:
        case 0:
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 1:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 2:
            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        }
    }

    static VkFilter toVkFilter(uint32_t f)
    {
        // 0=Nearest,1=Linear
        switch (f)
        {
        default:
        case 0:
            return VK_FILTER_NEAREST;
        case 1:
            return VK_FILTER_LINEAR;
        }
    }

    static VkSamplerMipmapMode toVkMip(uint32_t m)
    {
        // 0=None,1=Nearest,2=Linear
        switch (m)
        {
        default:
        case 0:
            return VK_SAMPLER_MIPMAP_MODE_NEAREST; // no mip levels anyway (phase 1)
        case 1:
            return VK_SAMPLER_MIPMAP_MODE_NEAREST;
        case 2:
            return VK_SAMPLER_MIPMAP_MODE_LINEAR;
        }
    }

    // ------------------------------------------------------------
    // AssetManager
    // ------------------------------------------------------------
    AssetManager::AssetManager(VkDevice device,
                               VkPhysicalDevice phys,
                               VkQueue graphicsQueue,
                               uint32_t graphicsQueueFamilyIndex)
        : m_device(device),
          m_phys(phys),
          m_graphicsQueue(graphicsQueue),
          m_graphicsQueueFamilyIndex(graphicsQueueFamilyIndex)
    {
    }

    AssetManager::~AssetManager()
    {
        // Destroy meshes
        for (auto &kv : m_meshes)
        {
            if (kv.second.asset)
                kv.second.asset->destroy(m_device);
        }

        // Destroy textures
        for (auto &kv : m_textures)
        {
            if (kv.second.asset)
                kv.second.asset->destroy(m_device);
        }

        // Materials + Models are CPU only (no gpu destroy needed)
        m_meshes.clear();
        m_textures.clear();
        m_materials.clear();
        m_models.clear();

        m_meshPathCache.clear();
        m_modelPathCache.clear();
    }

    // ------------------------------------------------------------
    // Mesh existing API
    // ------------------------------------------------------------
    MeshHandle AssetManager::loadMesh(const std::string &cookedMeshPath)
    {
        auto it = m_meshPathCache.find(cookedMeshPath);
        if (it != m_meshPathCache.end())
        {
            addRef(it->second);
            return it->second;
        }

        MeshData data;
        if (!LoadSMeshV0FromFile(cookedMeshPath, data))
            return MeshHandle{};

        MeshHandle h = createMeshFromData_Internal(data, cookedMeshPath, 1);
        if (h.isValid())
            m_meshPathCache.emplace(cookedMeshPath, h);

        return h;
    }

    MeshAsset *AssetManager::getMesh(MeshHandle h)
    {
        auto it = m_meshes.find(h.id);
        if (it == m_meshes.end())
            return nullptr;
        if (it->second.generation != h.generation)
            return nullptr;
        return it->second.asset.get();
    }

    void AssetManager::addRef(MeshHandle h)
    {
        auto it = m_meshes.find(h.id);
        if (it != m_meshes.end() && it->second.generation == h.generation)
            it->second.refCount++;
    }

    void AssetManager::release(MeshHandle h)
    {
        auto it = m_meshes.find(h.id);
        if (it != m_meshes.end() && it->second.generation == h.generation)
        {
            if (it->second.refCount > 0)
                it->second.refCount--;
        }
    }

    MeshHandle AssetManager::createMeshFromData_Internal(const MeshData &data, const std::string &path, uint32_t initialRef)
    {
        // Transient pool per mesh upload
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = m_graphicsQueueFamilyIndex;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

        VkCommandPool uploadPool = VK_NULL_HANDLE;
        VkResult pr = vkCreateCommandPool(m_device, &poolInfo, nullptr, &uploadPool);
        if (pr != VK_SUCCESS)
            return MeshHandle{};

        auto asset = std::make_unique<MeshAsset>();
        const bool ok = asset->upload(m_device, m_phys, uploadPool, m_graphicsQueue, data);

        vkDestroyCommandPool(m_device, uploadPool, nullptr);

        if (!ok)
            return MeshHandle{};

        const uint64_t id = m_nextMeshID++;
        MeshEntry entry;
        entry.asset = std::move(asset);
        entry.generation = 1;
        entry.refCount = initialRef;
        entry.path = path;

        m_meshes.emplace(id, std::move(entry));

        MeshHandle h;
        h.id = id;
        h.generation = 1;
        return h;
    }

    // ------------------------------------------------------------
    // Texture API
    // ------------------------------------------------------------
    TextureHandle AssetManager::createTexture_Internal(std::unique_ptr<TextureAsset> tex, uint32_t initialRef)
    {
        const uint64_t id = m_nextTextureID++;
        TextureEntry e;
        e.asset = std::move(tex);
        e.generation = 1;
        e.refCount = initialRef;

        m_textures.emplace(id, std::move(e));

        TextureHandle h;
        h.id = id;
        h.generation = 1;
        return h;
    }

    TextureAsset *AssetManager::getTexture(TextureHandle h)
    {
        auto it = m_textures.find(h.id);
        if (it == m_textures.end())
            return nullptr;
        if (it->second.generation != h.generation)
            return nullptr;
        return it->second.asset.get();
    }

    Engine::TextureHandle AssetManager::loadTextureFromFile(const std::string &filePath)
    {
        // Read file contents into a vector<uint8_t>
        std::ifstream file(filePath, std::ios::binary | std::ios::ate);
        if (!file)
            return TextureHandle{};

        const std::streamsize size = file.tellg();
        if (size <= 0)
            return TextureHandle{};

        file.seekg(0, std::ios::beg);
        std::vector<uint8_t> bytes;
        bytes.resize(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char *>(bytes.data()), size))
            return TextureHandle{};

        // Create upload pool for single texture (similar to loadModel)
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = m_graphicsQueueFamilyIndex;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

        VkCommandPool uploadPool = VK_NULL_HANDLE;
        VkResult pr = vkCreateCommandPool(m_device, &poolInfo, nullptr, &uploadPool);
        if (pr != VK_SUCCESS)
            return TextureHandle{};

        Engine::UploadContext upload{};
        if (!Engine::BeginUploadContext(upload, m_device, m_phys, uploadPool, m_graphicsQueue))
        {
            vkDestroyCommandPool(m_device, uploadPool, nullptr);
            return TextureHandle{};
        }

        auto tex = std::make_unique<TextureAsset>();

        // Default sampler params (tweak if you want)
        const bool isSRGB = false;
        const VkSamplerAddressMode wrapU = toVkWrap(0); // Repeat
        const VkSamplerAddressMode wrapV = toVkWrap(0);
        const VkFilter minF = toVkFilter(1);         // Linear
        const VkFilter magF = toVkFilter(1);         // Linear
        const VkSamplerMipmapMode mipM = toVkMip(2); // Linear mipmap
        const float maxAnisotropy = 1.0f;

        if (!tex->uploadEncodedImage_Deferred(
                upload,
                bytes.data(),
                bytes.size(),
                isSRGB,
                wrapU,
                wrapV,
                minF,
                magF,
                mipM,
                maxAnisotropy))
        {
            Engine::EndSubmitAndWait(upload);
            vkDestroyCommandPool(m_device, uploadPool, nullptr);
            return TextureHandle{};
        }

        // Submit and wait (similar to loadModel)
        if (!Engine::EndSubmitAndWait(upload))
        {
            vkDestroyCommandPool(m_device, uploadPool, nullptr);
            return TextureHandle{};
        }

        vkDestroyCommandPool(m_device, uploadPool, nullptr);

        // Create a texture entry with refCount = 1 (caller gets an owned handle)
        TextureHandle th = createTexture_Internal(std::move(tex), 1);
        return th;
    }

    void AssetManager::addRef(TextureHandle h)
    {
        auto it = m_textures.find(h.id);
        if (it != m_textures.end() && it->second.generation == h.generation)
            it->second.refCount++;
    }

    void AssetManager::release(TextureHandle h)
    {
        auto it = m_textures.find(h.id);
        if (it != m_textures.end() && it->second.generation == h.generation)
        {
            if (it->second.refCount > 0)
                it->second.refCount--;
        }
    }

    // ------------------------------------------------------------
    // Material API
    // ------------------------------------------------------------
    MaterialHandle AssetManager::createMaterial_Internal(std::unique_ptr<MaterialAsset> mat, uint32_t initialRef)
    {
        const uint64_t id = m_nextMaterialID++;
        MaterialEntry e;
        e.asset = std::move(mat);
        e.generation = 1;
        e.refCount = initialRef;

        // Gather dependency handles (textures)
        if (e.asset)
        {
            if (e.asset->baseColorTexture.isValid())
                e.textureDeps.push_back(e.asset->baseColorTexture);
            if (e.asset->normalTexture.isValid())
                e.textureDeps.push_back(e.asset->normalTexture);
            if (e.asset->metallicRoughnessTexture.isValid())
                e.textureDeps.push_back(e.asset->metallicRoughnessTexture);
            if (e.asset->occlusionTexture.isValid())
                e.textureDeps.push_back(e.asset->occlusionTexture);
            if (e.asset->emissiveTexture.isValid())
                e.textureDeps.push_back(e.asset->emissiveTexture);
        }

        m_materials.emplace(id, std::move(e));

        MaterialHandle h;
        h.id = id;
        h.generation = 1;
        return h;
    }

    MaterialAsset *AssetManager::getMaterial(MaterialHandle h)
    {
        auto it = m_materials.find(h.id);
        if (it == m_materials.end())
            return nullptr;
        if (it->second.generation != h.generation)
            return nullptr;
        return it->second.asset.get();
    }

    void AssetManager::addRef(MaterialHandle h)
    {
        auto it = m_materials.find(h.id);
        if (it != m_materials.end() && it->second.generation == h.generation)
            it->second.refCount++;
    }

    void AssetManager::release(MaterialHandle h)
    {
        auto it = m_materials.find(h.id);
        if (it != m_materials.end() && it->second.generation == h.generation)
        {
            if (it->second.refCount > 0)
                it->second.refCount--;
        }
    }

    // ------------------------------------------------------------
    // Model API
    // ------------------------------------------------------------
    ModelHandle AssetManager::createModel_Internal(std::unique_ptr<ModelAsset> model, const std::string &path, uint32_t initialRef)
    {
        const uint64_t id = m_nextModelID++;
        ModelEntry e;
        e.asset = std::move(model);
        e.generation = 1;
        e.refCount = initialRef;
        e.path = path;

        // Dependencies (fill later in loadModel)
        m_models.emplace(id, std::move(e));

        ModelHandle h;
        h.id = id;
        h.generation = 1;
        return h;
    }

    ModelHandle AssetManager::loadModel(const std::string &cookedModelPath)
    {
        auto it = m_modelPathCache.find(cookedModelPath);
        if (it != m_modelPathCache.end())
        {
            addRef(it->second);
            return it->second;
        }

        // --------------------------
        // Parse cooked .smodel file
        // --------------------------
        Engine::smodel::SModelFileView view;
        std::string err;
        if (!Engine::smodel::LoadSModelFile(cookedModelPath, view, err))
        {
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
            std::cerr << "[AssetManager] loadModel: Failed to load .smodel: " << err << "\n";
#endif
            return ModelHandle{};
        }

        // --------------------------
        // Create upload pool for all textures (single submit)
        // --------------------------
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = m_graphicsQueueFamilyIndex;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

        VkCommandPool uploadPool = VK_NULL_HANDLE;
        VkResult pr = vkCreateCommandPool(m_device, &poolInfo, nullptr, &uploadPool);
        if (pr != VK_SUCCESS)
            return ModelHandle{};

        Engine::UploadContext upload{};
        if (!Engine::BeginUploadContext(upload, m_device, m_phys, uploadPool, m_graphicsQueue))
        {
            vkDestroyCommandPool(m_device, uploadPool, nullptr);
            return ModelHandle{};
        }

        // --------------------------
        // Upload textures (deferred)
        // --------------------------
        std::vector<TextureHandle> textureHandles;
        textureHandles.resize(view.textureCount());

        for (uint32_t i = 0; i < view.textureCount(); i++)
        {
            const auto &t = view.textures[i];

            // These fields come from your .smodel texture record format
            const bool isSRGB = (t.colorSpace == 1); // 1 = SRGB
            const VkSamplerAddressMode wrapU = toVkWrap(t.wrapU);
            const VkSamplerAddressMode wrapV = toVkWrap(t.wrapV);
            const VkFilter minF = toVkFilter(t.minFilter);
            const VkFilter magF = toVkFilter(t.magFilter);
            const VkSamplerMipmapMode mipM = toVkMip(t.mipFilter);

            const uint8_t *bytes = view.blob + t.imageDataOffset;
            const size_t sizeBytes = static_cast<size_t>(t.imageDataSize);

            auto tex = std::make_unique<TextureAsset>();

            // IMPORTANT:
            // We create textures with refCount=0 (materials will addRef them)
            // This avoids leaking textures when model is destroyed.
            if (!tex->uploadEncodedImage_Deferred(
                    upload,
                    bytes,
                    sizeBytes,
                    isSRGB,
                    wrapU,
                    wrapV,
                    minF,
                    magF,
                    mipM,
                    t.maxAnisotropy))
            {
                // Cleanup on failure
                Engine::EndSubmitAndWait(upload);
                vkDestroyCommandPool(m_device, uploadPool, nullptr);
                return ModelHandle{};
            }

            textureHandles[i] = createTexture_Internal(std::move(tex), 0);
        }

        // ONE SUBMIT for all textures
        if (!Engine::EndSubmitAndWait(upload))
        {
            vkDestroyCommandPool(m_device, uploadPool, nullptr);
            return ModelHandle{};
        }

        vkDestroyCommandPool(m_device, uploadPool, nullptr);

        // --------------------------
        // Create materials (CPU only)
        // Materials addRef() to textures they use
        // --------------------------
        std::vector<MaterialHandle> materialHandles;
        materialHandles.resize(view.materialCount());

        for (uint32_t i = 0; i < view.materialCount(); i++)
        {
            const auto &m = view.materials[i];

            auto mat = std::make_unique<MaterialAsset>();
            mat->debugName = view.getStringOrEmpty(m.nameStrOffset);

            std::memcpy(mat->baseColorFactor, m.baseColorFactor, sizeof(mat->baseColorFactor));
            std::memcpy(mat->emissiveFactor, m.emissiveFactor, sizeof(mat->emissiveFactor));

            mat->metallicFactor = m.metallicFactor;
            mat->roughnessFactor = m.roughnessFactor;

            mat->normalScale = m.normalScale;
            mat->occlusionStrength = m.occlusionStrength;
            mat->alphaCutoff = m.alphaCutoff;

            mat->alphaMode = m.alphaMode;
            mat->doubleSided = m.doubleSided;

            // Convert texture indices -> handles (-1 means none)
            auto grabTex = [&](int32_t idx) -> TextureHandle
            {
                if (idx < 0)
                    return TextureHandle{};
                return textureHandles[static_cast<uint32_t>(idx)];
            };

            mat->baseColorTexture = grabTex(m.baseColorTexture);
            mat->normalTexture = grabTex(m.normalTexture);
            mat->metallicRoughnessTexture = grabTex(m.metallicRoughnessTexture);
            mat->occlusionTexture = grabTex(m.occlusionTexture);
            mat->emissiveTexture = grabTex(m.emissiveTexture);

            mat->baseColorTexCoord = m.baseColorTexCoord;
            mat->normalTexCoord = m.normalTexCoord;
            mat->metallicRoughnessTexCoord = m.metallicRoughnessTexCoord;
            mat->occlusionTexCoord = m.occlusionTexCoord;
            mat->emissiveTexCoord = m.emissiveTexCoord;

            // Create material with refCount=0 (model will addRef materials it uses)
            MaterialHandle mh = createMaterial_Internal(std::move(mat), 0);
            materialHandles[i] = mh;

            // AddRef textures used by this material
            auto *createdMat = getMaterial(mh);
            if (createdMat)
            {
                if (createdMat->baseColorTexture.isValid())
                    addRef(createdMat->baseColorTexture);
                if (createdMat->normalTexture.isValid())
                    addRef(createdMat->normalTexture);
                if (createdMat->metallicRoughnessTexture.isValid())
                    addRef(createdMat->metallicRoughnessTexture);
                if (createdMat->occlusionTexture.isValid())
                    addRef(createdMat->occlusionTexture);
                if (createdMat->emissiveTexture.isValid())
                    addRef(createdMat->emissiveTexture);
            }
        }

        // --------------------------
        // Create meshes (GPU upload)
        // Model will addRef() meshes it uses
        // --------------------------
        std::vector<MeshHandle> meshHandles;
        meshHandles.resize(view.meshCount());

        for (uint32_t i = 0; i < view.meshCount(); i++)
        {
            const auto &mr = view.meshes[i];

            MeshData md;
            md.vertexCount = mr.vertexCount;
            md.indexCount = mr.indexCount;
            md.vertexStride = mr.vertexStride;
            md.indexFormat = (mr.indexType == 0) ? 0 : 1;

            std::memcpy(md.aabbMin, mr.aabbMin, sizeof(md.aabbMin));
            std::memcpy(md.aabbMax, mr.aabbMax, sizeof(md.aabbMax));

            // Copy vertex bytes from blob
            const uint8_t *vb = view.blob + mr.vertexDataOffset;
            md.vertexBytes.assign(vb, vb + mr.vertexDataSize);

            // Copy index bytes
            const uint8_t *ib = view.blob + mr.indexDataOffset;

            if (md.indexFormat == 0)
            {
                md.indices16.resize(md.indexCount);
                std::memcpy(md.indices16.data(), ib, md.indexCount * sizeof(uint16_t));
            }
            else
            {
                md.indices32.resize(md.indexCount);
                std::memcpy(md.indices32.data(), ib, md.indexCount * sizeof(uint32_t));
            }

            // Create mesh with refCount=0 (model will addRef as needed)
            meshHandles[i] = createMeshFromData_Internal(md, cookedModelPath + "#mesh" + std::to_string(i), 0);
        }

        // --------------------------
        // Create model primitives
        // Model addsRef() to mesh/material dependencies
        // --------------------------
        auto model = std::make_unique<ModelAsset>();

        model->debugName = ""; // optional: you can store filename later

        // Initialize bounds as invalid until we see a mesh
        model->hasBounds = false;
        model->fitScale = 1.0f;
        model->center[0] = model->center[1] = model->center[2] = 0.0f;
        model->boundsMin[0] = model->boundsMin[1] = model->boundsMin[2] = 0.0f;
        model->boundsMax[0] = model->boundsMax[1] = model->boundsMax[2] = 0.0f;

        model->primitives.resize(view.primitiveCount());

        std::vector<MeshHandle> meshDeps;
        std::vector<MaterialHandle> matDeps;

        std::unordered_set<uint64_t> meshDepIds;
        std::unordered_set<uint64_t> matDepIds;
        meshDepIds.reserve(static_cast<size_t>(view.primitiveCount()));
        matDepIds.reserve(static_cast<size_t>(view.primitiveCount()));

        for (uint32_t i = 0; i < view.primitiveCount(); i++)
        {
            const auto &p = view.primitives[i];

            ModelPrimitive prim;
            prim.mesh = meshHandles[p.meshIndex];
            prim.material = materialHandles[p.materialIndex];
            prim.firstIndex = p.firstIndex;
            prim.indexCount = p.indexCount;
            prim.vertexOffset = p.vertexOffset;
            prim.skinIndex = p.skinIndex;

            model->primitives[i] = prim;

            // Dependency refs:
            if (prim.mesh.isValid())
            {
                // AddRef once per unique dependency (avoid duplicates in dep lists)
                if (meshDepIds.insert(prim.mesh.id).second)
                {
                    addRef(prim.mesh);
                    meshDeps.push_back(prim.mesh);
                }

                // Expand model bounds from mesh bounds
                MeshAsset *mesh = getMesh(prim.mesh);
                if (mesh)
                {
                    const float *mn = mesh->getAABBMin();
                    const float *mx = mesh->getAABBMax();

                    if (!model->hasBounds)
                    {
                        std::memcpy(model->boundsMin, mn, sizeof(model->boundsMin));
                        std::memcpy(model->boundsMax, mx, sizeof(model->boundsMax));
                        model->hasBounds = true;
                    }
                    else
                    {
                        model->boundsMin[0] = std::min(model->boundsMin[0], mn[0]);
                        model->boundsMin[1] = std::min(model->boundsMin[1], mn[1]);
                        model->boundsMin[2] = std::min(model->boundsMin[2], mn[2]);

                        model->boundsMax[0] = std::max(model->boundsMax[0], mx[0]);
                        model->boundsMax[1] = std::max(model->boundsMax[1], mx[1]);
                        model->boundsMax[2] = std::max(model->boundsMax[2], mx[2]);
                    }
                }
            }
            if (prim.material.isValid())
            {
                // AddRef once per unique dependency (avoid duplicates in dep lists)
                if (matDepIds.insert(prim.material.id).second)
                {
                    addRef(prim.material);
                    matDeps.push_back(prim.material);
                }
            }
        }

        // --------------------------
        // V4: Populate skin tables (optional)
        // --------------------------
        if (view.skinCount() > 0)
        {
            model->skins.resize(view.skinCount());
            model->totalJointCount = 0;

            for (uint32_t si = 0; si < view.skinCount(); ++si)
            {
                const auto &sr = view.skins[si];
                ModelAsset::ModelSkin skin{};
                skin.debugName = view.getStringOrEmpty(sr.nameStrOffset);
                skin.jointBase = model->totalJointCount;
                skin.jointCount = sr.jointCount;

                // Validate and copy joint node indices slice
                if (sr.jointCount > 0)
                {
                    if (sr.firstJointNodeIndex + sr.jointCount > view.skinJointNodeIndicesCount())
                    {
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
                        std::cout << "[AssetManager] loadModel: Skin jointNodeIndices out of range (skinIndex=" << si << ")\n";
#endif
                        return ModelHandle{};
                    }

                    const uint32_t *srcJ = view.skinJointNodeIndices + sr.firstJointNodeIndex;
                    skin.jointNodeIndices.assign(srcJ, srcJ + sr.jointCount);

                    // Validate inverse bind matrices
                    const uint64_t neededFloats = uint64_t(sr.jointCount) * 16ull;
                    if (uint64_t(sr.firstInverseBindMatrix) + neededFloats > view.skinInverseBindMatricesCount())
                    {
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
                        std::cout << "[AssetManager] loadModel: Skin inverseBindMatrices out of range (skinIndex=" << si << ")\n";
#endif
                        return ModelHandle{};
                    }

                    skin.inverseBind.resize(sr.jointCount);
                    const float *srcM = view.skinInverseBindMatrices + sr.firstInverseBindMatrix;
                    for (uint32_t j = 0; j < sr.jointCount; ++j)
                    {
                        std::memcpy(glm::value_ptr(skin.inverseBind[j]), srcM + size_t(j) * 16u, sizeof(float) * 16u);
                    }
                }

                model->skins[si] = std::move(skin);
                model->totalJointCount += sr.jointCount;
            }
        }
        else
        {
            model->skins.clear();
            model->totalJointCount = 0;
        }

        // Precompute center and fit scale to 20 units if bounds are valid
        if (model->hasBounds)
        {
            model->center[0] = 0.5f * (model->boundsMin[0] + model->boundsMax[0]);
            model->center[1] = 0.5f * (model->boundsMin[1] + model->boundsMax[1]);
            model->center[2] = 0.5f * (model->boundsMin[2] + model->boundsMax[2]);

            const float sizeX = model->boundsMax[0] - model->boundsMin[0];
            const float sizeY = model->boundsMax[1] - model->boundsMin[1];
            const float sizeZ = model->boundsMax[2] - model->boundsMin[2];
            const float maxExtent = std::max(sizeX, std::max(sizeY, sizeZ));

            const float target = TARGET;
            const float epsilon = 1e-4f;
            if (maxExtent > epsilon)
                model->fitScale = target / maxExtent;
            else
                model->fitScale = 4.0f;
        }

        // --------------------------
        // V2: Populate nodes and primitive index mapping
        // --------------------------
        if (view.nodeCount() > 0)
        {
            model->nodes.resize(view.nodeCount());
            model->nodePrimitiveIndices.resize(view.nodePrimitiveIndexCount());
            model->nodeChildIndices.resize(view.nodeChildIndexCount());

            // Copy primitive indices array
            if (view.nodePrimitiveIndexCount() > 0 && view.nodePrimitiveIndices)
            {
                std::memcpy(model->nodePrimitiveIndices.data(), view.nodePrimitiveIndices, sizeof(uint32_t) * view.nodePrimitiveIndexCount());
            }

            // Copy child indices array
            if (view.nodeChildIndexCount() > 0 && view.nodeChildIndices)
            {
                std::memcpy(model->nodeChildIndices.data(), view.nodeChildIndices, sizeof(uint32_t) * view.nodeChildIndexCount());
            }

            // Track first root (parentIndex == UINT32_MAX)
            uint32_t rootIdx = 0;
            const uint32_t U32_MAX = ~0u;

            for (uint32_t i = 0; i < view.nodeCount(); ++i)
            {
                const Engine::smodel::SModelNodeRecord &nr = view.nodes[i];
                ModelAsset::ModelNode &dst = model->nodes[i];

                dst.parentIndex = nr.parentIndex;
                dst.firstChildIndex = nr.childCount ? nr.firstChildIndex : U32_MAX;
                dst.childCount = nr.childCount;
                dst.firstPrimitiveIndex = nr.firstPrimitiveIndex;
                dst.primitiveCount = nr.primitiveCount;
                dst.debugName = view.getStringOrEmpty(nr.nameStrOffset);

                // Copy local matrix (column-major)
                std::memcpy(glm::value_ptr(dst.localMatrix), nr.localMatrix, sizeof(nr.localMatrix));

                // Defer global computation; ordering is not guaranteed.
                dst.globalMatrix = glm::mat4(1.0f);

                if (nr.parentIndex == U32_MAX)
                    rootIdx = i;
            }

            model->rootNodeIndex = rootIdx;

            // Compute globals using explicit child lists (supports any node ordering).
            {
                const uint32_t nodeCount = static_cast<uint32_t>(model->nodes.size());
                std::vector<uint8_t> visited(nodeCount, 0);

                std::function<void(uint32_t, const glm::mat4 &)> compute = [&](uint32_t nodeIdx, const glm::mat4 &parentGlobal)
                {
                    if (nodeIdx >= nodeCount)
                        return;
                    if (visited[nodeIdx])
                        return;
                    visited[nodeIdx] = 1;

                    ModelAsset::ModelNode &n = model->nodes[nodeIdx];
                    n.globalMatrix = parentGlobal * n.localMatrix;

                    if (n.childCount == 0)
                        return;
                    if (n.firstChildIndex == U32_MAX)
                        return;

                    const uint32_t start = n.firstChildIndex;
                    for (uint32_t ci = 0; ci < n.childCount; ++ci)
                    {
                        const uint32_t child = model->nodeChildIndices[start + ci];
                        compute(child, n.globalMatrix);
                    }
                };

                for (uint32_t i = 0; i < nodeCount; ++i)
                {
                    if (model->nodes[i].parentIndex == U32_MAX)
                        compute(i, glm::mat4(1.0f));
                }
            }

            // Recompute bounds in node-global space (node transforms applied)
            bool firstCorner = true;
            glm::vec3 bmin(0.0f);
            glm::vec3 bmax(0.0f);

            for (const auto &node : model->nodes)
            {
                if (node.primitiveCount == 0)
                    continue;

                for (uint32_t k = 0; k < node.primitiveCount; ++k)
                {
                    const uint32_t primIndex = model->nodePrimitiveIndices[node.firstPrimitiveIndex + k];
                    if (primIndex >= model->primitives.size())
                        continue;

                    const ModelPrimitive &prim = model->primitives[primIndex];
                    MeshAsset *mesh = getMesh(prim.mesh);
                    if (!mesh)
                        continue;

                    const float *mn = mesh->getAABBMin();
                    const float *mx = mesh->getAABBMax();

                    const glm::vec3 c0(mn[0], mn[1], mn[2]);
                    const glm::vec3 c1(mx[0], mn[1], mn[2]);
                    const glm::vec3 c2(mn[0], mx[1], mn[2]);
                    const glm::vec3 c3(mx[0], mx[1], mn[2]);
                    const glm::vec3 c4(mn[0], mn[1], mx[2]);
                    const glm::vec3 c5(mx[0], mn[1], mx[2]);
                    const glm::vec3 c6(mn[0], mx[1], mx[2]);
                    const glm::vec3 c7(mx[0], mx[1], mx[2]);

                    const glm::vec3 corners[8] = {c0, c1, c2, c3, c4, c5, c6, c7};
                    for (const glm::vec3 &corner : corners)
                    {
                        const glm::vec4 w = node.globalMatrix * glm::vec4(corner, 1.0f);
                        const glm::vec3 p(w.x, w.y, w.z);
                        if (firstCorner)
                        {
                            bmin = p;
                            bmax = p;
                            firstCorner = false;
                        }
                        else
                        {
                            bmin.x = std::min(bmin.x, p.x);
                            bmin.y = std::min(bmin.y, p.y);
                            bmin.z = std::min(bmin.z, p.z);
                            bmax.x = std::max(bmax.x, p.x);
                            bmax.y = std::max(bmax.y, p.y);
                            bmax.z = std::max(bmax.z, p.z);
                        }
                    }
                }
            }

            if (!firstCorner)
            {
                model->boundsMin[0] = bmin.x;
                model->boundsMin[1] = bmin.y;
                model->boundsMin[2] = bmin.z;
                model->boundsMax[0] = bmax.x;
                model->boundsMax[1] = bmax.y;
                model->boundsMax[2] = bmax.z;
                model->hasBounds = true;

                model->center[0] = 0.5f * (model->boundsMin[0] + model->boundsMax[0]);
                model->center[1] = 0.5f * (model->boundsMin[1] + model->boundsMax[1]);
                model->center[2] = 0.5f * (model->boundsMin[2] + model->boundsMax[2]);

                const float sizeX = model->boundsMax[0] - model->boundsMin[0];
                const float sizeY = model->boundsMax[1] - model->boundsMin[1];
                const float sizeZ = model->boundsMax[2] - model->boundsMin[2];
                const float maxExtent = std::max(sizeX, std::max(sizeY, sizeZ));

                const float target = TARGET;
                const float epsilon = 1e-4f;
                model->fitScale = (maxExtent > epsilon) ? (target / maxExtent) : 1.0f;
            }
        }

        // --------------------------
        // V3: Copy animations into ModelAsset (node TRS only)
        // --------------------------
        if (view.animClipCount() > 0)
        {
            model->animClips.resize(view.animClipCount());
            std::memcpy(model->animClips.data(), view.animClips, sizeof(smodel::SModelAnimationClipRecord) * view.animClipCount());
        }
        if (view.animChannelCount() > 0)
        {
            model->animChannels.resize(view.animChannelCount());
            std::memcpy(model->animChannels.data(), view.animChannels, sizeof(smodel::SModelAnimationChannelRecord) * view.animChannelCount());
        }
        if (view.animSamplerCount() > 0)
        {
            model->animSamplers.resize(view.animSamplerCount());
            std::memcpy(model->animSamplers.data(), view.animSamplers, sizeof(smodel::SModelAnimationSamplerRecord) * view.animSamplerCount());
        }
        if (view.animTimesCount() > 0)
        {
            model->animTimes.resize(view.animTimesCount());
            std::memcpy(model->animTimes.data(), view.animTimes, sizeof(float) * view.animTimesCount());
        }
        if (view.animValuesCount() > 0)
        {
            model->animValues.resize(view.animValuesCount());
            std::memcpy(model->animValues.data(), view.animValues, sizeof(float) * view.animValuesCount());
        }

        // --------------------------
        // Initialize runtime animation TRS buffers from node local matrices
        // --------------------------
        model->restTRS.resize(model->nodes.size());
        model->animatedTRS.resize(model->nodes.size());
        for (size_t i = 0; i < model->nodes.size(); i++)
        {
            const glm::mat4 local = model->nodes[i].localMatrix;
            model->restTRS[i] = DecomposeTRS(local);
            model->animatedTRS[i] = model->restTRS[i];
        }

        model->animState.clipIndex = 0;
        model->animState.timeSec = 0.0f;
        model->animState.loop = true;
        model->animState.playing = true;

        // Register model and cache it
        ModelHandle modelHandle = createModel_Internal(std::move(model), cookedModelPath, 1);

        // Fill dependency lists inside the ModelEntry
        auto modelIt = m_models.find(modelHandle.id);
        if (modelIt != m_models.end())
        {
            modelIt->second.meshDeps = std::move(meshDeps);
            modelIt->second.materialDeps = std::move(matDeps);
        }

        m_modelPathCache.emplace(cookedModelPath, modelHandle);
        return modelHandle;
    }

    ModelAsset *AssetManager::getModel(ModelHandle h)
    {
        auto it = m_models.find(h.id);
        if (it == m_models.end())
            return nullptr;
        if (it->second.generation != h.generation)
            return nullptr;
        return it->second.asset.get();
    }

    void AssetManager::addRef(ModelHandle h)
    {
        auto it = m_models.find(h.id);
        if (it != m_models.end() && it->second.generation == h.generation)
            it->second.refCount++;
    }

    void AssetManager::release(ModelHandle h)
    {
        auto it = m_models.find(h.id);
        if (it != m_models.end() && it->second.generation == h.generation)
        {
            if (it->second.refCount > 0)
                it->second.refCount--;
        }
    }

    // ------------------------------------------------------------
    // Garbage collection with dependency release
    // ------------------------------------------------------------
    void AssetManager::garbageCollect()
    {
        // 1) Destroy models with refCount == 0
        for (auto it = m_models.begin(); it != m_models.end();)
        {
            if (it->second.refCount == 0)
            {
                // Release model deps
                for (auto &mh : it->second.meshDeps)
                    release(mh);
                for (auto &mat : it->second.materialDeps)
                    release(mat);

                m_modelPathCache.erase(it->second.path);
                it = m_models.erase(it);
            }
            else
            {
                ++it;
            }
        }

        // 2) Destroy materials with refCount == 0
        for (auto it = m_materials.begin(); it != m_materials.end();)
        {
            if (it->second.refCount == 0)
            {
                // Release textures referenced by this material
                for (auto &th : it->second.textureDeps)
                    release(th);
                it = m_materials.erase(it);
            }
            else
            {
                ++it;
            }
        }

        // 3) Destroy meshes with refCount == 0
        for (auto it = m_meshes.begin(); it != m_meshes.end();)
        {
            if (it->second.refCount == 0)
            {
                if (it->second.asset)
                    it->second.asset->destroy(m_device);

                m_meshPathCache.erase(it->second.path);
                it = m_meshes.erase(it);
            }
            else
            {
                ++it;
            }
        }

        // 4) Destroy textures with refCount == 0
        for (auto it = m_textures.begin(); it != m_textures.end();)
        {
            if (it->second.refCount == 0)
            {
                if (it->second.asset)
                    it->second.asset->destroy(m_device);
                it = m_textures.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

} // namespace Engine