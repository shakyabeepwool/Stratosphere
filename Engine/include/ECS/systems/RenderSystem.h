#pragma once

#include "ECS/SystemFormat.h"

#include "assets/AssetManager.h"

#include "Engine/Camera.h"
#include "Engine/Renderer.h"
#include "Engine/SModelRenderPassModule.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <iostream>

class RenderSystem : public Engine::ECS::SystemBase
{
public:
    explicit RenderSystem(Engine::AssetManager *assets = nullptr)
        : m_assets(assets)
    {
        // Render-side caches rely on RenderTransform for stable versioning.
        setRequiredNames({"RenderModel", "PosePalette", "RenderTransform"});
        setExcludedNames({"Disabled", "Dead"});
    }

    const char *name() const override { return "RenderModelSystem"; }
    void setAssetManager(Engine::AssetManager *assets) { m_assets = assets; }

    void setRenderer(Engine::Renderer *renderer) { m_renderer = renderer; }
    void setCamera(Engine::Camera *camera) { m_camera = camera; }

    void update(Engine::ECS::ECSContext &ecs, float dt) override
    {
        (void)dt;
        if (!m_assets || !m_renderer || !m_camera)
            return;

        // We write into per-frame persistently-mapped GPU buffers during update().
        // Ensure the current frame slot is no longer in use by the GPU.
        if (!m_renderer->waitForCurrentFrameFence())
            return;

        ++m_frameCounter;

        auto keyFromHandle = [](const Engine::ModelHandle &h) -> uint64_t
        {
            return (static_cast<uint64_t>(h.generation) << 32) | static_cast<uint64_t>(h.id);
        };

        auto entityKeyFromHandle = [](const Engine::ECS::Entity &e) -> uint64_t
        {
            return (static_cast<uint64_t>(e.generation) << 32) | static_cast<uint64_t>(e.index);
        };

        auto markDirtyOnce = [](std::vector<uint8_t> &dirtyFlags, std::vector<uint32_t> &dirtySlots, uint32_t slot)
        {
            if (slot >= dirtyFlags.size())
                return;
            if (dirtyFlags[slot])
                return;
            dirtyFlags[slot] = 1u;
            dirtySlots.push_back(slot);
        };

        auto resetDirtyLists = [](std::vector<uint8_t> &dirtyFlags, std::vector<uint32_t> &dirtySlots)
        {
            for (uint32_t slot : dirtySlots)
            {
                if (slot < dirtyFlags.size())
                    dirtyFlags[slot] = 0u;
            }
            dirtySlots.clear();
        };

        if (m_queryId == Engine::ECS::QueryManager::InvalidQuery)
            m_queryId = ecs.queries.createQuery(required(), excluded(), ecs.stores);

        const auto &q = ecs.queries.get(m_queryId);
        for (uint32_t archetypeId : q.matchingArchetypeIds)
        {
            auto *ptr = ecs.stores.get(archetypeId);
            if (!ptr)
                continue;

            auto &store = *ptr;
            if (!store.signature().containsAll(required()))
                continue;
            if (!store.signature().containsNone(excluded()))
                continue;
            if (!store.hasRenderModel())
                continue;
            if (!store.hasPosePalette())
                continue;
            if (!store.hasRenderTransform())
                continue;

            auto &renderModels = store.renderModels();
            auto &renderTransforms = store.renderTransforms();
            auto &posePalettes = store.posePalettes();
            const auto &entities = store.entities();
            const uint32_t n = store.size();

            for (uint32_t row = 0; row < n; ++row)
            {
                const Engine::ModelHandle handle = renderModels[row].handle;
                Engine::ModelAsset *asset = m_assets->getModel(handle);
                if (!asset)
                    continue;

                const uint64_t key = keyFromHandle(handle);

                const uint32_t nodeCount = asset->nodes.empty() ? 1u : static_cast<uint32_t>(asset->nodes.size());
                const uint32_t jointStride = (asset->totalJointCount > 0) ? asset->totalJointCount : 1u;

                auto &cache = m_batches[key];
                cache.modelHandle = handle;

                // Start-of-frame dirty list reset for this cache.
                if (cache.lastDirtyResetFrame != m_frameCounter)
                {
                    resetDirtyLists(cache.dirtyWorldFlags, cache.dirtyWorldSlots);
                    resetDirtyLists(cache.dirtyPoseFlags, cache.dirtyPoseSlots);
                    cache.lastDirtyResetFrame = m_frameCounter;
                }

                // Initialize or handle asset layout changes.
                if (cache.nodeCount == 0)
                {
                    cache.nodeCount = nodeCount;
                    cache.jointStride = jointStride;
                }
                else if (cache.nodeCount != nodeCount || cache.jointStride != jointStride)
                {
                    // Model layout changed (hot reload). Drop cached slots and force re-upload.
                    cache.clear();
                    cache.nodeCount = nodeCount;
                    cache.jointStride = jointStride;
                    cache.modelHandle = handle;
                }

                const Engine::ECS::Entity e = (row < entities.size()) ? entities[row] : Engine::ECS::Entity{};
                if (!e.valid())
                    continue;
                const uint64_t ekey = entityKeyFromHandle(e);

                auto slotIt = cache.entityToSlot.find(ekey);
                uint32_t slot = 0;
                const bool isNew = (slotIt == cache.entityToSlot.end());

                if (isNew)
                {
                    slot = static_cast<uint32_t>(cache.slotToEntity.size());
                    cache.entityToSlot.emplace(ekey, slot);
                    cache.slotToEntity.emplace_back(e);

                    cache.lastSeenFrame.emplace_back(m_frameCounter);

                    cache.lastUploadedPoseVersion.emplace_back(std::numeric_limits<uint32_t>::max());
                    cache.lastUploadedTransformVersion.emplace_back(std::numeric_limits<uint32_t>::max());

                    cache.cpuInstanceWorlds.emplace_back(glm::mat4(1.0f));

                    cache.dirtyWorldFlags.emplace_back(0u);
                    cache.dirtyPoseFlags.emplace_back(0u);

                    // Grow palettes by exactly one slot.
                    cache.cpuNodePalette.resize(static_cast<size_t>(cache.slotToEntity.size()) * cache.nodeCount, glm::mat4(1.0f));
                    cache.cpuJointPalette.resize(static_cast<size_t>(cache.slotToEntity.size()) * cache.jointStride, glm::mat4(1.0f));
                }
                else
                {
                    slot = slotIt->second;
                    if (slot >= cache.slotToEntity.size())
                        continue;
                    cache.lastSeenFrame[slot] = m_frameCounter;
                }

                // World transform (RenderTransform component).
                const auto &rt = renderTransforms[row];
                if (isNew || rt.transformVersion != cache.lastUploadedTransformVersion[slot])
                {
                    cache.cpuInstanceWorlds[slot] = rt.world;
                    markDirtyOnce(cache.dirtyWorldFlags, cache.dirtyWorldSlots, slot);
                    cache.lastUploadedTransformVersion[slot] = rt.transformVersion;
                }

                // Pose palettes (PosePalette component).
                const auto &pose = posePalettes[row];
                if (isNew || pose.poseVersion != cache.lastUploadedPoseVersion[slot])
                {
                    const size_t nodeBase = static_cast<size_t>(slot) * static_cast<size_t>(cache.nodeCount);
                    if (pose.nodeCount == cache.nodeCount && pose.nodePalette.size() == static_cast<size_t>(cache.nodeCount))
                    {
                        std::copy(pose.nodePalette.begin(), pose.nodePalette.end(), cache.cpuNodePalette.begin() + nodeBase);
                    }
                    else
                    {
                        std::fill(cache.cpuNodePalette.begin() + nodeBase, cache.cpuNodePalette.begin() + nodeBase + cache.nodeCount, glm::mat4(1.0f));
                    }

                    const size_t jointBase = static_cast<size_t>(slot) * static_cast<size_t>(cache.jointStride);
                    if (pose.jointCount == cache.jointStride && pose.jointPalette.size() == static_cast<size_t>(cache.jointStride))
                    {
                        std::copy(pose.jointPalette.begin(), pose.jointPalette.end(), cache.cpuJointPalette.begin() + jointBase);
                    }
                    else
                    {
                        std::fill(cache.cpuJointPalette.begin() + jointBase, cache.cpuJointPalette.begin() + jointBase + cache.jointStride, glm::mat4(1.0f));
                    }

                    markDirtyOnce(cache.dirtyPoseFlags, cache.dirtyPoseSlots, slot);
                    cache.lastUploadedPoseVersion[slot] = pose.poseVersion;
                }
            }
        }

        // Remove stale entities from caches (swap-remove) and upload partial updates.
        const uint32_t frameIndex = m_renderer->getCurrentFrameIndex();
        const uint32_t frameCount = std::max(1u, m_renderer->getMaxFramesInFlight());
        const uint32_t allFramesMask = (frameCount >= 32u) ? 0xFFFFFFFFu : ((1u << frameCount) - 1u);
        const uint32_t frameBit = (frameIndex < 32u) ? (1u << frameIndex) : 0u;
        for (auto &kv : m_batches)
        {
            const uint64_t key = kv.first;
            auto &cache = kv.second;

            // Purge stale slots.
            for (int32_t slot = static_cast<int32_t>(cache.slotToEntity.size()) - 1; slot >= 0; --slot)
            {
                const uint32_t uslot = static_cast<uint32_t>(slot);
                if (uslot >= cache.lastSeenFrame.size())
                    continue;
                if (cache.lastSeenFrame[uslot] == m_frameCounter)
                    continue;
                cache.removeSlotSwap(uslot);
            }

            const uint32_t instanceCount = static_cast<uint32_t>(cache.slotToEntity.size());

            auto passIt = m_passes.find(key);
            if (instanceCount == 0)
            {
                if (passIt != m_passes.end())
                    passIt->second->setEnabled(false);
                continue;
            }

            if (passIt == m_passes.end())
            {
                auto pass = std::make_shared<Engine::SModelRenderPassModule>();
                pass->setAssets(m_assets);
                pass->setModel(cache.modelHandle);
                pass->setCamera(m_camera);
                pass->setEnabled(true);
                m_renderer->registerPass(pass);
                passIt = m_passes.emplace(key, std::move(pass)).first;
            }

            auto &pass = *passIt->second;
            pass.setCamera(m_camera);
            pass.setEnabled(true);

            if (!cache.printedStats)
            {
                Engine::ModelAsset *model = m_assets->getModel(cache.modelHandle);
                if (model)
                {
                    uint64_t indicesPerInstance = 0;
                    for (const auto &prim : model->primitives)
                        indicesPerInstance += prim.indexCount;
                    const uint64_t trisPerInstance = indicesPerInstance / 3ull;
                    const uint64_t estTris = trisPerInstance * static_cast<uint64_t>(instanceCount);
                    std::cout << "[RenderStats] modelId=" << cache.modelHandle.id
                              << " gen=" << cache.modelHandle.generation
                              << " primitives=" << model->primitives.size()
                              << " nodes=" << model->nodes.size()
                              << " joints=" << model->totalJointCount
                              << " instances=" << instanceCount
                              << " indices/inst=" << indicesPerInstance
                              << " estTris/frame=" << estTris
                              << "\n";
                }
                cache.printedStats = true;
            }

            // If swapchain/resources were recreated, force full upload once.
            const uint32_t passGen = pass.getResourceGeneration();
            const bool forceFull = (cache.lastSeenPassGeneration != passGen);
            cache.lastSeenPassGeneration = passGen;

            // Any swapchain/resource recreate means all frame slots need a refresh.
            if (forceFull)
            {
                cache.pendingWorldFrameMask = allFramesMask;
                cache.pendingPoseFrameMask = allFramesMask;
            }

            // If instance count changed, world+pose buffers effectively shift.
            if (cache.lastUploadedInstanceCount != instanceCount)
            {
                cache.lastUploadedInstanceCount = instanceCount;
                cache.pendingWorldFrameMask = allFramesMask;
                cache.pendingPoseFrameMask = allFramesMask;
            }

            cache.buildDirtyRanges(cache.worldRangesScratch, cache.dirtyWorldSlots);
            cache.buildDirtyRanges(cache.poseRangesScratch, cache.dirtyPoseSlots);

            // If we have new dirty data, every frame slot must receive it at least once.
            if (!cache.worldRangesScratch.empty())
                cache.pendingWorldFrameMask = allFramesMask;
            if (!cache.poseRangesScratch.empty())
                cache.pendingPoseFrameMask = allFramesMask;

            // If this frame slot is pending but there are no dirty ranges for that stream,
            // force a full upload to bring the slot up to date.
            const bool worldCatchUpNeedsFull = ((cache.pendingWorldFrameMask & frameBit) != 0u) && cache.worldRangesScratch.empty();
            const bool poseCatchUpNeedsFull = ((cache.pendingPoseFrameMask & frameBit) != 0u) && cache.poseRangesScratch.empty();
            const bool forceForThisFrame = forceFull || worldCatchUpNeedsFull || poseCatchUpNeedsFull;

            pass.setInstanceCount(instanceCount);
            pass.prepareFrameUploads(
                frameIndex,
                cache.cpuInstanceWorlds.data(), instanceCount,
                cache.cpuNodePalette.data(), cache.nodeCount,
                cache.cpuJointPalette.data(), cache.jointStride,
                cache.worldRangesScratch.data(), static_cast<uint32_t>(cache.worldRangesScratch.size()),
                cache.poseRangesScratch.data(), static_cast<uint32_t>(cache.poseRangesScratch.size()),
                forceForThisFrame);

            // This frame slot is now up-to-date.
            if (frameBit != 0u)
            {
                cache.pendingWorldFrameMask &= ~frameBit;
                cache.pendingPoseFrameMask &= ~frameBit;
            }
        }
    }

private:
    Engine::AssetManager *m_assets = nullptr; // not owned
    Engine::Renderer *m_renderer = nullptr;   // not owned
    Engine::Camera *m_camera = nullptr;       // not owned

