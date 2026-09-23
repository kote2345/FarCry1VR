#ifndef CRY_VR_H
#define CRY_VR_H

#include "openxr_api.h"
#include <vector>

namespace CryVR
{
// Android Activity is handed off by GameActivity before SDL starts its native
// thread; SDL's Android getters are not reliable before SDL initialization.
void GetAndroidOpenXRContext(void** jniEnv, void** activity);

struct VulkanBinding
{
    void* instance = nullptr;
    void* physicalDevice = nullptr;
    void* device = nullptr;
    void* queue = nullptr;
    uint32_t queueFamilyIndex = 0;
    uint32_t queueIndex = 0;
};

struct Frame
{
    XrTime predictedDisplayTime = 0;
    XrDuration predictedDisplayPeriod = 0;
    bool shouldRender = false;
    bool viewsValid = false;
    uint32_t viewCount = 0;
    XrView views[2]{};
};

struct VulkanSwapchain
{
    XrSwapchain handle = XR_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t arraySize = 0;
    std::vector<XrSwapchainImageVulkan2KHR> images;
};

struct ControllerState
{
    bool active = false;
    bool select = false;
    float thumbstickX = 0.0f;
    float thumbstickY = 0.0f;
};

class Runtime
{
public:
    Runtime();
    ~Runtime();

    // Creates only the API instance/system. Vulkan device creation remains in
    // the renderer and is therefore the only platform/backend-specific part.
    bool Initialize(const char* applicationName, const char* engineName,
                    void* androidJniEnv = nullptr, void* androidActivity = nullptr);
    bool GetVulkanRequirements(XrVersion* minApiVersion, XrVersion* maxApiVersion) const;
    bool GetVulkanGraphicsDevice(void* vulkanInstance, void** physicalDevice) const;
    bool CreateVulkanInstance(void* getInstanceProcAddr, const void* createInfo, void** instance,
                              int32_t* vulkanResult = nullptr) const;
    bool CreateVulkanDevice(void* getInstanceProcAddr, void* physicalDevice,
                            const void* createInfo, void** device,
                            int32_t* vulkanResult = nullptr) const;
    bool CreateVulkanSession(const VulkanBinding& binding);
    bool CreateVulkanSwapchain(int64_t format, uint32_t width, uint32_t height,
                               uint32_t arraySize, VulkanSwapchain& swapchain);
    bool EnumerateVulkanSwapchainFormats(std::vector<int64_t>& formats) const;
    void DestroyVulkanSwapchain(VulkanSwapchain& swapchain);
    bool AcquireSwapchainImage(VulkanSwapchain& swapchain, uint32_t& imageIndex);
    bool WaitSwapchainImage(VulkanSwapchain& swapchain);
    bool ReleaseSwapchainImage(VulkanSwapchain& swapchain);
    void Shutdown();

    void PollEvents();
    bool BeginFrame(Frame& frame);
    bool EndFrame(const XrCompositionLayerBaseHeader* const* layers, uint32_t layerCount);
    bool IsInitialized() const { return m_instance != XR_NULL_HANDLE; }
    bool IsSessionRunning() const { return m_sessionRunning; }
    XrSpace GetStageSpace() const { return m_stageSpace; }
    uint32_t GetRecommendedViewWidth() const { return m_viewConfig[0].recommendedImageRectWidth; }
    uint32_t GetRecommendedViewHeight() const { return m_viewConfig[0].recommendedImageRectHeight; }
    uint32_t GetViewCount() const { return m_viewCount; }
    const char* GetLastError() const { return m_lastError; }
    const ControllerState& GetLeftController() const { return m_leftController; }
    const ControllerState& GetRightController() const { return m_rightController; }

private:
    bool LoadLoader();
    bool LoadInstanceFunctions();
    bool CreateActions();
    bool BeginSession();
    void SetError(const char* message);
    bool Check(XrResult result, const char* operation);

