#include "MySampleApp.h"

#include "Engine/VulkanContext.h"
#include "Engine/Window.h"
#include "Engine/ImGuiLayer.h"
#include <vulkan/vulkan.h>

#include "ECS/Prefab.h"
#include "ECS/PrefabSpawner.h"
#include "ECS/ECSContext.h"

#include "ScenarioSpawner.h"
#include "assets/AssetManager.h"

#include "Engine/GroundPlaneRenderPassModule.h"

#include <nlohmann/json.hpp>
#include <fstream>

#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>

#include <cmath>
#include <algorithm>

#include <glm/gtc/matrix_transform.hpp>

#include "MenuManager.h"
#include <GLFW/glfw3.h>

using json = nlohmann::json;

MySampleApp::MySampleApp() : Engine::Application()
{
    m_assets = std::make_unique<Engine::AssetManager>(
        GetVulkanContext().GetDevice(),
        GetVulkanContext().GetPhysicalDevice(),
        GetVulkanContext().GetGraphicsQueue(),
        GetVulkanContext().GetGraphicsQueueFamilyIndex());

    m_menu.SetTextureLoader([this](const std::string &relpath) -> ImTextureID
                            {
            if (!m_assets)
                return nullptr;

            // Load into an Engine texture asset
            Engine::TextureHandle th = m_assets->loadTextureFromFile(relpath);
            if (!th.isValid())
                return nullptr;

            Engine::TextureAsset* ta = m_assets->getTexture(th);
            if (!ta)
                return nullptr;

            VkSampler sampler = ta->getSampler();
            VkImageView view = ta->getView();
            if (sampler == VK_NULL_HANDLE || view == VK_NULL_HANDLE)
                return nullptr;

            // Register with ImGui (uses ImGui_ImplVulkan_AddTexture internally)
            Engine::ImGuiLayer* layer = GetImGuiLayer();
            if (!layer)
                return nullptr;

            return layer->addTexture(sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); });
    // Optionally let the MenuManager know whether a save exists (so it can enable "Continue")
    m_menu.SetHasSaveFile(HasSaveFile());
    m_menu.SetMode(MenuManager::Mode::MainMenu);

    auto loader = m_menu.GetTextureLoader();
    if (loader)
    {
        // Use same relative path convention as MenuManager (example uses "assets/raw/...")
        ImTextureID bg = loader("assets/raw/menu.png");
        ImTextureID tnew = loader("assets/raw/newgame.png");
        ImTextureID tcont = loader("assets/raw/continuegame.png");
        ImTextureID texit = loader("assets/raw/exit.png");
    }
    // RTS camera initialization — raised for the larger maze map.
    m_rtsCam.focus = {0.0f, 0.0f, 0.0f};
    m_rtsCam.yawDeg = -45.0f;
    m_rtsCam.pitchDeg = -55.0f;
    m_rtsCam.height = 120.0f;
    m_rtsCam.minHeight = 5.0f;
    m_rtsCam.maxHeight = 250.0f;

    auto &win = GetWindow();
    const float aspect = static_cast<float>(win.GetWidth()) / static_cast<float>(win.GetHeight());
    ApplyRTSCamera(aspect);

    // Seed mouse position so the first frame doesn't produce a huge delta.
    double mx = 0.0, my = 0.0;
    win.GetCursorPosition(mx, my);
    m_lastMouse = {static_cast<float>(mx), static_cast<float>(my)};

    // Allow gameplay systems to resolve RenderModel handles to loaded assets.
    m_systems.SetAssetManager(m_assets.get());
    m_systems.SetRenderer(&GetRenderer());
    m_systems.SetCamera(&m_camera);

    // ------------------------------------------------------------
    // Background: simple ground-plane pass using ground baseColor tex
    // ------------------------------------------------------------
    {
        Engine::ModelHandle groundModel = m_assets->loadModel("assets/Ground/scene.smodel");
        if (groundModel.isValid())
        {
            if (Engine::ModelAsset *m = m_assets->getModel(groundModel))
            {
                if (!m->primitives.empty())
                {
                    const Engine::MaterialHandle mh = m->primitives[0].material;
                    if (Engine::MaterialAsset *mat = m_assets->getMaterial(mh))
                    {
                        if (mat->baseColorTexture.isValid())
                            m_groundTexture = mat->baseColorTexture;
                    }
                }
            }

            // Keep texture alive even if the model/material are collected later.
            if (m_groundTexture.isValid())
                m_assets->addRef(m_groundTexture);

            // We only needed the texture; let the model be GC'd.
            m_assets->release(groundModel);
        }

        if (m_groundTexture.isValid())
        {
            m_groundPass = std::make_shared<Engine::GroundPlaneRenderPassModule>();
            m_groundPass->setAssets(m_assets.get());
            m_groundPass->setCamera(&m_camera);
            m_groundPass->setBaseColorTexture(m_groundTexture);
            m_groundPass->setHalfSize(350.0f);
            m_groundPass->setTileWorldSize(5.0f);
            m_groundPass->setEnabled(true);
            GetRenderer().registerPass(m_groundPass);
        }
    }

    setupECSFromPrefabs();

    // Enable incremental query updates when new stores are created.
    GetECS().WireQueryManager();

    // Systems can be initialized after prefabs are registered.
    m_systems.Initialize(GetECS());

    // Hook engine window events into our handler.
    SetEventCallback([this](const std::string &e)
                     { this->OnEvent(e); });

    // Detect whether a save exists so MenuManager can show Continue
    m_menu.SetHasSaveFile(HasSaveFile());

    // Hook up event callback (already present in file previously)
    SetEventCallback([this](const std::string &e)
                     { this->OnEvent(e); });
}