    using UploadRange = Engine::SModelRenderPassModule::UploadRange;

    struct PerModelBatchCache
    {
        Engine::ModelHandle modelHandle{};

        uint32_t nodeCount = 0;
        uint32_t jointStride = 0;

        std::vector<Engine::ECS::Entity> slotToEntity;
        std::unordered_map<uint64_t, uint32_t> entityToSlot;

        std::vector<glm::mat4> cpuInstanceWorlds;
        std::vector<glm::mat4> cpuNodePalette;
        std::vector<glm::mat4> cpuJointPalette;

        std::vector<uint32_t> lastSeenFrame;

        std::vector<uint32_t> lastUploadedPoseVersion;
        std::vector<uint32_t> lastUploadedTransformVersion;

        std::vector<uint8_t> dirtyWorldFlags;
        std::vector<uint8_t> dirtyPoseFlags;
        std::vector<uint32_t> dirtyWorldSlots;
        std::vector<uint32_t> dirtyPoseSlots;

        uint32_t lastDirtyResetFrame = 0;
        uint32_t lastSeenPassGeneration = 0;

        bool printedStats = false;

        // When data changes, all per-frame slots must be updated at least once.
        uint32_t pendingWorldFrameMask = 0;
        uint32_t pendingPoseFrameMask = 0;
        uint32_t lastUploadedInstanceCount = 0;

