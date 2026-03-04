#pragma once
/*
  ArchetypeStore.h
  ----------------
  Purpose:
    - Provide a generic Struct-of-Arrays store for a single archetype (signature).
    - Conditionally hold arrays for components present in the signature (Position, Velocity, Health).
    - Support creation of rows with defaults, destruction via swap-remove, and per-row masks.

  Usage:
    - Construct with a signature.
    - resolveKnownComponents(registry) to enable arrays for known components.
    - createRow(entity) then applyDefaults(row, defaults, registry).
    - destroyRow(row) with dense packing.
*/

#include <vector>
#include <unordered_map>
#include <memory>
#include <variant>
#include <limits>
#include <functional>
#include "ECS/Components.h"
#include "ECS/Entity.h"

namespace Engine::ECS
{
    class ArchetypeStore
    {
    public:
        explicit ArchetypeStore(const ComponentMask &signature)
            : m_signature(signature) {}

        // Create a new row for the given entity; returns row index.
        uint32_t createRow(Entity e)
        {
            const uint32_t row = static_cast<uint32_t>(m_entities.size());
            m_entities.emplace_back(e);

            // Conditionally create per-component entries based on signature bits.
            if (hasPosition())
                m_positions.emplace_back(Position{});
            if (hasVelocity())
                m_velocities.emplace_back(Velocity{});
            if (hasHealth())
                m_healths.emplace_back(Health{100.f});
            if (hasMoveTarget())
                m_moveTargets.emplace_back(MoveTarget{});
            if (hasMoveSpeed())
                m_moveSpeeds.emplace_back(MoveSpeed{});
            if (hasRadius())
                m_radii.emplace_back(Radius{});
            if (hasSeparation())
                m_separations.emplace_back(Separation{});
            if (hasAvoidanceParams())
                m_avoidanceParams.emplace_back(AvoidanceParams{});
            if (hasRenderModel())
                m_renderModels.emplace_back(RenderModel{});
            if (hasLocomotionClips())
                m_locomotionClips.emplace_back(LocomotionClips{});
            if (hasCombatClips())
                m_combatClips.emplace_back(CombatClips{});
            if (hasRenderAnimation())
                m_renderAnimations.emplace_back(RenderAnimation{});
            if (hasFacing())
                m_facings.emplace_back(Facing{});
            if (hasRenderTransform())
                m_renderTransforms.emplace_back(RenderTransform{});
            if (hasObstacleRadius())
                m_obstacleRadii.emplace_back(ObstacleRadius{});
            if (hasPath())
                m_paths.emplace_back(Path{});
            if (hasPosePalette())
                m_posePalettes.emplace_back(PosePalette{});
            if (hasTeam())
                m_teams.emplace_back(Team{});
            if (hasAttackCooldown())
                m_attackCooldowns.emplace_back(AttackCooldown{});

            return row;
        }

        // Swap-remove a row; maintains dense arrays.
        // Returns the entity that was moved into 'row' (the previous last entity) when row!=last.
        // Returns an invalid entity when the removed row was already the last row, or if row was out of range.
        Entity destroyRowSwap(uint32_t row)
        {
            if (m_entities.empty())
                return Entity{};
            const uint32_t last = static_cast<uint32_t>(m_entities.size() - 1);
            if (row > last)
                return Entity{};

            Entity moved{};
            if (row != last)
                moved = m_entities[last];

            auto swapErase = [&](auto &vec)
            {
                if (row != last)
                    vec[row] = std::move(vec[last]);
                vec.pop_back();
            };

            swapErase(m_entities);
            if (hasPosition())
                swapErase(m_positions);
            if (hasVelocity())
                swapErase(m_velocities);
            if (hasHealth())
                swapErase(m_healths);
            if (hasMoveTarget())
                swapErase(m_moveTargets);
            if (hasMoveSpeed())
                swapErase(m_moveSpeeds);
            if (hasRadius())
                swapErase(m_radii);
            if (hasSeparation())
                swapErase(m_separations);
            if (hasAvoidanceParams())
                swapErase(m_avoidanceParams);
            if (hasRenderModel())
                swapErase(m_renderModels);
            if (hasLocomotionClips())
                swapErase(m_locomotionClips);
            if (hasCombatClips())
                swapErase(m_combatClips);
            if (hasRenderAnimation())
                swapErase(m_renderAnimations);
            if (hasFacing())
                swapErase(m_facings);
            if (hasRenderTransform())
                swapErase(m_renderTransforms);
            if (hasObstacleRadius())
                swapErase(m_obstacleRadii);
            if (hasPath())
                swapErase(m_paths);
            if (hasPosePalette())
                swapErase(m_posePalettes);
            if (hasTeam())
                swapErase(m_teams);
            if (hasAttackCooldown())
                swapErase(m_attackCooldowns);

            return moved;
        }