MySampleApp::~MySampleApp() = default;

void MySampleApp::Close()
{
    vkDeviceWaitIdle(GetVulkanContext().GetDevice());

    if (m_assets)
    {
        if (m_groundTexture.isValid())
            m_assets->release(m_groundTexture);
        m_assets->garbageCollect();
    }

    Engine::Application::Close();
}

void MySampleApp::OnUpdate(Engine::TimeStep ts)
{
    // When the in-game pause menu is visible, freeze the simulation so "Continue"
    // resumes exactly from the state when Escape was pressed.
    if (m_inGame && m_menu.IsVisible())
        return;

    auto &win = GetWindow();
    const float aspect = static_cast<float>(win.GetWidth()) / static_cast<float>(win.GetHeight());

    // Read mouse and compute per-frame delta.
    double mx = 0.0, my = 0.0;
    win.GetCursorPosition(mx, my);
    const glm::vec2 mouse{static_cast<float>(mx), static_cast<float>(my)};
    const glm::vec2 delta = mouse - m_lastMouse;
    m_lastMouse = mouse;

    // Pan (LMB drag) in ground plane; modifies focus only.
    if (m_isPanning)
    {
        if (m_panJustStarted)
        {
            // Prevent a jump on the initial press frame.
            m_panJustStarted = false;
        }
        else
        {
            glm::vec3 forward;
            forward.x = std::cos(glm::radians(m_rtsCam.yawDeg)) * std::cos(glm::radians(m_rtsCam.pitchDeg));
            forward.y = std::sin(glm::radians(m_rtsCam.pitchDeg));
            forward.z = std::sin(glm::radians(m_rtsCam.yawDeg)) * std::cos(glm::radians(m_rtsCam.pitchDeg));
            forward = glm::normalize(forward);

            const glm::vec3 worldUp{0.0f, 1.0f, 0.0f};
            const glm::vec3 right = glm::normalize(glm::cross(forward, worldUp));

            glm::vec3 forwardXZ{forward.x, 0.0f, forward.z};
            glm::vec3 rightXZ{right.x, 0.0f, right.z};

            const float forwardLen2 = glm::dot(forwardXZ, forwardXZ);
            const float rightLen2 = glm::dot(rightXZ, rightXZ);
            if (forwardLen2 > 1e-6f)
                forwardXZ *= 1.0f / std::sqrt(forwardLen2);
            if (rightLen2 > 1e-6f)
                rightXZ *= 1.0f / std::sqrt(rightLen2);

            const float panScale = m_rtsCam.basePanSpeed * m_rtsCam.height;
            // Update focus (not position). Mouse delta is in pixels.
            m_rtsCam.focus += (-rightXZ * delta.x + forwardXZ * delta.y) * panScale;
            m_rtsCam.focus.y = 0.0f;
        }
    }

    // Zoom (mouse wheel) modifies height.
    const float wheel = m_scrollDelta;
    m_scrollDelta = 0.0f;
    if (wheel != 0.0f)
    {
        m_rtsCam.height -= wheel * m_rtsCam.zoomSpeed;
        m_rtsCam.height = glm::clamp(m_rtsCam.height, m_rtsCam.minHeight, m_rtsCam.maxHeight);
    }

    // Apply RTS state to engine camera every frame.
    ApplyRTSCamera(aspect);

    m_systems.Update(GetECS(), ts.DeltaSeconds);
}