        std::vector<UploadRange> worldRangesScratch;
        std::vector<UploadRange> poseRangesScratch;

        void clear()
        {
            slotToEntity.clear();
            entityToSlot.clear();
            cpuInstanceWorlds.clear();
            cpuNodePalette.clear();
            cpuJointPalette.clear();
            lastSeenFrame.clear();
            lastUploadedPoseVersion.clear();
            lastUploadedTransformVersion.clear();
            dirtyWorldFlags.clear();
            dirtyPoseFlags.clear();
            dirtyWorldSlots.clear();
            dirtyPoseSlots.clear();
            worldRangesScratch.clear();
            poseRangesScratch.clear();
            lastDirtyResetFrame = 0;
            printedStats = false;
        }

        void removeSlotSwap(uint32_t slot)
        {
            const uint32_t last = static_cast<uint32_t>(slotToEntity.size() - 1);
            const uint32_t slotCount = static_cast<uint32_t>(slotToEntity.size());
            if (slot >= slotCount)
                return;

            const auto entityKeyFromHandle = [](const Engine::ECS::Entity &e) -> uint64_t
            {
                return (static_cast<uint64_t>(e.generation) << 32) | static_cast<uint64_t>(e.index);
            };

            auto swapErase = [&](auto &vec)
            {
                if (slot != last)
                    vec[slot] = std::move(vec[last]);
                vec.pop_back();
            };

            // Update mapping for swapped entity.
            if (slot != last)
            {
                const Engine::ECS::Entity movedEnt = slotToEntity[last];
                entityToSlot[entityKeyFromHandle(movedEnt)] = slot;

                // Copy palette slices for moved slot to its new index.
                const size_t nodeDst = static_cast<size_t>(slot) * nodeCount;
                const size_t nodeSrc = static_cast<size_t>(last) * nodeCount;
                std::copy(cpuNodePalette.begin() + nodeSrc, cpuNodePalette.begin() + nodeSrc + nodeCount, cpuNodePalette.begin() + nodeDst);

                const size_t jointDst = static_cast<size_t>(slot) * jointStride;
                const size_t jointSrc = static_cast<size_t>(last) * jointStride;
                std::copy(cpuJointPalette.begin() + jointSrc, cpuJointPalette.begin() + jointSrc + jointStride, cpuJointPalette.begin() + jointDst);
            }

            // Erase entity mapping for removed entity.
            const uint64_t removedKey = entityKeyFromHandle(slotToEntity[slot]);
            entityToSlot.erase(removedKey);

            swapErase(slotToEntity);
            swapErase(cpuInstanceWorlds);
            swapErase(lastSeenFrame);
            swapErase(lastUploadedPoseVersion);
            swapErase(lastUploadedTransformVersion);
            swapErase(dirtyWorldFlags);
            swapErase(dirtyPoseFlags);

            // Shrink palettes by one slot.
            cpuNodePalette.resize(static_cast<size_t>(slotToEntity.size()) * nodeCount);
            cpuJointPalette.resize(static_cast<size_t>(slotToEntity.size()) * jointStride);

            // Conservatively mark the slot we wrote into (swap) as dirty so GPU reflects the new contents.
            if (slot < dirtyWorldFlags.size())
            {
                dirtyWorldFlags[slot] = 1u;
                dirtyWorldSlots.push_back(slot);
            }
            if (slot < dirtyPoseFlags.size())
            {
                dirtyPoseFlags[slot] = 1u;
                dirtyPoseSlots.push_back(slot);
            }
        }

        void buildDirtyRanges(std::vector<UploadRange> &outRanges, std::vector<uint32_t> &dirtySlots)
        {
            outRanges.clear();
            if (dirtySlots.empty())
                return;
            std::sort(dirtySlots.begin(), dirtySlots.end());
            dirtySlots.erase(std::unique(dirtySlots.begin(), dirtySlots.end()), dirtySlots.end());

            uint32_t runStart = dirtySlots[0];
            uint32_t prev = dirtySlots[0];
            for (size_t i = 1; i < dirtySlots.size(); ++i)
            {
                const uint32_t cur = dirtySlots[i];
                if (cur == prev + 1u)
                {
                    prev = cur;
                    continue;
                }
                outRanges.push_back(UploadRange{runStart, (prev - runStart) + 1u});
                runStart = prev = cur;
            }
            outRanges.push_back(UploadRange{runStart, (prev - runStart) + 1u});
        }
    };

    std::unordered_map<uint64_t, PerModelBatchCache> m_batches;
    std::unordered_map<uint64_t, std::shared_ptr<Engine::SModelRenderPassModule>> m_passes;
    Engine::ECS::QueryId m_queryId = Engine::ECS::QueryManager::InvalidQuery;

    uint32_t m_frameCounter = 0;
};
