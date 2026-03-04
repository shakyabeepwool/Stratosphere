#pragma once

#include "ECS/SystemFormat.h"
#include "assets/AssetManager.h"

#include <algorithm>
#include <cstdint>
#include <vector>

class PoseUpdateSystem : public Engine::ECS::SystemBase
{
public:
    PoseUpdateSystem()
    {
        setRequiredNames({"RenderModel", "RenderAnimation", "PosePalette"});
        setExcludedNames({"Disabled", "Dead"});
    }

    const char *name() const override { return "PoseUpdateSystem"; }

    void setAssetManager(Engine::AssetManager *assets) { m_assets = assets; }

    void buildMasks(Engine::ECS::ComponentRegistry &registry) override
    {
        Engine::ECS::SystemBase::buildMasks(registry);
        m_renderAnimId = registry.ensureId("RenderAnimation");
        m_renderModelId = registry.ensureId("RenderModel");
    }

    void update(Engine::ECS::ECSContext &ecs, float /*dt*/) override
    {
        if (!m_assets)
            return;

        ++m_frameCounter;

        if (m_queryId == Engine::ECS::QueryManager::InvalidQuery)
        {
            Engine::ECS::ComponentMask dirty;
            dirty.set(m_renderAnimId);
            dirty.set(m_renderModelId);
            m_queryId = ecs.queries.createDirtyQuery(required(), excluded(), dirty, ecs.stores);
        }

        const auto &q = ecs.queries.get(m_queryId);
        for (uint32_t archetypeId : q.matchingArchetypeIds)
        {
            Engine::ECS::ArchetypeStore *store = ecs.stores.get(archetypeId);
            if (!store)
                continue;
            if (!store->hasRenderModel() || !store->hasRenderAnimation() || !store->hasPosePalette())
                continue;

            auto dirtyRows = ecs.queries.consumeDirtyRows(m_queryId, archetypeId);
            if (dirtyRows.empty())
                continue;

            auto &renderModels = store->renderModels();
            auto &renderAnimations = store->renderAnimations();
            auto &posePalettes = store->posePalettes();

            for (uint32_t row : dirtyRows)
            {
                if (row >= store->size())
                    continue;

                const Engine::ModelHandle handle = renderModels[row].handle;
                Engine::ModelAsset *asset = m_assets->getModel(handle);
                auto &out = posePalettes[row];

                // Any write to the palette counts as a new version.
                out.poseVersion += 1u;
                out.lastUpdatedFrame = m_frameCounter;

                if (!asset || asset->nodes.empty())
                {
                    out.nodePalette.clear();
                    out.jointPalette.clear();
                    out.nodeCount = 0;
                    out.jointCount = 0;
                    continue;
                }

                const auto &anim = renderAnimations[row];
                const uint32_t safeClip = (!asset->animClips.empty())
                                              ? std::min(anim.clipIndex, static_cast<uint32_t>(asset->animClips.size() - 1))
                                              : 0u;
                const float timeSec = (!asset->animClips.empty() && anim.playing) ? anim.timeSec : 0.0f;

                asset->evaluatePoseInto(safeClip, timeSec,
                                        m_trsScratch,
                                        m_localsScratch,
                                        m_globalsScratch,
                                        m_visitedScratch);

                out.nodeCount = static_cast<uint32_t>(asset->nodes.size());
                out.nodePalette = m_globalsScratch;

                out.jointCount = asset->totalJointCount;
                out.jointPalette.assign(out.jointCount, glm::mat4(1.0f));

                if (out.jointCount > 0 && m_globalsScratch.size() == out.nodeCount)
                {
                    for (const auto &skin : asset->skins)
                    {
                        if (skin.jointCount == 0)
                            continue;
                        for (uint32_t j = 0; j < skin.jointCount; ++j)
                        {
                            if (j >= skin.jointNodeIndices.size() || j >= skin.inverseBind.size())
                                continue;

                            const uint32_t nodeIx = skin.jointNodeIndices[j];
                            if (nodeIx >= m_globalsScratch.size())
                                continue;

                            const uint32_t outIx = skin.jointBase + j;
                            if (outIx >= out.jointPalette.size())
                                continue;

                            out.jointPalette[outIx] = m_globalsScratch[nodeIx] * skin.inverseBind[j];
                        }
                    }
                }
            }
        }
    }

private:
    Engine::AssetManager *m_assets = nullptr;

    Engine::ECS::QueryId m_queryId = Engine::ECS::QueryManager::InvalidQuery;
    uint32_t m_renderAnimId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_renderModelId = Engine::ECS::ComponentRegistry::InvalidID;

    std::vector<Engine::ModelAsset::NodeTRS> m_trsScratch;
    std::vector<glm::mat4> m_localsScratch;
    std::vector<glm::mat4> m_globalsScratch;
    std::vector<uint8_t> m_visitedScratch;

    uint32_t m_frameCounter = 0;
};