void MySampleApp::PickAndSelectEntityAtCursor()
{
    auto &ecs = GetECS();
    auto &win = GetWindow();

    double mx = 0.0, my = 0.0;
    win.GetCursorPosition(mx, my);

    const float mouseX = static_cast<float>(mx);
    const float mouseY = static_cast<float>(my);
    const float width = static_cast<float>(win.GetWidth());
    const float height = static_cast<float>(win.GetHeight());

    const uint32_t selectedId = ecs.components.ensureId("Selected");
    const uint32_t teamId = ecs.components.ensureId("Team");
    const uint32_t posId = ecs.components.ensureId("Position");
    const uint32_t rmId = ecs.components.ensureId("RenderModel");
    const uint32_t raId = ecs.components.ensureId("RenderAnimation");
    const uint32_t disabledId = ecs.components.ensureId("Disabled");
    const uint32_t deadId = ecs.components.ensureId("Dead");

    Engine::ECS::ComponentMask required;
    required.set(posId);
    required.set(rmId);
    required.set(raId);

    Engine::ECS::ComponentMask excluded;
    excluded.set(disabledId);
    excluded.set(deadId);

    // Project entities to screen; pick closest to cursor within a small radius.
    const glm::mat4 view = m_camera.GetViewMatrix();
    const glm::mat4 proj = m_camera.GetProjectionMatrix();
    const glm::mat4 vp = proj * view;

    constexpr float kPickRadiusPx = 50.0f;
    const float bestRadius2 = kPickRadiusPx * kPickRadiusPx;
    float bestD2 = bestRadius2;
    float bestCamD2 = std::numeric_limits<float>::infinity();

    Engine::ECS::ArchetypeStore *bestStore = nullptr;
    uint32_t bestRow = 0;

    static Engine::ECS::QueryId pickQueryId = Engine::ECS::QueryManager::InvalidQuery;
    if (pickQueryId == Engine::ECS::QueryManager::InvalidQuery)
        pickQueryId = ecs.queries.createQuery(required, excluded, ecs.stores);

    const auto &q = ecs.queries.get(pickQueryId);
    for (uint32_t archetypeId : q.matchingArchetypeIds)
    {
        Engine::ECS::ArchetypeStore *storePtr = ecs.stores.get(archetypeId);
        if (!storePtr)
            continue;
        auto &store = *storePtr;
        if (!store.hasPosition() || !store.hasRenderModel() || !store.hasRenderAnimation())
            continue;

        const auto &positions = store.positions();
        const uint32_t n = store.size();
        for (uint32_t row = 0; row < n; ++row)
        {
            const auto &p = positions[row];
            const glm::vec4 world(p.x, p.y, p.z, 1.0f);
            const glm::vec4 clip = vp * world;
            if (clip.w <= 1e-6f)
                continue;

            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f)
                continue;

            const float sx = (ndc.x * 0.5f + 0.5f) * width;
            // Camera projection already flips Y for Vulkan, so NDC Y is in the same "down is +" sense as window pixels.
            const float sy = (ndc.y * 0.5f + 0.5f) * height;

            const float dx = sx - mouseX;
            const float dy = sy - mouseY;
            const float d2 = dx * dx + dy * dy;

            const glm::vec3 camPos = m_camera.GetPosition();
            const glm::vec3 worldPos(p.x, p.y, p.z);
            const float camD2 = glm::dot(worldPos - camPos, worldPos - camPos);

            if (d2 < bestD2 || (std::abs(d2 - bestD2) < 1e-4f && camD2 < bestCamD2))
            {
                bestD2 = d2;
                bestCamD2 = camD2;
                bestStore = &store;
                bestRow = row;
            }
        }
    }

    if (bestStore)
    {
        // Clicked on an entity.
        // - Friendly: select the entire group (all living units with the same Team id).
        // - Enemy: issue an attack-move for currently-selected units toward that enemy.

        const Engine::ECS::Entity picked = bestStore->entities()[bestRow];
        const auto &pickedPos = bestStore->positions()[bestRow];

        const int playerTeam = m_systems.GetCombatSystem().humanTeamId();
        const bool havePickedTeam = bestStore->hasTeam();
        const uint8_t pickedTeam = havePickedTeam ? bestStore->teams()[bestRow].id : 0u;

        auto clearSelection = [&]()
        {
            std::vector<Engine::ECS::Entity> toClear;
            toClear.reserve(256);
            for (const auto &ptr : ecs.stores.stores())
            {
                if (!ptr)
                    continue;
                const auto &store = *ptr;
                if (!store.signature().has(selectedId))
                    continue;
                const auto &ents = store.entities();
                const uint32_t n = store.size();
                for (uint32_t row = 0; row < n; ++row)
                    toClear.push_back(ents[row]);
            }
            for (const auto &e : toClear)
                (void)ecs.removeTag(e, selectedId);
        };

        auto selectTeamGroup = [&](uint8_t teamToSelect)
        {
            clearSelection();
            std::vector<Engine::ECS::Entity> toSelect;
            toSelect.reserve(512);
            for (const auto &ptr : ecs.stores.stores())
            {
                if (!ptr)
                    continue;
                const auto &store = *ptr;
                if (!store.signature().has(posId))
                    continue;
                if (!store.signature().has(teamId))
                    continue;
                if (store.signature().has(disabledId) || store.signature().has(deadId))
                    continue;

                const auto &teams = store.teams();
                const auto &ents = store.entities();
                const uint32_t n = store.size();
                for (uint32_t row = 0; row < n; ++row)
                {
                    if (teams[row].id == teamToSelect)
                        toSelect.push_back(ents[row]);
                }
            }
            for (const auto &e : toSelect)
                (void)ecs.addTag(e, selectedId);
        };

        const bool isEnemy = (playerTeam >= 0 && havePickedTeam && pickedTeam != static_cast<uint8_t>(playerTeam));
        if (!isEnemy)
        {
            // Friendly group selection
            selectTeamGroup(pickedTeam);
        }
        else
        {
            // Enemy clicked: ensure we have a friendly group selected, then issue an attack-move.
            if (playerTeam >= 0)
                selectTeamGroup(static_cast<uint8_t>(playerTeam));

            // Use existing command pipeline (formation offsets) toward the enemy position.
            m_systems.SetGlobalMoveTarget(pickedPos.x, 0.0f, pickedPos.z);
        }
    }
    else
    {
        // Clicked on ground - move selected units to this position
        // Ray-cast from camera through cursor to ground plane (Y=0)

        // Convert mouse coords to NDC
        const float ndcX = (mouseX / width) * 2.0f - 1.0f;
        const float ndcY = (mouseY / height) * 2.0f - 1.0f;

        // Unproject near and far points
        const glm::mat4 invVP = glm::inverse(vp);
        const glm::vec4 nearClip(ndcX, ndcY, 0.0f, 1.0f); // Near plane (z=0 in NDC)
        const glm::vec4 farClip(ndcX, ndcY, 1.0f, 1.0f);  // Far plane (z=1 in NDC)

        glm::vec4 nearWorld = invVP * nearClip;
        glm::vec4 farWorld = invVP * farClip;

        if (std::abs(nearWorld.w) > 1e-6f)
            nearWorld /= nearWorld.w;
        if (std::abs(farWorld.w) > 1e-6f)
            farWorld /= farWorld.w;

        const glm::vec3 rayOrigin(nearWorld);
        const glm::vec3 rayDir = glm::normalize(glm::vec3(farWorld) - glm::vec3(nearWorld));

        // Intersect with ground plane (Y = 0)
        // Ray: P = O + t * D
        // Plane: Y = 0  =>  O.y + t * D.y = 0  =>  t = -O.y / D.y
        if (std::abs(rayDir.y) > 1e-6f)
        {
            const float t = -rayOrigin.y / rayDir.y;
            if (t > 0.0f)
            {
                const glm::vec3 hitPoint = rayOrigin + t * rayDir;

                // Send move command to selected units
                m_systems.SetGlobalMoveTarget(hitPoint.x, 0.0f, hitPoint.z);

                (void)hitPoint;
            }
        }
    }
}