    void* m_loader = nullptr;
    PFN_xrGetInstanceProcAddr m_getInstanceProcAddr = nullptr;
    PFN_xrEnumerateInstanceExtensionProperties m_enumerateExtensions = nullptr;
    PFN_xrCreateInstance m_createInstance = nullptr;
    PFN_xrDestroyInstance m_destroyInstance = nullptr;
    PFN_xrGetSystem m_getSystem = nullptr;
    PFN_xrEnumerateViewConfigurationViews m_enumerateViews = nullptr;
    PFN_xrCreateSession m_createSession = nullptr;
    PFN_xrDestroySession m_destroySession = nullptr;
    PFN_xrBeginSession m_beginSession = nullptr;
    PFN_xrEndSession m_endSession = nullptr;
    PFN_xrPollEvent m_pollEvent = nullptr;
    PFN_xrWaitFrame m_waitFrame = nullptr;
    PFN_xrBeginFrame m_beginFrame = nullptr;
    PFN_xrEndFrame m_endFrame = nullptr;
    PFN_xrLocateViews m_locateViews = nullptr;
    PFN_xrCreateReferenceSpace m_createReferenceSpace = nullptr;
    PFN_xrDestroySpace m_destroySpace = nullptr;
    PFN_xrStringToPath m_stringToPath = nullptr;
    PFN_xrCreateActionSet m_createActionSet = nullptr;
    PFN_xrDestroyActionSet m_destroyActionSet = nullptr;
    PFN_xrCreateAction m_createAction = nullptr;
    PFN_xrSuggestInteractionProfileBindings m_suggestBindings = nullptr;
    PFN_xrAttachSessionActionSets m_attachActionSets = nullptr;
    PFN_xrCreateActionSpace m_createActionSpace = nullptr;
    PFN_xrSyncActions m_syncActions = nullptr;
    PFN_xrGetActionStateBoolean m_getBoolean = nullptr;
    PFN_xrGetActionStateFloat m_getFloat = nullptr;
    PFN_xrGetActionStateVector2f m_getVector2 = nullptr;
    PFN_xrGetVulkanGraphicsRequirements2KHR m_getVulkanRequirements = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR m_getVulkanGraphicsDevice = nullptr;
    PFN_xrCreateVulkanInstanceKHR m_createVulkanInstance = nullptr;
    PFN_xrCreateVulkanDeviceKHR m_createVulkanDevice = nullptr;
    PFN_xrEnumerateSwapchainFormats m_enumerateSwapchainFormats = nullptr;
    PFN_xrCreateSwapchain m_createSwapchain = nullptr;
    PFN_xrDestroySwapchain m_destroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages m_enumerateSwapchainImages = nullptr;
    PFN_xrAcquireSwapchainImage m_acquireSwapchainImage = nullptr;
    PFN_xrWaitSwapchainImage m_waitSwapchainImage = nullptr;
    PFN_xrReleaseSwapchainImage m_releaseSwapchainImage = nullptr;

    XrInstance m_instance = XR_NULL_HANDLE;
    XrSystemId m_system = 0;
    XrSession m_session = XR_NULL_HANDLE;
    XrSpace m_stageSpace = XR_NULL_HANDLE;
    XrActionSet m_gameplayActionSet = XR_NULL_HANDLE;
    XrAction m_selectAction = XR_NULL_HANDLE;
    XrAction m_triggerAction = XR_NULL_HANDLE;
    XrAction m_moveAction = XR_NULL_HANDLE;
    XrPath m_leftHandPath = 0;
    XrPath m_rightHandPath = 0;
    XrViewConfigurationView m_viewConfig[2]{};
    uint32_t m_viewCount = 0;
    bool m_sessionRunning = false;
    bool m_shouldExit = false;
    bool m_metaTouchPlusEnabled = false;
    ControllerState m_leftController;
    ControllerState m_rightController;
    char m_lastError[256]{};
};
}

#endif
