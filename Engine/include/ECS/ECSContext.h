#pragma once
//
// ECSContext.h
// -------------
// Purpose:
//   - Centralize Engine-owned ECS managers under a single context object.
//   - Owned by Application; SampleApp accesses it via Application::GetECS().
//
// Managers included:
//   - ComponentRegistry: name <-> id mapping for components (data-driven).
//   - ArchetypeManager: archetype IDs keyed by component signature.
//   - ArchetypeStoreManager: lazily created SoA stores per archetype.
//   - EntitiesRecord: control-plane mapping of entity handle -> (archetypeId, row).
//   - PrefabManager: dictionary of prefabs keyed by name (SampleApp loads JSON and fills it).
//
// Notes:
//   - Engine/Application owns lifetime of ECSContext.
//   - SampleApp should perform game-specific configuration (ensure IDs, load JSON, spawn, systems).
//

#include "ECS/Components.h"       // ComponentRegistry, ComponentMask and components (Position/Velocity/Health)
#include "ECS/ArchetypeManager.h" // ArchetypeManager
#include "ECS/ArchetypeStore.h"   // ArchetypeStoreManager
#include "ECS/Entity.h"           // EntitiesRecord
#include "ECS/Prefab.h"           // PrefabManager
#include "ECS/QueryManager.h"     // QueryManager
#include <vector>

namespace Engine::ECS
{
    struct ECSContext
    {
        // Core managers
        ComponentRegistry components;
        ArchetypeManager archetypes;
        ArchetypeStoreManager stores;
        EntitiesRecord entities;
        PrefabManager prefabs;
        QueryManager queries;

        // Call once to keep QueryManager updated as new stores are created.
        void WireQueryManager()
        {
            stores.setOnStoreCreated([this](uint32_t archetypeId, const ComponentMask &signature)
                                     { this->queries.onStoreCreated(archetypeId, signature); });
        }

        // -------------------------
        // Entity operations
        // -------------------------
        // Move an entity to a different archetype signature.
        // Copies the intersection of known component arrays from the source row to the destination row.
        // Updates EntitiesRecord for this entity and for any entity swap-moved inside the source store.
        bool moveEntity(Entity e, const ComponentMask &newSignature)
        {
            EntityRecord *rec = entities.find(e);
            if (!rec)
                return false;

            const uint32_t srcArchetypeId = rec->archetypeId;
            ArchetypeStore *srcStore = stores.get(srcArchetypeId);
            if (!srcStore)
                return false;
            const uint32_t srcRow = rec->row;
            if (srcRow >= srcStore->size())
                return false;

            const Archetype *srcArch = archetypes.get(srcArchetypeId);
            const ComponentMask srcSignature = srcArch ? srcArch->signature : srcStore->signature();
            if (srcSignature.toKey() == newSignature.toKey())
                return true;

            const uint32_t dstArchetypeId = archetypes.getOrCreate(newSignature);
            ArchetypeStore *dstStore = stores.getOrCreate(dstArchetypeId, newSignature, components);
            if (!dstStore)
                return false;

            const uint32_t dstRow = dstStore->createRow(e);

            // Copy intersection components.
            if (srcStore->hasPosition() && dstStore->hasPosition())
                dstStore->positions()[dstRow] = srcStore->positions()[srcRow];
            if (srcStore->hasVelocity() && dstStore->hasVelocity())
                dstStore->velocities()[dstRow] = srcStore->velocities()[srcRow];
            if (srcStore->hasHealth() && dstStore->hasHealth())
                dstStore->healths()[dstRow] = srcStore->healths()[srcRow];
            if (srcStore->hasMoveTarget() && dstStore->hasMoveTarget())
                dstStore->moveTargets()[dstRow] = srcStore->moveTargets()[srcRow];
            if (srcStore->hasMoveSpeed() && dstStore->hasMoveSpeed())
                dstStore->moveSpeeds()[dstRow] = srcStore->moveSpeeds()[srcRow];
            if (srcStore->hasRadius() && dstStore->hasRadius())
                dstStore->radii()[dstRow] = srcStore->radii()[srcRow];
            if (srcStore->hasSeparation() && dstStore->hasSeparation())
                dstStore->separations()[dstRow] = srcStore->separations()[srcRow];
            if (srcStore->hasAvoidanceParams() && dstStore->hasAvoidanceParams())
                dstStore->avoidanceParams()[dstRow] = srcStore->avoidanceParams()[srcRow];
            if (srcStore->hasRenderModel() && dstStore->hasRenderModel())
                dstStore->renderModels()[dstRow] = srcStore->renderModels()[srcRow];
            if (srcStore->hasRenderAnimation() && dstStore->hasRenderAnimation())
                dstStore->renderAnimations()[dstRow] = srcStore->renderAnimations()[srcRow];
            if (srcStore->hasFacing() && dstStore->hasFacing())
                dstStore->facings()[dstRow] = srcStore->facings()[srcRow];
            if (srcStore->hasRenderTransform() && dstStore->hasRenderTransform())
                dstStore->renderTransforms()[dstRow] = srcStore->renderTransforms()[srcRow];
            if (srcStore->hasPosePalette() && dstStore->hasPosePalette())
                dstStore->posePalettes()[dstRow] = srcStore->posePalettes()[srcRow];

            // Update mapping for moved entity first (so the source destroy can't leave it stale).
            entities.attach(e, dstArchetypeId, dstRow);

            // Entity entered a new matching store: mark it dirty for any dirty queries.
            queries.markRowDirtyAll(dstArchetypeId, dstRow, dstStore->size());

            // Remove from source store (swap-remove) and fix mapping for the swap-moved entity.
            Entity swapped = srcStore->destroyRowSwap(srcRow);
            if (swapped.valid())
            {
                // swapped moved into srcRow in srcArchetypeId
                entities.attach(swapped, srcArchetypeId, srcRow);
                // Row content changed due to swap-move; conservatively mark as dirty.
                queries.markRowDirtyAll(srcArchetypeId, srcRow, srcStore->size());
            }

            return true;
        }

