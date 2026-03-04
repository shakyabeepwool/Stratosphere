#pragma once
/*
  CombatSystem.h
  --------------
  Public interface + state for the sample CombatSystem.

  The large implementation (update loop + helper methods) lives in:
    Sample/utils/CombatSystemImpl.inl

  All tunable constants are defined at the top of this file in ALL_CAPS.
*/

#include "ECS/SystemFormat.h"
#include "ECS/Components.h"
#include "ECS/systems/SpatialIndexSystem.h"
#include "assets/AssetManager.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <iostream>

// -----------------------------------------------------------------------------
// TUNABLES (ALL_CAPS)
// -----------------------------------------------------------------------------
namespace CombatTuning
{
    static constexpr float MELEE_RANGE_DEFAULT = 2.0f;
    static constexpr float ENGAGE_RANGE_DEFAULT = 10.0f;
    static constexpr float DAMAGE_MIN_DEFAULT = 12.0f;
    static constexpr float DAMAGE_MAX_DEFAULT = 28.0f;
    static constexpr float DEATH_REMOVE_DELAY_DEFAULT = 3.0f;
    static constexpr float MAX_HP_PER_UNIT_DEFAULT = 140.0f;

    static constexpr float MISS_CHANCE_DEFAULT = 0.20f;
    static constexpr float CRIT_CHANCE_DEFAULT = 0.10f;
    static constexpr float CRIT_MULTIPLIER_DEFAULT = 2.0f;
    static constexpr float RAGE_MAX_BONUS_DEFAULT = 0.50f;
    static constexpr float COOLDOWN_JITTER_DEFAULT = 0.30f;
    static constexpr float STAGGER_MAX_DEFAULT = 0.60f;

    static constexpr float ATTACK_ANIM_SPEED = 1.5f;
    static constexpr float DAMAGE_ANIM_SPEED = 1.0f;
    static constexpr float CRIT_DAMAGE_ANIM_SPEED = 1.4f;

    static constexpr float ENEMY_TIE_EPS = 1e-6f;
    static constexpr float BEST_DIST2_INIT = 1e18f;
    static constexpr float FALLBACK_NO_ENEMY_DIST2 = 1e17f;
    static constexpr float YAW_DIST2_EPS = 1e-6f;

    static constexpr float MOVE_TARGET_REPATH_DIST2 = 4.0f;
    static constexpr float CLICK_TARGET_MATCH_DIST2 = 1.0f;

    // Charge leg switching: unit is considered "passing through" the click point.
    static constexpr float PASS_RADIUS = 3.0f;
    static constexpr float PASS_RADIUS2 = PASS_RADIUS * PASS_RADIUS;
}

// Knight animation clip indices relevant to combat
namespace CombatAnims
{
    // Attacks: Stand_Attack_1 through Stand_Attack_8 (indices 36-43)
    constexpr uint32_t ATTACK_START = 36;
    constexpr uint32_t ATTACK_END = 43;

    // Damage reactions: Stand_Damage_0 through Stand_Damage_4 (indices 52-56)
    constexpr uint32_t DAMAGE_START = 52;
    constexpr uint32_t DAMAGE_END = 56;

    // Death: Stand_Death_0 through Stand_Death_3 (indices 61-64)
    constexpr uint32_t DEATH_START = 61;
    constexpr uint32_t DEATH_END = 64;

    // Run (for charging)
    constexpr uint32_t RUN = 28;

    // Idle
    constexpr uint32_t IDLE = 65;
}

class CombatSystem : public Engine::ECS::SystemBase
{
public:
    // Per-team aggregate stats for the HUD overlay
    struct TeamStats
    {
        int alive = 0;          // living units this frame
        int totalSpawned = 0;   // units that were initially spawned
        float currentHP = 0.0f; // sum of living units' health
        float maxHP = 0.0f;     // totalSpawned * maxHPPerUnit
    };

    // All combat tuning in one struct — maps 1:1 to BattleConfig.json "combat"
    struct CombatConfig
    {
        float meleeRange = CombatTuning::MELEE_RANGE_DEFAULT;
        float engageRange = CombatTuning::ENGAGE_RANGE_DEFAULT; // auto-start when opposing units come within this range
        float damageMin = CombatTuning::DAMAGE_MIN_DEFAULT;
        float damageMax = CombatTuning::DAMAGE_MAX_DEFAULT;
        float deathRemoveDelay = CombatTuning::DEATH_REMOVE_DELAY_DEFAULT;
        float maxHPPerUnit = CombatTuning::MAX_HP_PER_UNIT_DEFAULT;

        float missChance = CombatTuning::MISS_CHANCE_DEFAULT;
        float critChance = CombatTuning::CRIT_CHANCE_DEFAULT;
        float critMultiplier = CombatTuning::CRIT_MULTIPLIER_DEFAULT;

        float rageMaxBonus = CombatTuning::RAGE_MAX_BONUS_DEFAULT;
        float cooldownJitter = CombatTuning::COOLDOWN_JITTER_DEFAULT;
        float staggerMax = CombatTuning::STAGGER_MAX_DEFAULT;
    };

