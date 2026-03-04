#pragma once
/*
  PrefabSpawner.h
  ---------------
  Purpose:
    - Provide a function to spawn an entity from a Prefab:
      * Create an Entity (EntitiesRecord)
      * Create/get the matching ArchetypeStore
      * Create a row in the store, apply defaults
      * Attach the entity mapping for quick lookup

  Usage:
    - Prefer: SpawnResult res = spawnFromPrefab(prefab, ecs);
      (This marks the new row dirty for dirty-enabled queries so pose/world caches initialize.)
*/

#include "ECS/Prefab.h"
#include "ECS/ArchetypeManager.h"
#include "ECS/ArchetypeStore.h"
#include "ECS/Entity.h"
#include "ECS/Components.h"
#include "ECS/ECSContext.h"

namespace Engine::ECS
{
  // Result of spawning: includes entity handle, row index, and archetypeId.
  struct SpawnResult
  {
    Entity entity{};
    uint32_t row = UINT32_MAX;
    uint32_t archetypeId = UINT32_MAX;
  };

  namespace detail
  {
    inline SpawnResult spawnFromPrefabImpl(const Prefab &prefab,
                                           ComponentRegistry &registry,
                                           ArchetypeManager &archetypes,
                                           ArchetypeStoreManager &stores,
                                           EntitiesRecord &entities)
    {
      (void)archetypes;

      SpawnResult res{};
      res.archetypeId = prefab.archetypeId;

      // Create entity
      res.entity = entities.create();

      // Get or create store for this archetype signature
      ArchetypeStore *store = stores.getOrCreate(prefab.archetypeId, prefab.signature, registry);

      // Create row and apply defaults
      res.row = store->createRow(res.entity);
      store->applyDefaults(res.row, prefab.defaults, registry);

      // Attach entity to record for quick per-entity operations
      entities.attach(res.entity, res.archetypeId, res.row);

      return res;
    }
  } // namespace detail

  // WARNING: This overload does NOT mark the spawned row dirty for dirty-enabled queries.
  // Prefer the ECSContext overload unless you are intentionally managing dirty/initialization yourself.
  [[deprecated("Prefer spawnFromPrefab(prefab, ecs) so dirty-driven systems initialize newly spawned entities")]]
  inline SpawnResult spawnFromPrefab(const Prefab &prefab,
                                     ComponentRegistry &registry,
                                     ArchetypeManager &archetypes,
                                     ArchetypeStoreManager &stores,
                                     EntitiesRecord &entities)
  {
    return detail::spawnFromPrefabImpl(prefab, registry, archetypes, stores, entities);
  }

  // ECSContext overload: additionally marks the spawned row dirty for any dirty-enabled queries.
  inline SpawnResult spawnFromPrefab(const Prefab &prefab, ECSContext &ecs)
  {
    SpawnResult res = detail::spawnFromPrefabImpl(prefab, ecs.components, ecs.archetypes, ecs.stores, ecs.entities);
    ArchetypeStore *store = ecs.stores.get(res.archetypeId);
    if (store)
      ecs.queries.markRowDirtyAll(res.archetypeId, res.row, store->size());
    return res;
  }

} // namespace Engine::ECS