void MySampleApp::ApplyRTSCamera(float aspect)
{
    // Projection stays perspective; keep it synced with window aspect.
    m_camera.SetPerspective(glm::radians(60.0f), aspect, 0.1f, 600.0f);

    // Direction from yaw/pitch.
    glm::vec3 forward;
    forward.x = std::cos(glm::radians(m_rtsCam.yawDeg)) * std::cos(glm::radians(m_rtsCam.pitchDeg));
    forward.y = std::sin(glm::radians(m_rtsCam.pitchDeg));
    forward.z = std::sin(glm::radians(m_rtsCam.yawDeg)) * std::cos(glm::radians(m_rtsCam.pitchDeg));
    forward = glm::normalize(forward);

    // Stable RTS mapping: keep a fixed slant while moving over ground.
    glm::vec3 forwardXZ{forward.x, 0.0f, forward.z};
    const float forwardLen2 = glm::dot(forwardXZ, forwardXZ);
    if (forwardLen2 > 1e-6f)
        forwardXZ *= 1.0f / std::sqrt(forwardLen2);
    else
        forwardXZ = {0.0f, 0.0f, -1.0f};

    const float backDistance = m_rtsCam.height;
    const glm::vec3 camPos = m_rtsCam.focus - forwardXZ * backDistance + glm::vec3(0.0f, m_rtsCam.height, 0.0f);

    m_camera.SetPosition(camPos);
    m_camera.SetRotation(m_rtsCam.yawDeg, m_rtsCam.pitchDeg);
}

