#include "Engine/VulkanContext.h"
#include "Engine/Window.h"
#include "Engine/SwapChain.h"
#include "utils/VulkanValidationUtils.h"
#include <GLFW/glfw3.h> // for glfwCreateWindowSurface
#include <iostream>
#include <vector>
#include <set>
#include <stdexcept>
#include <algorithm>
#include <cstring>

static const std::vector<const char *> requiredDeviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME};

// Optional extensions to enable if supported
static const std::vector<const char *> optionalDeviceExtensions = {
    VK_EXT_MEMORY_BUDGET_EXTENSION_NAME};

namespace Engine
{

    VulkanContext::VulkanContext(Window &window) : m_Window(window)
    {
        // constructor does not init the instance automatically; explicit Init() call pattern can be used
        Init();
    }

    VulkanContext::~VulkanContext()
    {
        Shutdown();
    }

    void VulkanContext::Init()
    {
        createInstance();
        createSurface();
        pickPhysicalDeviceForPresentation();
        createLogicalDevice();

        m_SwapChain = std::make_unique<SwapChain>(
            m_Device,
            m_SelectedDeviceInfo.physicalDevice,
            m_Surface,
            m_SelectedDeviceInfo.queueFamilyIndices,
            VkExtent2D{m_Window.GetWidth(), m_Window.GetHeight()});
        m_SwapChain->Init();
    }

    void VulkanContext::Shutdown()
    {
        if (m_SwapChain)
        {
            m_SwapChain->Cleanup();
            m_SwapChain.reset();
        }
        if (m_Device != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(m_Device);
        }

        // Destroy device first (this will free device-local resources)
        if (m_Device != VK_NULL_HANDLE)
        {
            vkDestroyDevice(m_Device, nullptr);
            m_Device = VK_NULL_HANDLE;
        }

        if (m_Surface != VK_NULL_HANDLE)
        {
            vkDestroySurfaceKHR(m_Instance, m_Surface, nullptr);
            m_Surface = VK_NULL_HANDLE;
        }
        if (m_Instance != VK_NULL_HANDLE)
        {
            if (m_DebugMessenger != VK_NULL_HANDLE)
            {
                DestroyDebugUtilsMessengerEXT(m_Instance, m_DebugMessenger, nullptr);
                m_DebugMessenger = VK_NULL_HANDLE;
            }
            vkDestroyInstance(m_Instance, nullptr);
            m_Instance = VK_NULL_HANDLE;
        }
    }

