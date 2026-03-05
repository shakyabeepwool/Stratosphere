#include "Engine/PerformanceMonitor.h"
#include "Engine/VulkanContext.h"
#include "Engine/Renderer.h"
#include "Engine/Window.h"

#include <imgui.h>
#include <algorithm>
#include <numeric>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/processor_info.h>
#include <mach/mach_host.h>
#include <sys/sysctl.h>
#endif

namespace Engine
{
    // -----------------------------------------------------------------------
    // Helpers (Windows)
    // -----------------------------------------------------------------------
#ifdef _WIN32
    static uint64_t fileTimeToU64(const FILETIME &ft)
    {
        return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    }
#endif

    // -----------------------------------------------------------------------
    // Global atomic draw call counter
    // -----------------------------------------------------------------------
    static std::atomic<uint32_t> g_drawCallCount{0};

    void DrawCallCounter::increment(uint32_t count)
    {
        g_drawCallCount.fetch_add(count, std::memory_order_relaxed);
    }

    void DrawCallCounter::reset()
    {
        g_drawCallCount.store(0, std::memory_order_relaxed);
    }

    uint32_t DrawCallCounter::get()
    {
        return g_drawCallCount.load(std::memory_order_relaxed);
    }

    // -----------------------------------------------------------------------
    // Ctor / Dtor
    // -----------------------------------------------------------------------
    PerformanceMonitor::PerformanceMonitor()
        : m_frameStart(Clock::now()), m_lastFrameEnd(Clock::now())
    {
    }

    PerformanceMonitor::~PerformanceMonitor()
    {
        cleanup();
    }

    // -----------------------------------------------------------------------
    // Init / Cleanup
    // -----------------------------------------------------------------------
    void PerformanceMonitor::init(VulkanContext *ctx, Renderer *renderer, Window *window)
    {
        m_ctx = ctx;
        m_renderer = renderer;
        m_window = window;
        m_initialized = true;
        m_frameTimeHistory.clear();

        querySystemInfo(); // GPU name, total VRAM, DXGI adapter, initial CPU times
    }

    void PerformanceMonitor::cleanup()
    {
        m_initialized = false;
        m_frameTimeHistory.clear();
    }

    // -----------------------------------------------------------------------
    // One-time system info query (called from init)
    // -----------------------------------------------------------------------
    void PerformanceMonitor::querySystemInfo()
    {
        if (!m_ctx)
            return;

        // --- GPU name from Vulkan ---
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_ctx->GetPhysicalDevice(), &props);
        m_gpuName = props.deviceName;