void MySampleApp::OnRender()
{
    // Rendering handled by Renderer/Engine.
    // If ImGui frame active, draw the menu. (Application's Run begins an ImGui frame before OnRender.)
    Engine::ImGuiLayer *layer = GetImGuiLayer();
    if (!layer || !layer->isInitialized() || ImGui::GetCurrentContext() == nullptr)
        return;

    if (m_reloadMenuTextures)
    {
        // After swapchain resize, Application recreates the ImGui layer and its descriptor pool.
        // Any cached ImTextureID (VkDescriptorSet) from before the resize becomes invalid.
        auto loader = m_menu.GetTextureLoader();
        if (loader)
            m_menu.SetTextureLoader(loader);
        m_reloadMenuTextures = false;
    }

    // Apply fade-in effect to game world rendering
    if (m_menu.IsFadingToGame())
    {
        float alpha = m_menu.GetGameAlpha();
        // Apply alpha to your render pass or use a post-process overlay
        // Example: render a black quad with inverse alpha over everything
        // or multiply your fragment shader output by alpha
    }
    m_menu.OnImGuiFrame();

    // ---- Battle HUD: Team health bars ----
    if (m_inGame && !m_menu.IsVisible())
    {
        const auto &combat = m_systems.GetCombatSystem();
        const auto &teamA = combat.getTeamStats(0);
        const auto &teamB = combat.getTeamStats(1);

        ImGuiIO &io = ImGui::GetIO();
        const float screenW = io.DisplaySize.x;

        // Bar dimensions
        const float barW = 280.0f;
        const float barH = 28.0f;
        const float padding = 20.0f;
        const float topY = 16.0f;

        // Transparent overlay window covering full screen
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##BattleHUD", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();

        ImDrawList *draw = ImGui::GetWindowDrawList();

        // Helper lambda: draw a labeled health bar
        auto drawTeamBar = [&](float x, float y, const CombatSystem::TeamStats &stats,
                               const char *label, ImU32 fillColor, ImU32 bgColor)
        {
            float fraction = (stats.maxHP > 0.0f)
                                 ? std::clamp(stats.currentHP / stats.maxHP, 0.0f, 1.0f)
                                 : 0.0f;

            // Background (dark)
            draw->AddRectFilled(ImVec2(x, y), ImVec2(x + barW, y + barH), bgColor, 4.0f);
            // Fill
            draw->AddRectFilled(ImVec2(x, y), ImVec2(x + barW * fraction, y + barH), fillColor, 4.0f);
            // Border
            draw->AddRect(ImVec2(x, y), ImVec2(x + barW, y + barH),
                          IM_COL32(200, 200, 200, 200), 4.0f, 0, 1.5f);

            // Text: "Team A   5/10   350/1400"
            char buf[128];
            snprintf(buf, sizeof(buf), "%s   %d/%d   %.0f/%.0f",
                     label, stats.alive, stats.totalSpawned,
                     std::max(0.0f, stats.currentHP), stats.maxHP);

            ImVec2 textSize = ImGui::CalcTextSize(buf);
            float tx = x + (barW - textSize.x) * 0.5f;
            float ty = y + (barH - textSize.y) * 0.5f;
            // Shadow
            draw->AddText(ImVec2(tx + 1, ty + 1), IM_COL32(0, 0, 0, 200), buf);
            // Foreground
            draw->AddText(ImVec2(tx, ty), IM_COL32(255, 255, 255, 240), buf);
        };

        // Team A — top-left (blue) — PLAYER
        drawTeamBar(padding, topY, teamA, "YOU (A)",
                    IM_COL32(50, 120, 220, 220), IM_COL32(20, 40, 80, 180));

        // Team B — top-right (red) — AI
        drawTeamBar(screenW - barW - padding, topY, teamB, "Enemy (B)",
                    IM_COL32(200, 50, 50, 220), IM_COL32(80, 20, 20, 180));

        // --- Hint text ---
        {
            const char *hint = nullptr;
            ImU32 hintColor = IM_COL32(200, 200, 200, 180);

            if (!combat.isBattleStarted())
            {
                hint = "Click anywhere to start — armies will charge toward each other!";
                hintColor = IM_COL32(100, 255, 100, 240);
            }
            else
            {
                hint = "Battle in progress!";
                hintColor = IM_COL32(255, 200, 50, 255);
            }

            ImVec2 hintSize = ImGui::CalcTextSize(hint);
            float hx = (screenW - hintSize.x) * 0.5f;
            float hy = topY + barH + 12.0f;
            draw->AddText(ImVec2(hx + 1, hy + 1), IM_COL32(0, 0, 0, 180), hint);
            draw->AddText(ImVec2(hx, hy), hintColor, hint);
        }

        // --- Victory / Defeat overlay ---
        if (combat.isBattleStarted() && teamA.totalSpawned > 0 && teamB.totalSpawned > 0 && (teamA.alive == 0 || teamB.alive == 0))
        {
            const float screenH = io.DisplaySize.y;

            // Dim background
            draw->AddRectFilled(ImVec2(0, 0), ImVec2(screenW, screenH),
                                IM_COL32(0, 0, 0, 150));

            const char *resultText = nullptr;
            ImU32 resultColor = IM_COL32(255, 255, 255, 255);

            if (teamA.alive > 0 && teamB.alive == 0)
            {
                resultText = "VICTORY!";
                resultColor = IM_COL32(50, 220, 80, 255);
            }
            else if (teamB.alive > 0 && teamA.alive == 0)
            {
                resultText = "DEFEAT";
                resultColor = IM_COL32(220, 50, 50, 255);
            }
            else
            {
                resultText = "DRAW";
                resultColor = IM_COL32(200, 200, 100, 255);
            }

            // Large result text (scaled up)
            ImFont *font = ImGui::GetFont();
            const float bigSize = font->FontSize * 3.0f;

            ImVec2 textSize = font->CalcTextSizeA(bigSize, FLT_MAX, 0.0f, resultText);
            float tx = (screenW - textSize.x) * 0.5f;
            float ty = screenH * 0.35f;

            // Shadow
            draw->AddText(font, bigSize, ImVec2(tx + 3, ty + 3),
                          IM_COL32(0, 0, 0, 220), resultText);
            // Main text
            draw->AddText(font, bigSize, ImVec2(tx, ty),
                          resultColor, resultText);

            // Subtitle with survivor count
            char subtitle[128];
            if (teamA.alive > 0)
                snprintf(subtitle, sizeof(subtitle), "%d of your units survived", teamA.alive);
            else if (teamB.alive > 0)
                snprintf(subtitle, sizeof(subtitle), "%d enemy units remaining", teamB.alive);
            else
                snprintf(subtitle, sizeof(subtitle), "Both armies have fallen");

            ImVec2 subSize = ImGui::CalcTextSize(subtitle);
            float sx = (screenW - subSize.x) * 0.5f;
            float sy = ty + textSize.y + 16.0f;
            draw->AddText(ImVec2(sx + 1, sy + 1), IM_COL32(0, 0, 0, 200), subtitle);
            draw->AddText(ImVec2(sx, sy), IM_COL32(220, 220, 220, 240), subtitle);
        }

        ImGui::End();
    }

    // If menu produced a result, handle it
    if (m_menu.GetResult() != MenuManager::Result::None)
    {
        auto res = m_menu.GetResult();
        m_menu.ClearResult();

        if (res == MenuManager::Result::NewGame)
        {
            std::remove(m_saveFilePath.c_str());
            m_menu.SetHasSaveFile(false);

            m_inGame = true;

            // Start fade-in effect instead of just hiding
            m_menu.StartGameFadeIn();

            // optionally reset game state
        }
        else if (res == MenuManager::Result::ContinueGame)
        {
            if (m_menu.GetMode() == MenuManager::Mode::PauseMenu)
            {
                // Resume the current game state
                m_menu.Hide();
            }
            else
            {
                // Main menu: load from disk and enter game
                LoadGameState();
                m_inGame = true;
                m_menu.Hide();
            }
        }
        else if (res == MenuManager::Result::Exit)
        {
            std::exit(0); // Quick exit - no GPU wait, immediate termination
        }
    }
}

