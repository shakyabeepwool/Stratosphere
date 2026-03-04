#pragma once
/*
  Components.h
  ------------
  Purpose:
    - Define component data structures (Position, Velocity, Health).
    - Provide ComponentRegistry for name <-> ID mapping (data-driven).
    - Provide ComponentMask: dynamic bitset keyed by component IDs.

  Usage:
    - ComponentRegistry gives stable numeric IDs for component names defined in JSON.
    - ComponentMask builds signatures using those IDs to represent an entity/archetype's component set.
*/

#include <cstdint>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <unordered_map>
#include <variant>
#include <assets/Handles.h>
#include <glm/glm.hpp>

namespace Engine::ECS
{
    // -----------------------
    // Component Data Types
    // -----------------------

    // Spatial position in world space.
    // Convention for gameplay:
    //  - X/Z define the ground plane (meters).
    //  - Y is height (meters).
    struct Position
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    // Linear velocity (units per second), world space.
    struct Velocity
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    // Simple health component.
    struct Health
    {
        float value = 100.0f;
    };

    // Target position for movement.
    struct MoveTarget
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        uint8_t active = 0; // 0 = inactive, 1 = active
    };

    // Movement speed
    struct MoveSpeed
    {
        float value = 5.0f; // default if not overridden in prefabs
    };

    // Simple radius component for local avoidance
    struct Radius
    {
        float r = 0.07f; // tune per unit type
    };

    // Desired extra spacing beyond physical radii (meters).
    // Used by local avoidance to keep units from clustering too tightly.
    struct Separation
    {
        float value = 0.0f;
    };

    // Tunables for local avoidance
    struct AvoidanceParams
    {
        // Base separation
        float strength = 1.0f; // acceleration scale (units/s^2)
        float maxAccel = 0.9f; // clamp acceleration magnitude (units/s^2)
        float blend = 0.55f;   // 0..1 blend from preferred velocity to avoided velocity

        // Neighbor interaction
        float predictionTime = 0.55f;   // seconds, short-horizon predictive check
        float interactSlack = 0.35f;    // meters, extra interaction range beyond desired distance
        float falloffWeight = 0.35f;    // weight for soft keep-apart when not overlapping
        float predictiveWeight = 0.75f; // weight for predictive closest-approach avoidance
        float pressureBoost = 1.5f;     // multiplier when overlapped / high-pressure

        // Arrival / stopped behavior
        float nearGoalRadius = 2.0f; // meters
        float nearGoalBoost = 2.5f;  // multiplier added near goal: 1 + nearGoalBoost*t
        float stoppedBoost = 2.0f;   // multiplier when not moving and no target
        float maxStopSpeed = 0.9f;   // clamp speed when preferred speed ~0
    };

    // Row-level tag used by selection (no per-row storage; stored in row masks).
    struct Selected
    {
    };

    // -----------------------
    // Obstacle Components
    // -----------------------

    // Tag for static, impassable entities
    struct Obstacle
    {
    };

    // Collision footprint for obstacles (meters)
    struct ObstacleRadius
    {
        float r = 1.0f;
    };

    // -----------------------
    // Pathfinding Components
    // -----------------------

    // Waypoint buffer for A* pathfinding
    struct Path
    {
        static constexpr uint32_t MAX_WAYPOINTS = 64;

        float waypointsX[MAX_WAYPOINTS];
        float waypointsZ[MAX_WAYPOINTS];
        uint32_t count = 0;   // how many waypoints are valid
        uint32_t current = 0; // index of the next waypoint to walk toward
        bool valid = false;   // was a path successfully found?
    };

    struct RenderModel
    {
        ModelHandle handle;
    };

    // Data-driven clip mapping for generic locomotion controllers.
    // Indices are model-specific and should be provided via JSON.
    struct LocomotionClips
    {
        uint32_t idleClip = 65;
        uint32_t walkClip = 112;
        uint32_t runClip = 28;
    };

    // Data-driven clip mapping for generic combat controllers.
    // Ranges are inclusive.
    struct CombatClips
    {
        uint32_t attackStart = 36;
        uint32_t attackEnd = 48;
        uint32_t damageStart = 52;
        uint32_t damageEnd = 56;
        uint32_t deathStart = 61;
        uint32_t deathEnd = 64;
    };

    // Per-entity animation state (node TRS only; no skinning yet)
    struct RenderAnimation
    {
        uint32_t clipIndex = 0;
        float timeSec = 0.0f;
        float speed = 1.0f;
        bool loop = false;
        bool playing = false;
    };

    // Entity facing direction (Y-axis rotation in radians)
    // 0 = facing +Z, PI/2 = facing +X, PI = facing -Z, -PI/2 = facing -X
    struct Facing
    {
        float yaw = 0.0f; // Rotation around Y axis in radians
    };

    // Render-side cached world transform.
    // Updated by a dedicated system from Position (+ optional Facing).
    struct RenderTransform
    {
        glm::mat4 world{1.0f};
        uint32_t transformVersion = 0; // monotonic; wrap is OK
    };

    // Cached pose palettes computed by PoseUpdateSystem.
    // nodePalette: one matrix per node in the model.
    // jointPalette: one matrix per joint across all skins in the model (flattened).
    struct PosePalette
    {
        std::vector<glm::mat4> nodePalette;
        std::vector<glm::mat4> jointPalette;
        uint32_t nodeCount = 0;
        uint32_t jointCount = 0;

        // Monotonic version incremented when PoseUpdateSystem recomputes this pose.
        // Wrap is fine; render-side compares for inequality.
        uint32_t poseVersion = 0;

        // Optional debugging aid (local frame counter from PoseUpdateSystem).
        uint32_t lastUpdatedFrame = 0;
    };

    // -----------------------
    // Combat Components
    // -----------------------

    // Which team/faction the entity belongs to (0 = Team A, 1 = Team B, etc.)
    struct Team
    {
        uint8_t id = 0;
    };

    // Melee attack cooldown timer.
    struct AttackCooldown
    {
        float timer = 0.0f;    // current countdown (seconds)
        float interval = 1.5f; // time between attacks (seconds)
    };

    // Typed defaults per component ID (used by Prefabs/Stores).
    using DefaultValue = std::variant<Position, Velocity, Health, MoveTarget, MoveSpeed, Radius, Separation, AvoidanceParams, RenderModel, LocomotionClips, CombatClips, RenderAnimation, Facing, RenderTransform, ObstacleRadius, Path, PosePalette, Team, AttackCooldown>;
    // -----------------------
    // Component Registry
    // -----------------------
    // Maps component names (e.g., "Position") to stable numeric IDs, and vice versa.
    // This enables data-driven JSON to refer to components by name while the engine uses compact IDs.
    class ComponentRegistry
    {
    public:
        static constexpr uint32_t InvalidID = UINT32_MAX;

        // Register a component name and return its stable ID.
        // If already registered, returns the existing ID.
        uint32_t registerComponent(const std::string &name)
        {
            auto it = m_nameToId.find(name);
            if (it != m_nameToId.end())
                return it->second;

            const uint32_t id = static_cast<uint32_t>(m_idToName.size());
            m_nameToId.emplace(name, id);
            m_idToName.emplace_back(name);
            return id;
        }

        // Get ID by name; returns InvalidID if not found.
        uint32_t getId(const std::string &name) const
        {
            auto it = m_nameToId.find(name);
            return (it != m_nameToId.end()) ? it->second : InvalidID;
        }

        // Ensure a name exists; if missing, register it and return the new ID.
        uint32_t ensureId(const std::string &name)
        {
            auto it = m_nameToId.find(name);
            if (it != m_nameToId.end())
                return it->second;
            return registerComponent(name);
        }

        // Get name by ID; returns empty string if invalid.
        const std::string &getName(uint32_t id) const
        {
            static const std::string empty{};
            return (id < m_idToName.size()) ? m_idToName[id] : empty;
        }

        // Total number of registered components.
        uint32_t count() const { return static_cast<uint32_t>(m_idToName.size()); }

    private:
        std::unordered_map<std::string, uint32_t> m_nameToId;
        std::vector<std::string> m_idToName;
    };

    // -----------------------
    // Component Mask (dynamic)
    // -----------------------
    // Represents a set of components by their IDs. Backed by 64-bit words.
    class ComponentMask
    {
    public:
        ComponentMask() = default;

        // Set a bit for component ID.
        void set(uint32_t compId)
        {
            ensureBit(compId);
            const auto [wordIdx, bit] = bitPos(compId);
            m_words[wordIdx] |= (uint64_t(1) << bit);
        }

        // Clear a bit for component ID.
        void clear(uint32_t compId)
        {
            if (!has(compId))
                return;
            const auto [wordIdx, bit] = bitPos(compId);
            m_words[wordIdx] &= ~(uint64_t(1) << bit);
        }

        // Check if a bit for component ID is set.
        bool has(uint32_t compId) const
        {
            const auto [wordIdx, bit] = bitPos(compId);
            if (wordIdx >= m_words.size())
                return false;
            return (m_words[wordIdx] & (uint64_t(1) << bit)) != 0;
        }

        // Return true if this mask contains all bits in 'rhs'.
        bool containsAll(const ComponentMask &rhs) const
        {
            const size_t n = std::max(m_words.size(), rhs.m_words.size());
            for (size_t i = 0; i < n; ++i)
            {
                const uint64_t a = i < m_words.size() ? m_words[i] : 0;
                const uint64_t b = i < rhs.m_words.size() ? rhs.m_words[i] : 0;
                if ((a & b) != b)
                    return false;
            }
            return true;
        }

        // Return true if this mask contains none of the bits in 'rhs'.
        bool containsNone(const ComponentMask &rhs) const
        {
            const size_t n = std::max(m_words.size(), rhs.m_words.size());
            for (size_t i = 0; i < n; ++i)
            {
                const uint64_t a = i < m_words.size() ? m_words[i] : 0;
                const uint64_t b = i < rhs.m_words.size() ? rhs.m_words[i] : 0;
                if ((a & b) != 0)
                    return false;
            }
            return true;
        }

        // Convenience: required/excluded match.
        bool matches(const ComponentMask &required, const ComponentMask &excluded) const
        {
            return containsAll(required) && containsNone(excluded);
        }

        // Stable string key for dictionary indexing (hex of words, high word first).
        std::string toKey() const
        {
            if (m_words.empty())
                return "0";
            std::ostringstream oss;
            oss << std::hex << std::setfill('0');
            for (size_t i = m_words.size(); i-- > 0;)
            {
                oss << std::setw(16) << m_words[i];
            }
            return oss.str();
        }

        // Build a mask from a list of component IDs.
        static ComponentMask fromIds(const std::vector<uint32_t> &ids)
        {
            ComponentMask m;
            for (uint32_t id : ids)
                m.set(id);
            return m;
        }

        const std::vector<uint64_t> &words() const { return m_words; }

    private:
        std::vector<uint64_t> m_words; // 64 bits per word

        static std::pair<size_t, uint32_t> bitPos(uint32_t compId)
        {
            const size_t wordIdx = compId / 64;
            const uint32_t bit = compId % 64;
            return {wordIdx, bit};
        }

        void ensureBit(uint32_t compId)
        {
            const size_t need = compId / 64 + 1;
            if (m_words.size() < need)
                m_words.resize(need, 0);
        }
    };

} // namespace Engine::ECS