        // --- Total VRAM from Vulkan memory properties ---
        VkPhysicalDeviceMemoryProperties memProps{};
        vkGetPhysicalDeviceMemoryProperties(m_ctx->GetPhysicalDevice(), &memProps);
        VkDeviceSize totalDeviceLocal = 0;
        for (uint32_t i = 0; i < memProps.memoryHeapCount; i++)
        {
            if (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            {
                totalDeviceLocal += memProps.memoryHeaps[i].size;
            }
        }
        m_vramTotalMB = static_cast<float>(totalDeviceLocal) / (1024.0f * 1024.0f);

        // --- Check VK_EXT_memory_budget support ---
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(m_ctx->GetPhysicalDevice(), nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateDeviceExtensionProperties(m_ctx->GetPhysicalDevice(), nullptr, &extCount, exts.data());
        for (const auto &ext : exts)
        {
            if (std::strcmp(ext.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0)
            {
                m_hasMemoryBudget = true;
                break;
            }
        }
        if (m_hasMemoryBudget)
        {
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
            std::cout << "[PerfMon] VK_EXT_memory_budget available — cross-platform VRAM tracking enabled\n";
#endif
        }
        else
        {
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
            std::cout << "[PerfMon] VK_EXT_memory_budget not available — VRAM usage will show N/A\n";
#endif
        }

        // Do an initial VRAM query to get total from budget
        queryVramViaVulkan();

        // --- Seed platform-specific CPU tracking ---
#ifdef _WIN32
        FILETIME idle, kernel, user;
        if (GetSystemTimes(&idle, &kernel, &user))
        {
            m_prevIdleTime = fileTimeToU64(idle);
            m_prevKernelTime = fileTimeToU64(kernel);
            m_prevUserTime = fileTimeToU64(user);
        }
#elif defined(__linux__)
        // Seed /proc/stat CPU tracking
        std::ifstream statFile("/proc/stat");
        if (statFile.is_open())
        {
            std::string line;
            std::getline(statFile, line);
            std::istringstream iss(line);
            std::string cpu;
            uint64_t userT, nice, system, idle, iowait, irq, softirq, steal;
            iss >> cpu >> userT >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
            m_prevCpuIdle = idle + iowait;
            m_prevCpuTotal = userT + nice + system + idle + iowait + irq + softirq + steal;
        }
#endif
    }

    // -----------------------------------------------------------------------
    // Cross-platform VRAM query via VK_EXT_memory_budget
    // -----------------------------------------------------------------------
    void PerformanceMonitor::queryVramViaVulkan()
    {
        if (!m_ctx || !m_hasMemoryBudget)
            return;

        VkPhysicalDeviceMemoryBudgetPropertiesEXT budgetProps{};
        budgetProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;

        VkPhysicalDeviceMemoryProperties2 memProps2{};
        memProps2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        memProps2.pNext = &budgetProps;

        vkGetPhysicalDeviceMemoryProperties2(m_ctx->GetPhysicalDevice(), &memProps2);

        // Sum device-local heaps
        VkDeviceSize totalBudget = 0;
        VkDeviceSize totalUsage = 0;
        for (uint32_t i = 0; i < memProps2.memoryProperties.memoryHeapCount; i++)
        {
            if (memProps2.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            {
                totalBudget += budgetProps.heapBudget[i];
                totalUsage += budgetProps.heapUsage[i];
            }
        }

        m_vramUsedMB = static_cast<float>(totalUsage) / (1024.0f * 1024.0f);
        // Keep m_vramTotalMB as the physical GPU VRAM (set once in querySystemInfo)
    }

    // -----------------------------------------------------------------------
    // Per-frame system metrics (VRAM used, CPU %, RAM)
    // -----------------------------------------------------------------------
    void PerformanceMonitor::updateSystemMetrics()
    {
        // --- VRAM usage via VK_EXT_memory_budget (cross-platform) ---
        queryVramViaVulkan();

        // --- Platform-specific CPU and RAM metrics ---
#ifdef _WIN32
        // System-wide CPU usage via GetSystemTimes
        {
            FILETIME idle, kernel, user;
            if (GetSystemTimes(&idle, &kernel, &user))
            {
                uint64_t curIdle = fileTimeToU64(idle);
                uint64_t curKernel = fileTimeToU64(kernel);
                uint64_t curUser = fileTimeToU64(user);

                uint64_t idleDiff = curIdle - m_prevIdleTime;
                uint64_t kernelDiff = curKernel - m_prevKernelTime;
                uint64_t userDiff = curUser - m_prevUserTime;
                uint64_t totalSys = kernelDiff + userDiff;

                if (totalSys > 0)
                {
                    m_cpuUsagePercent =
                        (1.0f - static_cast<float>(idleDiff) / static_cast<float>(totalSys)) * 100.0f;
                }

                m_prevIdleTime = curIdle;
                m_prevKernelTime = curKernel;
                m_prevUserTime = curUser;
            }
        }

        // Process RAM (working set)
        {
            PROCESS_MEMORY_COUNTERS pmc{};
            pmc.cb = sizeof(pmc);
            if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
            {
                m_ramUsedMB = static_cast<float>(pmc.WorkingSetSize) / (1024.0f * 1024.0f);
            }
        }

#elif defined(__linux__)
        // System-wide CPU usage via /proc/stat
        {
            std::ifstream statFile("/proc/stat");
            if (statFile.is_open())
            {
                std::string line;
                std::getline(statFile, line);
                std::istringstream iss(line);
                std::string cpu;
                uint64_t userT, nice, system, idle, iowait, irq, softirq, steal;
                iss >> cpu >> userT >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;

                uint64_t curIdle = idle + iowait;
                uint64_t curTotal = userT + nice + system + idle + iowait + irq + softirq + steal;

                uint64_t totalDiff = curTotal - m_prevCpuTotal;
                uint64_t idleDiff = curIdle - m_prevCpuIdle;

                if (totalDiff > 0)
                {
                    m_cpuUsagePercent =
                        (1.0f - static_cast<float>(idleDiff) / static_cast<float>(totalDiff)) * 100.0f;
                }

                m_prevCpuTotal = curTotal;
                m_prevCpuIdle = curIdle;
            }
        }

        // Process RAM via /proc/self/status (VmRSS)
        {
            std::ifstream statusFile("/proc/self/status");
            if (statusFile.is_open())
            {
                std::string line;
                while (std::getline(statusFile, line))
                {
                    if (line.rfind("VmRSS:", 0) == 0)
                    {
                        std::istringstream iss(line);
                        std::string label;
                        uint64_t kb;
                        iss >> label >> kb; // "VmRSS:" then value in kB
                        m_ramUsedMB = static_cast<float>(kb) / 1024.0f;
                        break;
                    }
                }
            }
        }

#elif defined(__APPLE__)
        // System-wide CPU usage via host_processor_info (Mach)
        {
            natural_t numCPUs = 0;
            processor_info_array_t cpuInfo = nullptr;
            mach_msg_type_number_t numCpuInfo = 0;

            kern_return_t kr = host_processor_info(
                mach_host_self(), PROCESSOR_CPU_LOAD_INFO,
                &numCPUs, &cpuInfo, &numCpuInfo);

            if (kr == KERN_SUCCESS && numCPUs > 0)
            {
                uint64_t totalUser = 0, totalSystem = 0, totalIdle = 0, totalNice = 0;
                for (natural_t i = 0; i < numCPUs; i++)
                {
                    totalUser += cpuInfo[CPU_STATE_MAX * i + CPU_STATE_USER];
                    totalSystem += cpuInfo[CPU_STATE_MAX * i + CPU_STATE_SYSTEM];
                    totalIdle += cpuInfo[CPU_STATE_MAX * i + CPU_STATE_IDLE];
                    totalNice += cpuInfo[CPU_STATE_MAX * i + CPU_STATE_NICE];
                }
                uint64_t total = totalUser + totalSystem + totalIdle + totalNice;
                if (total > 0)
                {
                    m_cpuUsagePercent =
                        static_cast<float>(totalUser + totalSystem + totalNice) /
                        static_cast<float>(total) * 100.0f;
                }

                vm_deallocate(mach_task_self(),
                              reinterpret_cast<vm_address_t>(cpuInfo),
                              numCpuInfo * sizeof(integer_t));
            }
        }

        // Process RAM via task_info (Mach)
        {
            mach_task_basic_info_data_t info{};
            mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
            kern_return_t kr = task_info(
                mach_task_self(), MACH_TASK_BASIC_INFO,
                reinterpret_cast<task_info_t>(&info), &count);
            if (kr == KERN_SUCCESS)
            {
                m_ramUsedMB = static_cast<float>(info.resident_size) / (1024.0f * 1024.0f);
            }
        }
#endif
    }

    // -----------------------------------------------------------------------
    // Frame begin / end
    // -----------------------------------------------------------------------
    void PerformanceMonitor::beginFrame()
    {
        m_frameStart = Clock::now();
        DrawCallCounter::reset();
    }

    void PerformanceMonitor::endFrame()
    {
        auto now = Clock::now();

        // Calculate frame time
        float frameTimeMs = std::chrono::duration<float, std::milli>(now - m_lastFrameEnd).count();
        m_lastFrameEnd = now;

        // CPU time is the time spent between beginFrame and endFrame
        m_cpuTimeMs = std::chrono::duration<float, std::milli>(now - m_frameStart).count();

        // Store frame time in history
        m_frameTimeHistory.push_back(frameTimeMs);
        if (m_frameTimeHistory.size() > HISTORY_SIZE)
        {
            m_frameTimeHistory.pop_front();
        }

        // Get draw call count from global counter
        m_lastFrameDrawCalls = DrawCallCounter::get();

        // Get GPU time from renderer if available
        if (m_renderer)
        {
            m_gpuTimeMs = m_renderer->getGpuTimeMs();
        }

        // Apply EMA (Exponential Moving Average) smoothing for display values
        m_smoothedFrameTimeMs = EMA_SMOOTHING_FACTOR * frameTimeMs +
                                (1.0f - EMA_SMOOTHING_FACTOR) * m_smoothedFrameTimeMs;
        m_smoothedCpuTimeMs = EMA_SMOOTHING_FACTOR * m_cpuTimeMs +
                              (1.0f - EMA_SMOOTHING_FACTOR) * m_smoothedCpuTimeMs;
        m_smoothedGpuTimeMs = EMA_SMOOTHING_FACTOR * m_gpuTimeMs +
                              (1.0f - EMA_SMOOTHING_FACTOR) * m_smoothedGpuTimeMs;

        // Update FPS metrics periodically (100ms)
        m_updateTimer += frameTimeMs / 1000.0f;
        if (m_updateTimer >= UPDATE_INTERVAL)
        {
            updateMetrics();
            m_updateTimer = 0.0f;
        }

        // Update system metrics less frequently (500ms) to reduce overhead
        m_sysUpdateTimer += frameTimeMs / 1000.0f;
        if (m_sysUpdateTimer >= SYS_UPDATE_INTERVAL)
        {
            updateSystemMetrics();

            // Smooth VRAM and CPU % so the overlay doesn't jump around
            m_smoothedVramUsedMB = EMA_SMOOTHING_FACTOR * m_vramUsedMB +
                                   (1.0f - EMA_SMOOTHING_FACTOR) * m_smoothedVramUsedMB;
            m_smoothedCpuUsagePercent = EMA_SMOOTHING_FACTOR * m_cpuUsagePercent +
                                        (1.0f - EMA_SMOOTHING_FACTOR) * m_smoothedCpuUsagePercent;
            m_sysUpdateTimer = 0.0f;
        }

        m_frameTimeMs = frameTimeMs;
    }

    void PerformanceMonitor::recordDrawCall(uint32_t primitiveCount)
    {
        DrawCallCounter::increment(1);
        m_primitiveCount += primitiveCount;
    }

    void PerformanceMonitor::resetDrawCalls()
    {
        DrawCallCounter::reset();
        m_primitiveCount = 0;
    }

    void PerformanceMonitor::toggle()
    {
        m_visible = !m_visible;
    }

    void PerformanceMonitor::updateMetrics()
    {
        if (m_frameTimeHistory.empty())
            return;

        // Calculate average FPS
        float totalTime = 0.0f;
        for (float t : m_frameTimeHistory)
        {
            totalTime += t;
        }
        float avgFrameTime = totalTime / static_cast<float>(m_frameTimeHistory.size());
        m_avgFPS = (avgFrameTime > 0.0f) ? (1000.0f / avgFrameTime) : 0.0f;

        // Calculate percentile FPS
        calculatePercentileFPS();
    }

    void PerformanceMonitor::calculatePercentileFPS()
    {
        if (m_frameTimeHistory.size() < 10)
        {
            m_1percentLowFPS = m_avgFPS;
            m_01percentLowFPS = m_avgFPS;
            return;
        }

        // Copy and sort frame times (descending - longest times first = worst frames)
        std::vector<float> sortedTimes(m_frameTimeHistory.begin(), m_frameTimeHistory.end());
        std::sort(sortedTimes.begin(), sortedTimes.end(), std::greater<float>());

        // 1% low = average of worst 1% of frames
        size_t onePercentCount = std::max(static_cast<size_t>(1), sortedTimes.size() / 100);
        float sum1Percent = 0.0f;
        for (size_t i = 0; i < onePercentCount; ++i)
        {
            sum1Percent += sortedTimes[i];
        }
        float avg1PercentTime = sum1Percent / static_cast<float>(onePercentCount);
        m_1percentLowFPS = (avg1PercentTime > 0.0f) ? (1000.0f / avg1PercentTime) : 0.0f;

        // 0.1% low = the single worst frame (or average of worst 0.1%)
        size_t point1PercentCount = std::max(static_cast<size_t>(1), sortedTimes.size() / 1000);
        float sum01Percent = 0.0f;
        for (size_t i = 0; i < point1PercentCount; ++i)
        {
            sum01Percent += sortedTimes[i];
        }
        float avg01PercentTime = sum01Percent / static_cast<float>(point1PercentCount);
        m_01percentLowFPS = (avg01PercentTime > 0.0f) ? (1000.0f / avg01PercentTime) : 0.0f;
    }

    uint32_t PerformanceMonitor::getResolutionWidth() const
    {
        if (m_window)
            return m_window->GetWidth();
        return 0;
    }

    uint32_t PerformanceMonitor::getResolutionHeight() const
    {
        if (m_window)
            return m_window->GetHeight();
        return 0;
    }

    void PerformanceMonitor::renderOverlay()
    {
        if (!m_visible || !m_initialized)
            return;

        // Set up overlay window flags
        ImGuiWindowFlags windowFlags =
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoMove;

        // Position in top-right corner with padding
        const float padding = 10.0f;
        const ImGuiViewport *viewport = ImGui::GetMainViewport();
        ImVec2 workPos = viewport->WorkPos;
        ImVec2 workSize = viewport->WorkSize;
        ImVec2 windowPos(workPos.x + workSize.x - padding, workPos.y + padding);
        ImVec2 windowPivot(1.0f, 0.0f);
        ImGui::SetNextWindowPos(windowPos, ImGuiCond_Always, windowPivot);
        ImGui::SetNextWindowBgAlpha(0.75f);

        if (ImGui::Begin("Performance Monitor", nullptr, windowFlags))
        {
            // Title with styling
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.9f, 0.4f, 1.0f));
            ImGui::Text("Performance Monitor");
            ImGui::PopStyleColor();
            ImGui::Separator();

            // --- GPU Name ---
            if (!m_gpuName.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.85f, 1.0f, 1.0f));
                ImGui::Text("GPU: %s", m_gpuName.c_str());
                ImGui::PopStyleColor();
                ImGui::Spacing();
            }

            // --- FPS Section ---
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.4f, 1.0f));
            ImGui::Text("FPS");
            ImGui::PopStyleColor();

            // Color-code FPS based on performance
            ImVec4 fpsColor;
            if (m_avgFPS >= 60.0f)
                fpsColor = ImVec4(0.4f, 1.0f, 0.4f, 1.0f); // Green
            else if (m_avgFPS >= 30.0f)
                fpsColor = ImVec4(1.0f, 1.0f, 0.4f, 1.0f); // Yellow
            else
                fpsColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f); // Red

            ImGui::PushStyleColor(ImGuiCol_Text, fpsColor);
            ImGui::Text("  Average: %.1f", m_avgFPS);
            ImGui::PopStyleColor();
            ImGui::Text("  1%% Low:  %.1f", m_1percentLowFPS);
            ImGui::Text("  0.1%% Low: %.1f", m_01percentLowFPS);

            ImGui::Spacing();

            // --- Frame Time Section ---
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.4f, 1.0f));
            ImGui::Text("Frame Time");
            ImGui::PopStyleColor();
            ImGui::Text("  Frame: %.2f ms", m_smoothedFrameTimeMs);
            ImGui::Text("  CPU:   %.2f ms", m_smoothedCpuTimeMs);

            if (m_gpuTimeMs > 0.0f)
            {
                ImGui::Text("  GPU:   %.2f ms", m_smoothedGpuTimeMs);
            }
            else
            {
                ImGui::TextDisabled("  GPU:   N/A");
            }

            ImGui::Spacing();

            // --- VRAM Section ---
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.4f, 1.0f));
            ImGui::Text("VRAM");
            ImGui::PopStyleColor();

            if (m_vramTotalMB > 0.0f)
            {
                float usedDisplay = (m_smoothedVramUsedMB > 0.0f) ? m_smoothedVramUsedMB : m_vramUsedMB;
                if (usedDisplay > 0.0f)
                {
                    float pct = (usedDisplay / m_vramTotalMB) * 100.0f;
                    // Color-code VRAM: green < 60%, yellow 60-85%, red > 85%
                    ImVec4 vramColor;
                    if (pct < 60.0f)
                        vramColor = ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
                    else if (pct < 85.0f)
                        vramColor = ImVec4(1.0f, 1.0f, 0.4f, 1.0f);
                    else
                        vramColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);

                    ImGui::PushStyleColor(ImGuiCol_Text, vramColor);
                    ImGui::Text("  Used: %.0f / %.0f MB (%.0f%%)",
                                usedDisplay, m_vramTotalMB, pct);
                    ImGui::PopStyleColor();
                }
                else
                {
                    ImGui::Text("  Total: %.0f MB", m_vramTotalMB);
                    ImGui::TextDisabled("  Used:  N/A");
                }
            }
            else
            {
                ImGui::TextDisabled("  N/A");
            }

            ImGui::Spacing();

            // --- CPU Usage Section ---
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.4f, 1.0f));
            ImGui::Text("System");
            ImGui::PopStyleColor();

            {
                float cpuDisplay = (m_smoothedCpuUsagePercent > 0.0f)
                                       ? m_smoothedCpuUsagePercent
                                       : m_cpuUsagePercent;
                if (cpuDisplay > 0.0f)
                {
                    ImVec4 cpuColor;
                    if (cpuDisplay < 50.0f)
                        cpuColor = ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
                    else if (cpuDisplay < 80.0f)
                        cpuColor = ImVec4(1.0f, 1.0f, 0.4f, 1.0f);
                    else
                        cpuColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);

                    ImGui::PushStyleColor(ImGuiCol_Text, cpuColor);
                    ImGui::Text("  CPU:  %.1f%%", cpuDisplay);
                    ImGui::PopStyleColor();
                }
                else
                {
                    ImGui::TextDisabled("  CPU:  N/A");
                }
            }

            if (m_ramUsedMB > 0.0f)
            {
                ImGui::Text("  RAM:  %.0f MB", m_ramUsedMB);
            }

            ImGui::Spacing();

            // --- Resolution & Display ---
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.4f, 1.0f));
            ImGui::Text("Display");
            ImGui::PopStyleColor();
            ImGui::Text("  Resolution: %ux%u", getResolutionWidth(), getResolutionHeight());

            ImGui::Spacing();

            // --- Draw Calls Section ---
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.4f, 1.0f));
            ImGui::Text("Rendering");
            ImGui::PopStyleColor();
            ImGui::Text("  Draw Calls: %u", m_lastFrameDrawCalls);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextDisabled("Press F1 to toggle");
        }
        ImGui::End();
    }

} // namespace Engine