void MySampleApp::setupECSFromPrefabs()
{
    auto &ecs = GetECS();

    // Load all prefab definitions from JSON copied next to executable.
    // (CMake copies Sample/entities/*.json -> <build>/Sample/entities/)
    size_t prefabCount = 0;
    try
    {
        for (const auto &entry : std::filesystem::directory_iterator("entities"))
        {
            if (!entry.is_regular_file())
                continue;
            if (entry.path().extension() != ".json")
                continue;
            const std::string path = entry.path().generic_string();
            const std::string jsonText = Engine::ECS::readFileText(path);
            if (jsonText.empty())
            {
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
                std::cerr << "[Prefab] Failed to read: " << path << "\n";
#endif
                continue;
            }
            Engine::ECS::Prefab p = Engine::ECS::loadPrefabFromJson(jsonText, ecs.components, ecs.archetypes, *m_assets);
            if (p.name.empty())
            {
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
                std::cerr << "[Prefab] Missing name in: " << path << "\n";
#endif
                continue;
            }
            ecs.prefabs.add(p);
            ++prefabCount;
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
            std::cout << "[Prefab] Loaded " << p.name << " from " << path << "\n";
#endif
        }
    }
    catch (const std::exception &e)
    {
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        std::cerr << "[Prefab] Failed to enumerate entities/: " << e.what() << "\n";
#endif
        return;
    }

    if (prefabCount == 0)
    {
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        std::cerr << "[Prefab] No prefabs loaded from entities/*.json\n";
#endif
        return;
    }

    Sample::SpawnFromScenarioFile(ecs, "BattleConfig.json", /*selectSpawned=*/false);

    // --- Load combat tuning from BattleConfig.json ---
    try
    {
        std::ifstream cfgFile("BattleConfig.json");
        if (cfgFile.is_open())
        {
            nlohmann::json root = nlohmann::json::parse(cfgFile);
            if (root.contains("combat") && root["combat"].is_object())
            {
                const auto &c = root["combat"];
                CombatSystem::CombatConfig cfg;
                if (c.contains("meleeRange"))
                    cfg.meleeRange = c["meleeRange"].get<float>();
                if (c.contains("engageRange"))
                    cfg.engageRange = c["engageRange"].get<float>();
                if (c.contains("damageMin"))
                    cfg.damageMin = c["damageMin"].get<float>();
                if (c.contains("damageMax"))
                    cfg.damageMax = c["damageMax"].get<float>();
                // Legacy single-value fallback
                if (c.contains("damagePerHit") && !c.contains("damageMin"))
                {
                    float d = c["damagePerHit"].get<float>();
                    cfg.damageMin = d * 0.6f;
                    cfg.damageMax = d * 1.4f;
                }
                if (c.contains("deathRemoveDelay"))
                    cfg.deathRemoveDelay = c["deathRemoveDelay"].get<float>();
                if (c.contains("maxHPPerUnit"))
                    cfg.maxHPPerUnit = c["maxHPPerUnit"].get<float>();
                if (c.contains("missChance"))
                    cfg.missChance = c["missChance"].get<float>();
                if (c.contains("critChance"))
                    cfg.critChance = c["critChance"].get<float>();
                if (c.contains("critMultiplier"))
                    cfg.critMultiplier = c["critMultiplier"].get<float>();
                if (c.contains("rageMaxBonus"))
                    cfg.rageMaxBonus = c["rageMaxBonus"].get<float>();
                if (c.contains("cooldownJitter"))
                    cfg.cooldownJitter = c["cooldownJitter"].get<float>();
                if (c.contains("staggerMax"))
                    cfg.staggerMax = c["staggerMax"].get<float>();
                m_systems.GetCombatSystemMut().applyConfig(cfg);
                m_systems.GetCombatSystemMut().setHumanTeam(0); // Team A = human player
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
                std::cout << "[Config] Combat config loaded from BattleConfig.json\n";
#endif
            }

            // Load start zone (click here to begin battle)
            if (root.contains("startZone") && root["startZone"].is_object())
            {
                const auto &sz = root["startZone"];
                m_startZoneX = sz.value("x", 0.0f);
                m_startZoneZ = sz.value("z", 0.0f);
                m_startZoneRadius = sz.value("radius", 10.0f);
                m_hasStartZone = true;
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
                std::cout << "[Config] Start zone at (" << m_startZoneX << "," << m_startZoneZ
                          << ") r=" << m_startZoneRadius << "\n";
#endif
            }
        }
    }
    catch (const std::exception &e)
    {
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        std::cerr << "[Config] Failed to parse combat config: " << e.what() << "\n";
#endif
    }
}