        // Mark dirty by explicit archetype+row.
        void markDirty(uint32_t compId, uint32_t archetypeId, uint32_t row)
        {
            ArchetypeStore *store = stores.get(archetypeId);
            if (!store)
                return;
            queries.markDirtyComponent(compId, archetypeId, row, store->size());
        }

        // Mark dirty by entity handle.
        void markDirty(uint32_t compId, Entity e)
        {
            const EntityRecord *rec = entities.find(e);
            if (!rec)
                return;
            markDirty(compId, rec->archetypeId, rec->row);
        }

        bool addTag(Entity e, uint32_t tagId)
        {
            const EntityRecord *rec = entities.find(e);
            if (!rec)
                return false;
            const Archetype *arch = archetypes.get(rec->archetypeId);
            ComponentMask sig = arch ? arch->signature : ComponentMask{};
            if (!arch)
                sig = stores.get(rec->archetypeId) ? stores.get(rec->archetypeId)->signature() : ComponentMask{};
            if (sig.has(tagId))
                return true;
            sig.set(tagId);
            return moveEntity(e, sig);
        }

        bool removeTag(Entity e, uint32_t tagId)
        {
            const EntityRecord *rec = entities.find(e);
            if (!rec)
                return false;
            const Archetype *arch = archetypes.get(rec->archetypeId);
            ComponentMask sig = arch ? arch->signature : ComponentMask{};
            if (!arch)
                sig = stores.get(rec->archetypeId) ? stores.get(rec->archetypeId)->signature() : ComponentMask{};
            if (!sig.has(tagId))
                return true;
            sig.clear(tagId);
            return moveEntity(e, sig);
        }

        // Set a tag exclusively on 'target': clears the tag from any other entity that currently has it,
        // then adds it to target.
        bool setTagExclusive(Entity target, uint32_t tagId)
        {
            std::vector<Entity> toClear;
            for (const auto &ptr : stores.stores())
            {
                if (!ptr)
                    continue;
                const ArchetypeStore &store = *ptr;
                if (!store.signature().has(tagId))
                    continue;
                const uint32_t n = store.size();
                const auto &ents = store.entities();
                for (uint32_t row = 0; row < n; ++row)
                {
                    const Entity e = ents[row];
                    if (target.valid() && e.index == target.index && e.generation == target.generation)
                        continue;
                    toClear.push_back(e);
                }
            }

            for (Entity e : toClear)
                (void)removeTag(e, tagId);

            return addTag(target, tagId);
        }

        // Optional helper to reset state (typically not needed except in tests/tools).
        void Reset()
        {
            // Recreate default-constructed managers.
            *this = ECSContext{};
        }
    };
}