        // Backwards-compatible API; does not report the moved entity.
        void destroyRow(uint32_t row)
        {
            (void)destroyRowSwap(row);
        }

        // Apply typed defaults for a newly created row.
        void applyDefaults(uint32_t row, const std::unordered_map<uint32_t, DefaultValue> &defaults,
                           const ComponentRegistry & /*registry*/)
        {
            for (const auto &kv : defaults)
            {
                const uint32_t cid = kv.first;
                if (!m_signature.has(cid))
                    continue;

                if (std::holds_alternative<Position>(kv.second) && hasPosition())
                {
                    m_positions[row] = std::get<Position>(kv.second);
                }
                else if (std::holds_alternative<Velocity>(kv.second) && hasVelocity())
                {
                    m_velocities[row] = std::get<Velocity>(kv.second);
                }
                else if (std::holds_alternative<Health>(kv.second) && hasHealth())
                {
                    m_healths[row] = std::get<Health>(kv.second);
                }
                else if (std::holds_alternative<MoveTarget>(kv.second) && hasMoveTarget())
                {
                    m_moveTargets[row] = std::get<MoveTarget>(kv.second);
                }
                else if (std::holds_alternative<MoveSpeed>(kv.second) && hasMoveSpeed())
                {
                    m_moveSpeeds[row] = std::get<MoveSpeed>(kv.second);
                }
                else if (std::holds_alternative<Radius>(kv.second) && hasRadius())
                {
                    m_radii[row] = std::get<Radius>(kv.second);
                }
                else if (std::holds_alternative<Separation>(kv.second) && hasSeparation())
                {
                    m_separations[row] = std::get<Separation>(kv.second);
                }
                else if (std::holds_alternative<AvoidanceParams>(kv.second) && hasAvoidanceParams())
                {
                    m_avoidanceParams[row] = std::get<AvoidanceParams>(kv.second);
                }
                else if (std::holds_alternative<RenderModel>(kv.second) && hasRenderModel())
                {
                    m_renderModels[row] = std::get<RenderModel>(kv.second);
                }
                else if (std::holds_alternative<LocomotionClips>(kv.second) && hasLocomotionClips())
                {
                    m_locomotionClips[row] = std::get<LocomotionClips>(kv.second);
                }
                else if (std::holds_alternative<CombatClips>(kv.second) && hasCombatClips())
                {
                    m_combatClips[row] = std::get<CombatClips>(kv.second);
                }
                else if (std::holds_alternative<RenderAnimation>(kv.second) && hasRenderAnimation())
                {
                    m_renderAnimations[row] = std::get<RenderAnimation>(kv.second);
                }
                else if (std::holds_alternative<Facing>(kv.second) && hasFacing())
                {
                    m_facings[row] = std::get<Facing>(kv.second);
                }
                else if (std::holds_alternative<RenderTransform>(kv.second) && hasRenderTransform())
                {
                    m_renderTransforms[row] = std::get<RenderTransform>(kv.second);
                }
                else if (std::holds_alternative<ObstacleRadius>(kv.second) && hasObstacleRadius())
                {
                    m_obstacleRadii[row] = std::get<ObstacleRadius>(kv.second);
                }
                else if (std::holds_alternative<Path>(kv.second) && hasPath())
                {
                    m_paths[row] = std::get<Path>(kv.second);
                }
                else if (std::holds_alternative<PosePalette>(kv.second) && hasPosePalette())
                {
                    m_posePalettes[row] = std::get<PosePalette>(kv.second);
                }
                else if (std::holds_alternative<Team>(kv.second) && hasTeam())
                {
                    m_teams[row] = std::get<Team>(kv.second);
                }
                else if (std::holds_alternative<AttackCooldown>(kv.second) && hasAttackCooldown())
                {
                    m_attackCooldowns[row] = std::get<AttackCooldown>(kv.second);
                }
            }
        }

        // Accessors
        const ComponentMask &signature() const { return m_signature; }
        uint32_t size() const { return static_cast<uint32_t>(m_entities.size()); }

        const std::vector<Entity> &entities() const { return m_entities; }

        // Component arrays (conditionally enabled).
        std::vector<Position> &positions() { return m_positions; }
        const std::vector<Position> &positions() const { return m_positions; }

        std::vector<Velocity> &velocities() { return m_velocities; }
        const std::vector<Velocity> &velocities() const { return m_velocities; }

        std::vector<Health> &healths() { return m_healths; }
        const std::vector<Health> &healths() const { return m_healths; }

        std::vector<MoveTarget> &moveTargets() { return m_moveTargets; }
        const std::vector<MoveTarget> &moveTargets() const { return m_moveTargets; }

        std::vector<MoveSpeed> &moveSpeeds() { return m_moveSpeeds; }
        const std::vector<MoveSpeed> &moveSpeeds() const { return m_moveSpeeds; }