void MySampleApp::OnEvent(const std::string &name)
{
    std::istringstream iss(name);
    std::string evt;
    iss >> evt;

    // If the pause menu is open, ignore gameplay mouse input.
    if (m_inGame && m_menu.IsVisible())
    {
        if (evt == "MouseButtonLeftDown" || evt == "MouseButtonLeftUp" || evt == "MouseButtonRightDown" || evt == "MouseButtonRightUp" || evt == "MouseScroll")
            return;
    }

    if (evt == "MouseButtonLeftDown")
    {
        // Left click is reserved for camera drag/pan only.
        m_isPanning = true;
        m_panJustStarted = true;
        auto &win = GetWindow();
        double mx = 0.0, my = 0.0;
        win.GetCursorPosition(mx, my);
        m_lastMouse = {static_cast<float>(mx), static_cast<float>(my)};
        return;
    }

    if (evt == "MouseButtonLeftUp")
    {
        m_isPanning = false;
        m_panJustStarted = false;
        return;
    }

    if (evt == "MouseButtonRightDown")
    {
        PickAndSelectEntityAtCursor();
        return;
    }

    if (evt == "MouseScroll")
    {
        double xoff = 0.0, yoff = 0.0;
        iss >> xoff >> yoff;
        (void)xoff;
        m_scrollDelta += static_cast<float>(yoff);
        return;
    }

    if (evt == "EscapePressed")
    {
        if (!m_inGame)
        {
            // On the main menu, ignore Escape (engine no longer force-quits).
            return;
        }

        // Toggle pause menu.
        if (m_menu.IsVisible())
        {
            m_menu.Hide();
        }
        else
        {
            m_menu.SetMode(MenuManager::Mode::PauseMenu);
            m_menu.Show();
        }
        return;
    }

    if (name == "WindowResize")
    {
        // Application handles recreating swapchain/renderer/ImGui.
        // We just mark UI textures for re-registration next render.
        m_reloadMenuTextures = true;
        return;
    }
}