    CombatSystem();

    const char *name() const override { return "CombatSystem"; }

    void setSpatialIndex(SpatialIndexSystem *spatial) { m_spatial = spatial; }
    void setAssetManager(Engine::AssetManager *assets) { m_assets = assets; }

    void applyConfig(const CombatConfig &cfg) { m_cfg = cfg; }
    const CombatConfig &config() const { return m_cfg; }

    void startBattle(float clickX, float clickZ)
    {
        m_battleStarted = true;
        m_chargeActive = true;
        m_chargeIssued = false;
        m_battleClickX = clickX;
        m_battleClickZ = clickZ;
        std::cout << "[CombatSystem] Battle started! Click=(" << clickX << "," << clickZ << ")\n";
    }
    void startBattle()
    {
        m_battleStarted = true;
        m_chargeActive = false;
        m_chargeIssued = false;
        std::cout << "[CombatSystem] Battle started!\n";
    }
    bool isBattleStarted() const { return m_battleStarted; }

    void setHumanTeam(int teamId) { m_humanTeamId = teamId; } // -1 = all AI
    void setHumanAttacking(bool attacking) { m_humanAttacking = attacking; }
    int humanTeamId() const { return m_humanTeamId; }
    bool isHumanAttacking() const { return m_humanAttacking; }

    void setMeleeRange(float range) { m_cfg.meleeRange = range; }
    void setDamagePerHit(float dmg)
    {
        m_cfg.damageMin = dmg * 0.6f;
        m_cfg.damageMax = dmg * 1.4f;
    }
    void setDeathRemoveDelay(float sec) { m_cfg.deathRemoveDelay = sec; }
    void setMaxHPPerUnit(float hp) { m_cfg.maxHPPerUnit = hp; }

    const TeamStats &getTeamStats(uint8_t teamId) const
    {
        static const TeamStats empty{};
        auto it = m_teamStats.find(teamId);
        return (it != m_teamStats.end()) ? it->second : empty;
    }

    void buildMasks(Engine::ECS::ComponentRegistry &registry) override;
    void update(Engine::ECS::ECSContext &ecs, float dt) override;

private:
    void staggerInitialCooldowns(Engine::ECS::ECSContext &ecs);
    void refreshTeamStats(Engine::ECS::ECSContext &ecs);
    void processDeathRemovals(Engine::ECS::ECSContext &ecs, float dt);

    void issueClickTargets(Engine::ECS::ECSContext &ecs, const Engine::ECS::Query &q);
    void promoteUnitsNearClick(Engine::ECS::ECSContext &ecs, const Engine::ECS::Query &q);

    struct PendingDeath
    {
        Engine::ECS::Entity entity;
        float timeRemaining;
    };

    struct DamageAction
    {
        Engine::ECS::Entity target;
        float damage;
    };
    struct AnimAction
    {
        Engine::ECS::Entity entity;
        uint32_t clipIndex;
        float speed;
        bool loop;
    };
    struct MoveAction
    {
        Engine::ECS::Entity entity;
        float tx, tz;
        bool active;
        float yaw;
        uint32_t runClip;
        bool setRunAnim;
    };
    struct StopAction
    {
        Engine::ECS::Entity entity;
        float yaw;
        bool clearVelocity;
    };

    SpatialIndexSystem *m_spatial = nullptr;
    Engine::AssetManager *m_assets = nullptr;

    CombatConfig m_cfg;

    bool m_loggedStart = false;
    bool m_statsDirty = true;

    bool m_battleStarted = false;
    bool m_chargeActive = false;
    bool m_chargeIssued = false;
    float m_battleClickX = 0.0f;
    float m_battleClickZ = 0.0f;

    int m_humanTeamId = -1;
    bool m_humanAttacking = true;

    std::unordered_map<uint8_t, TeamStats> m_teamStats;

    uint32_t m_positionId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_healthId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_velocityId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_moveTargetId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_teamId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_attackCooldownId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_renderAnimId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_facingId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_deadId = Engine::ECS::ComponentRegistry::InvalidID;

    Engine::ECS::QueryId m_queryId = Engine::ECS::QueryManager::InvalidQuery;

    std::vector<PendingDeath> m_deathQueue;
    std::unordered_set<uint32_t> m_deathQueueSet;

    std::vector<DamageAction> m_damages;
    std::vector<AnimAction> m_attackAnims;
    std::vector<AnimAction> m_damageAnims;
    std::vector<MoveAction> m_moves;
    std::vector<StopAction> m_stops;
    std::vector<Engine::ECS::Entity> m_newlyDead;

    struct UnitCombatMemory
    {
        Engine::ECS::Entity targetEnemy{};
        bool engaged = false;
        uint32_t lastSeenFrame = 0;
    };
    std::unordered_map<uint64_t, UnitCombatMemory> m_unitMem;
    uint32_t m_frameCounter = 0;

    std::mt19937 m_rng;
    std::uniform_real_distribution<float> m_unitDist{0.0f, 1.0f};
    std::uniform_real_distribution<float> m_realDist{-1.0f, 1.0f};
};

#include "utils/CombatSystemImpl.inl"