        std::vector<Radius> &radii() { return m_radii; }
        const std::vector<Radius> &radii() const { return m_radii; }

        std::vector<Separation> &separations() { return m_separations; }
        const std::vector<Separation> &separations() const { return m_separations; }

        std::vector<AvoidanceParams> &avoidanceParams() { return m_avoidanceParams; }
        const std::vector<AvoidanceParams> &avoidanceParams() const { return m_avoidanceParams; }

        std::vector<RenderModel> &renderModels() { return m_renderModels; }
        const std::vector<RenderModel> &renderModels() const { return m_renderModels; }

        std::vector<LocomotionClips> &locomotionClips() { return m_locomotionClips; }
        const std::vector<LocomotionClips> &locomotionClips() const { return m_locomotionClips; }

        std::vector<CombatClips> &combatClips() { return m_combatClips; }
        const std::vector<CombatClips> &combatClips() const { return m_combatClips; }

        std::vector<RenderAnimation> &renderAnimations() { return m_renderAnimations; }
        const std::vector<RenderAnimation> &renderAnimations() const { return m_renderAnimations; }

        std::vector<Facing> &facings() { return m_facings; }
        const std::vector<Facing> &facings() const { return m_facings; }

        std::vector<RenderTransform> &renderTransforms() { return m_renderTransforms; }
        const std::vector<RenderTransform> &renderTransforms() const { return m_renderTransforms; }

        std::vector<ObstacleRadius> &obstacleRadii() { return m_obstacleRadii; }
        const std::vector<ObstacleRadius> &obstacleRadii() const { return m_obstacleRadii; }

        std::vector<Path> &paths() { return m_paths; }
        const std::vector<Path> &paths() const { return m_paths; }
        std::vector<PosePalette> &posePalettes() { return m_posePalettes; }
        const std::vector<PosePalette> &posePalettes() const { return m_posePalettes; }

        std::vector<Team> &teams() { return m_teams; }
        const std::vector<Team> &teams() const { return m_teams; }

        std::vector<AttackCooldown> &attackCooldowns() { return m_attackCooldowns; }
        const std::vector<AttackCooldown> &attackCooldowns() const { return m_attackCooldowns; }

        // Helpers
        bool hasPosition() const { return m_hasPosition; }
        bool hasVelocity() const { return m_hasVelocity; }
        bool hasHealth() const { return m_hasHealth; }
        bool hasMoveTarget() const { return m_hasMoveTarget; }
        bool hasMoveSpeed() const { return m_hasMoveSpeed; }
        bool hasRadius() const { return m_hasRadius; }
        bool hasSeparation() const { return m_hasSeparation; }
        bool hasAvoidanceParams() const { return m_hasAvoidanceParams; }
        bool hasRenderModel() const { return m_hasRenderModel; }
        bool hasLocomotionClips() const { return m_hasLocomotionClips; }
        bool hasCombatClips() const { return m_hasCombatClips; }
        bool hasRenderAnimation() const { return m_hasRenderAnimation; }
        bool hasFacing() const { return m_hasFacing; }
        bool hasRenderTransform() const { return m_hasRenderTransform; }
        bool hasObstacle() const { return m_hasObstacle; }
        bool hasObstacleRadius() const { return m_hasObstacleRadius; }
        bool hasPath() const { return m_hasPath; }
        bool hasPosePalette() const { return m_hasPosePalette; }
        bool hasTeam() const { return m_hasTeam; }
        bool hasAttackCooldown() const { return m_hasAttackCooldown; }