void MySampleApp::SaveGameState()
{
    json j;
    j["rts_focus_x"] = m_rtsCam.focus.x;
    j["rts_focus_y"] = m_rtsCam.focus.y;
    j["rts_focus_z"] = m_rtsCam.focus.z;
    j["yawDeg"] = m_rtsCam.yawDeg;
    j["pitchDeg"] = m_rtsCam.pitchDeg;
    j["height"] = m_rtsCam.height;

    // Save window size using Engine::Window interface (available)
    j["win_w"] = static_cast<int>(GetWindow().GetWidth());
    j["win_h"] = static_cast<int>(GetWindow().GetHeight());

    // Save GLFW window position if available
    GLFWwindow *wnd = static_cast<GLFWwindow *>(GetWindow().GetWindowPointer());
    if (wnd)
    {
        int wx = 0, wy = 0;
        glfwGetWindowPos(wnd, &wx, &wy);
        j["win_x"] = wx;
        j["win_y"] = wy;
    }

    std::ofstream o(m_saveFilePath);
    if (o.good())
        o << j.dump(4);
}

void MySampleApp::LoadGameState()
{
    std::ifstream i(m_saveFilePath);
    if (!i.good())
        return;
    json j;
    i >> j;

    m_rtsCam.focus.x = j.value("rts_focus_x", m_rtsCam.focus.x);
    m_rtsCam.focus.y = j.value("rts_focus_y", m_rtsCam.focus.y);
    m_rtsCam.focus.z = j.value("rts_focus_z", m_rtsCam.focus.z);
    m_rtsCam.yawDeg = j.value("yawDeg", m_rtsCam.yawDeg);
    m_rtsCam.pitchDeg = j.value("pitchDeg", m_rtsCam.pitchDeg);
    m_rtsCam.height = j.value("height", m_rtsCam.height);

    // Re-apply camera projection with current window aspect
    auto &win = GetWindow();
    const float aspect = static_cast<float>(win.GetWidth()) / static_cast<float>(win.GetHeight());
    ApplyRTSCamera(aspect);

    // Note: we intentionally do NOT call GLFW-specific functions to set window position/size here,
    // because the Engine::Window interface in this repo does not provide setters for position,
    // and directly depending on GLFW types in Sample sources caused the previous undefined-identifier issues.

    // Restore window position if saved (use GLFW directly via the window pointer)
    GLFWwindow *wnd = static_cast<GLFWwindow *>(GetWindow().GetWindowPointer());
    if (wnd)
    {
        int winx = j.value("win_x", INT32_MIN);
        int winy = j.value("win_y", INT32_MIN);
        if (winx != INT32_MIN && winy != INT32_MIN)
        {
            glfwSetWindowPos(wnd, winx, winy);
        }
    }
}

bool MySampleApp::HasSaveFile() const
{
    std::ifstream f(m_saveFilePath);
    return f.good();
}

// Expose the texture loader so callers can query it.