    bool VulkanContext::checkValidationLayerSupport()
    {
        const char *layerName = "VK_LAYER_KHRONOS_validation";
        uint32_t layerCount = 0;
        if (vkEnumerateInstanceLayerProperties(&layerCount, nullptr) != VK_SUCCESS)
        {
            return false;
        }
        std::vector<VkLayerProperties> availableLayers(layerCount);
        if (layerCount > 0)
        {
            vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());
        }
        for (const auto &layerProp : availableLayers)
        {
            if (std::strcmp(layerProp.layerName, layerName) == 0)
            {
                return true;
            }
        }
        return false;
    }

    void VulkanContext::populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT &createInfo)
    {
        std::memset(&createInfo, 0, sizeof(createInfo));
        createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        createInfo.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        createInfo.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        createInfo.pfnUserCallback = debugCallback;
        createInfo.pUserData = nullptr;
    }

    void VulkanContext::createInstance()
    {
        uint32_t extCount = 0;
        const char **glfwExts = glfwGetRequiredInstanceExtensions(&extCount);
        std::vector<const char *> extensions(glfwExts, glfwExts + extCount);

        // Enable validation layers in debug builds only
#ifndef NDEBUG
        const bool enableValidationLayers = true;
#else
        const bool enableValidationLayers = false;
#endif

        // If validation is enabled, request the debug utils extension so we can create a messenger.
        if (enableValidationLayers)
        {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "MyEngine";
        appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.pEngineName = "MyEngine";
        appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.apiVersion = VK_API_VERSION_1_0;

        VkInstanceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &appInfo;
        ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        ci.ppEnabledExtensionNames = extensions.data();

        // Validation layers
        std::vector<const char *> layers;
        if (enableValidationLayers)
        {
            const char *validationLayerName = "VK_LAYER_KHRONOS_validation";
            if (!checkValidationLayerSupport())
            {
                throw std::runtime_error("Validation layer requested but not available");
            }
            layers.push_back(validationLayerName);
            ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
            ci.ppEnabledLayerNames = layers.data();
        }
        else
        {
            ci.enabledLayerCount = 0;
            ci.ppEnabledLayerNames = nullptr;
        }

        // If validation is enabled, chain the debug messenger create info into pNext so we receive
        // messages produced during vkCreateInstance as well.
        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
        if (enableValidationLayers)
        {
            populateDebugMessengerCreateInfo(debugCreateInfo);
            ci.pNext = reinterpret_cast<const void *>(&debugCreateInfo);
        }
        else
        {
            ci.pNext = nullptr;
        }

        VkResult res = vkCreateInstance(&ci, nullptr, &m_Instance);
        if (res != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create Vulkan instance");
        }

        // Create the debug messenger for runtime callbacks (after instance creation)
        if (enableValidationLayers)
        {
            populateDebugMessengerCreateInfo(debugCreateInfo);
            if (CreateDebugUtilsMessengerEXT(m_Instance, &debugCreateInfo, nullptr, &m_DebugMessenger) != VK_SUCCESS)
            {
                std::cerr << "Warning: failed to set up debug messenger!" << std::endl;
                m_DebugMessenger = VK_NULL_HANDLE;
            }
        }
    }

    void VulkanContext::createSurface()
    {
        void *w = m_Window.GetWindowPointer();
        if (!w)
        {
            throw std::runtime_error("VulkanContext::createSurface - window handle is null");
        }
        GLFWwindow *window = reinterpret_cast<GLFWwindow *>(w);
        VkResult result = glfwCreateWindowSurface(m_Instance, window, nullptr, &m_Surface);
        if (result != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create window surface via GLFW");
        }
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        std::cout << "Vulkan surface created successfully." << std::endl;
#endif
    }

    void VulkanContext::pickPhysicalDeviceForPresentation()
    {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(m_Instance, &deviceCount, nullptr);
        if (deviceCount == 0)
        {
            throw std::runtime_error("Failed to find GPUs with Vulkan support");
        }

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(m_Instance, &deviceCount, devices.data());

        // Evaluate devices and pick the first suitable one
        for (const auto &device : devices)
        {
            QueueFamilyIndices indices = findQueueFamiliesForPresentation(device);

            bool swapchainAdequate = false;
            SwapChainSupportDetails swapDetails = querySwapChainSupport(device);
            swapchainAdequate = !swapDetails.formats.empty() && !swapDetails.presentModes.empty();

            if (indices.isComplete() && swapchainAdequate)
            {
                m_SelectedDeviceInfo = {device, indices};
                return;
            }
        }

        throw std::runtime_error("Failed to find a suitable GPU (no device met requirements)");
    }

    QueueFamilyIndices VulkanContext::findQueueFamiliesForPresentation(VkPhysicalDevice device) const
    {
        QueueFamilyIndices indices;

        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

        int i = 0;
        for (const auto &qf : queueFamilies)
        {
            // Check for graphics capability
            if (qf.queueFlags & VK_QUEUE_GRAPHICS_BIT)
            {
                indices.graphicsFamily = static_cast<uint32_t>(i);
            }

            // Check for presentation support to our surface
            VkBool32 presentSupport = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, static_cast<uint32_t>(i), m_Surface, &presentSupport);
            if (presentSupport)
            {
                indices.presentFamily = static_cast<uint32_t>(i);
            }

            if (indices.isComplete())
                break;
            ++i;
        }

        return indices;
    }

    VulkanContext::SwapChainSupportDetails VulkanContext::querySwapChainSupport(VkPhysicalDevice device) const
    {
        SwapChainSupportDetails details;

        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, m_Surface, &details.capabilities);

        uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_Surface, &formatCount, nullptr);
        if (formatCount != 0)
        {
            details.formats.resize(formatCount);
            vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_Surface, &formatCount, details.formats.data());
        }

        uint32_t presentModeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_Surface, &presentModeCount, nullptr);
        if (presentModeCount != 0)
        {
            details.presentModes.resize(presentModeCount);
            vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_Surface, &presentModeCount, details.presentModes.data());
        }

        return details;
    }

    void VulkanContext::createLogicalDevice()
    {
        if (m_SelectedDeviceInfo.physicalDevice == VK_NULL_HANDLE)
        {
            throw std::runtime_error("createLogicalDevice called without a selected physical device");
        }

        // Ensure we have queue family indices
        QueueFamilyIndices indices = m_SelectedDeviceInfo.queueFamilyIndices;
        if (!indices.isComplete())
        {
            throw std::runtime_error("Queue families are not complete for logical device creation");
        }

        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        std::set<uint32_t> uniqueQueueFamilies;
        uniqueQueueFamilies.insert(indices.graphicsFamily.value());
        uniqueQueueFamilies.insert(indices.presentFamily.value());

        float queuePriority = 1.0f;
        for (uint32_t queueFamily : uniqueQueueFamilies)
        {
            VkDeviceQueueCreateInfo queueCreate{};
            queueCreate.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueCreate.queueFamilyIndex = queueFamily;
            queueCreate.queueCount = 1;
            queueCreate.pQueuePriorities = &queuePriority;
            queueCreateInfos.push_back(queueCreate);
        }

        // (Optional) request device features here
        VkPhysicalDeviceFeatures deviceFeatures{};
        // deviceFeatures.samplerAnisotropy = VK_TRUE; // enable if needed

        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
        createInfo.pQueueCreateInfos = queueCreateInfos.data();
        createInfo.pEnabledFeatures = &deviceFeatures;

        // Build device extension list: required + available optional extensions
        std::vector<const char *> enabledExtensions(requiredDeviceExtensions.begin(),
                                                    requiredDeviceExtensions.end());
        {
            uint32_t extCount = 0;
            vkEnumerateDeviceExtensionProperties(m_SelectedDeviceInfo.physicalDevice,
                                                 nullptr, &extCount, nullptr);
            std::vector<VkExtensionProperties> availableExts(extCount);
            vkEnumerateDeviceExtensionProperties(m_SelectedDeviceInfo.physicalDevice,
                                                 nullptr, &extCount, availableExts.data());
            for (const char *optExt : optionalDeviceExtensions)
            {
                for (const auto &avail : availableExts)
                {
                    if (std::strcmp(avail.extensionName, optExt) == 0)
                    {
                        enabledExtensions.push_back(optExt);
                        std::cout << "[Vulkan] Enabling optional extension: " << optExt << "\n";
                        break;
                    }
                }
            }
        }

        // Device extensions
        createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
        createInfo.ppEnabledExtensionNames = enabledExtensions.data();

        // No device layers (deprecated), validation layers enabled at instance level if desired
        createInfo.enabledLayerCount = 0;

        VkResult result = vkCreateDevice(m_SelectedDeviceInfo.physicalDevice, &createInfo, nullptr, &m_Device);
        if (result != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create logical device");
        }
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        std::cout << "Logical device created\n";
#endif

        // Retrieve queue handles
        vkGetDeviceQueue(m_Device, indices.graphicsFamily.value(), 0, &m_GraphicsQueue);
        vkGetDeviceQueue(m_Device, indices.presentFamily.value(), 0, &m_PresentQueue);

#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        std::cout << "Graphics queue and Present queue retrieved\n";
#endif
    }
}