        // Resolve which known components are present in signature; enables arrays accordingly.
        void resolveKnownComponents(ComponentRegistry &registry)
        {
            const uint32_t posId = registry.ensureId("Position");
            const uint32_t velId = registry.ensureId("Velocity");
            const uint32_t heaId = registry.ensureId("Health");
            const uint32_t tgtId = registry.ensureId("MoveTarget");
            const uint32_t spdId = registry.ensureId("MoveSpeed");
            const uint32_t radId = registry.ensureId("Radius");
            const uint32_t sepId = registry.ensureId("Separation");
            const uint32_t apId = registry.ensureId("AvoidanceParams");
            const uint32_t rmId = registry.ensureId("RenderModel");
            const uint32_t locoId = registry.ensureId("LocomotionClips");
            const uint32_t ccId = registry.ensureId("CombatClips");
            const uint32_t raId = registry.ensureId("RenderAnimation");
            const uint32_t faceId = registry.ensureId("Facing");
            const uint32_t rtId = registry.ensureId("RenderTransform");
            const uint32_t obsId = registry.ensureId("Obstacle");
            const uint32_t obsRId = registry.ensureId("ObstacleRadius");
            const uint32_t pathId = registry.ensureId("Path");
            const uint32_t ppId = registry.ensureId("PosePalette");
            m_hasPosition = m_signature.has(posId);
            m_hasVelocity = m_signature.has(velId);
            m_hasHealth = m_signature.has(heaId);
            m_hasMoveTarget = m_signature.has(tgtId);
            m_hasMoveSpeed = m_signature.has(spdId);
            m_hasRadius = m_signature.has(radId);
            m_hasSeparation = m_signature.has(sepId);
            m_hasAvoidanceParams = m_signature.has(apId);
            m_hasRenderModel = m_signature.has(rmId);
            m_hasLocomotionClips = m_signature.has(locoId);
            m_hasCombatClips = m_signature.has(ccId);
            m_hasRenderAnimation = m_signature.has(raId);
            m_hasFacing = m_signature.has(faceId);
            m_hasRenderTransform = m_signature.has(rtId);
            m_hasObstacle = m_signature.has(obsId);
            m_hasObstacleRadius = m_signature.has(obsRId);
            m_hasPath = m_signature.has(pathId);
            m_hasPosePalette = m_signature.has(ppId);

            const uint32_t teamId = registry.ensureId("Team");
            const uint32_t ackId = registry.ensureId("AttackCooldown");
            m_hasTeam = m_signature.has(teamId);
            m_hasAttackCooldown = m_signature.has(ackId);
        }

    private:
        ComponentMask m_signature;
        std::vector<Entity> m_entities;

        // Component arrays (only used if signature includes them).
        std::vector<Position> m_positions;
        std::vector<Velocity> m_velocities;
        std::vector<Health> m_healths;
        std::vector<MoveTarget> m_moveTargets;
        std::vector<MoveSpeed> m_moveSpeeds;
        std::vector<Radius> m_radii;
        std::vector<Separation> m_separations;
        std::vector<AvoidanceParams> m_avoidanceParams;
        std::vector<RenderModel> m_renderModels;
        std::vector<LocomotionClips> m_locomotionClips;
        std::vector<CombatClips> m_combatClips;
        std::vector<RenderAnimation> m_renderAnimations;
        std::vector<Facing> m_facings;
        std::vector<RenderTransform> m_renderTransforms;
        std::vector<ObstacleRadius> m_obstacleRadii;
        std::vector<Path> m_paths;
        std::vector<PosePalette> m_posePalettes;
        std::vector<Team> m_teams;
        std::vector<AttackCooldown> m_attackCooldowns;

        // Flags indicating which arrays are active.
        bool m_hasPosition = false;
        bool m_hasVelocity = false;
        bool m_hasHealth = false;
        bool m_hasMoveTarget = false;
        bool m_hasMoveSpeed = false;
        bool m_hasRadius = false;
        bool m_hasSeparation = false;
        bool m_hasAvoidanceParams = false;
        bool m_hasRenderModel = false;
        bool m_hasLocomotionClips = false;
        bool m_hasCombatClips = false;
        bool m_hasRenderAnimation = false;
        bool m_hasFacing = false;
        bool m_hasRenderTransform = false;
        bool m_hasObstacle = false;
        bool m_hasObstacleRadius = false;
        bool m_hasPath = false;
        bool m_hasPosePalette = false;
        bool m_hasTeam = false;
        bool m_hasAttackCooldown = false;
    };

    class ArchetypeStoreManager
    {
    public:
        void setOnStoreCreated(std::function<void(uint32_t archetypeId, const ComponentMask &signature)> cb)
        {
            m_onStoreCreated = std::move(cb);
        }

        ArchetypeStore *getOrCreate(uint32_t archetypeId, const ComponentMask &signature, ComponentRegistry &registry)
        {
            if (archetypeId >= m_stores.size())
                m_stores.resize(archetypeId + 1);

            if (!m_stores[archetypeId])
            {
                m_stores[archetypeId] = std::make_unique<ArchetypeStore>(signature);
                m_stores[archetypeId]->resolveKnownComponents(registry);
                if (m_onStoreCreated)
                    m_onStoreCreated(archetypeId, signature);
            }
            return m_stores[archetypeId].get();
        }

        ArchetypeStore *get(uint32_t archetypeId)
        {
            return (archetypeId < m_stores.size()) ? m_stores[archetypeId].get() : nullptr;
        }

        const ArchetypeStore *get(uint32_t archetypeId) const
        {
            return (archetypeId < m_stores.size()) ? m_stores[archetypeId].get() : nullptr;
        }

        const std::vector<std::unique_ptr<ArchetypeStore>> &stores() const { return m_stores; }

    private:
        std::vector<std::unique_ptr<ArchetypeStore>> m_stores;
        std::function<void(uint32_t archetypeId, const ComponentMask &signature)> m_onStoreCreated;
    };

} // namespace Engine::ECS