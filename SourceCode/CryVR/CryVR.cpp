#include "CryVR.h"

#if defined(_WIN32)
#include <Windows.h>
#elif defined(__ANDROID__)
#include <SDL3/SDL.h>
#include <SDL3/SDL_loadso.h>
#else
#include <dlfcn.h>
#endif
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <algorithm>

namespace
{
void LogRuntime(const char* format, ...)
{
    va_list args;
    va_start(args, format);
#if defined(_WIN32)
    char message[1024];
    _vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    OutputDebugStringA(message);
#elif defined(__ANDROID__)
    char message[1024];
    vsnprintf(message, sizeof(message), format, args);
    SDL_Log("%s", message);
#else
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
#endif
    va_end(args);
}

template<class T>
bool LoadInstanceProc(PFN_xrGetInstanceProcAddr getProc, XrInstance instance, const char* name, T& output)
{
    PFN_xrVoidFunction function = nullptr;
    if (getProc(instance, name, &function) != XR_SUCCESS || !function)
        return false;
    output = reinterpret_cast<T>(function);
    return true;
}

XrPosef IdentityPose()
{
    XrPosef pose{};
    pose.orientation.w = 1.0f;
    return pose;
}
}

namespace CryVR
{
Runtime::Runtime() = default;

Runtime::~Runtime()
{
    Shutdown();
}

void Runtime::SetError(const char* message)
{
    strncpy(m_lastError, message ? message : "OpenXR error", sizeof(m_lastError) - 1);
    m_lastError[sizeof(m_lastError) - 1] = 0;
}

bool Runtime::Check(XrResult result, const char* operation)
{
    if (result >= 0)
        return true;
    char text[256];
    snprintf(text, sizeof(text), "%s failed (OpenXR result %d)", operation, result);
    SetError(text);
    return false;
}

bool Runtime::LoadLoader()
{
    const char* names[] = {
#if defined(_WIN32)
        "openxr_loader.dll",
#elif defined(__ANDROID__)
        "libopenxr_loader.so",
#else
        "libopenxr_loader.so.1",
        "libopenxr_loader.so",
#endif
    };
    for (const char* name : names)
    {
#if defined(_WIN32)
        m_loader = LoadLibraryA(name);
#elif defined(__ANDROID__)
        m_loader = SDL_LoadObject(name);
#else
        m_loader = dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
        if (m_loader)
            break;
    }
    if (!m_loader)
    {
#if defined(_WIN32)
        SetError("Could not load openxr_loader.dll");
#elif defined(__ANDROID__)
        SetError(SDL_GetError());
#else
        SetError(dlerror());
#endif
        return false;
    }
#if defined(_WIN32)
    m_getInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(
        GetProcAddress(static_cast<HMODULE>(m_loader), "xrGetInstanceProcAddr"));
#elif defined(__ANDROID__)
    m_getInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(SDL_LoadFunction(reinterpret_cast<SDL_SharedObject*>(m_loader), "xrGetInstanceProcAddr"));
#else
    m_getInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(dlsym(m_loader, "xrGetInstanceProcAddr"));
#endif
    if (!m_getInstanceProcAddr)
    {
        SetError("OpenXR loader does not export xrGetInstanceProcAddr");
        return false;
    }
    return true;
}

bool Runtime::LoadInstanceFunctions()
{
#define XR_LOAD(member, symbol) if (!LoadInstanceProc(m_getInstanceProcAddr, m_instance, symbol, m_##member)) { SetError("Missing OpenXR function: " symbol); return false; }
    XR_LOAD(destroyInstance, "xrDestroyInstance")
    XR_LOAD(getSystem, "xrGetSystem")
    XR_LOAD(enumerateViews, "xrEnumerateViewConfigurationViews")
    XR_LOAD(createSession, "xrCreateSession")
    XR_LOAD(destroySession, "xrDestroySession")
    XR_LOAD(beginSession, "xrBeginSession")
    XR_LOAD(endSession, "xrEndSession")
    XR_LOAD(pollEvent, "xrPollEvent")
    XR_LOAD(waitFrame, "xrWaitFrame")
    XR_LOAD(beginFrame, "xrBeginFrame")
    XR_LOAD(endFrame, "xrEndFrame")
    XR_LOAD(locateViews, "xrLocateViews")
    XR_LOAD(createReferenceSpace, "xrCreateReferenceSpace")
    XR_LOAD(destroySpace, "xrDestroySpace")
    XR_LOAD(stringToPath, "xrStringToPath")
    XR_LOAD(createActionSet, "xrCreateActionSet")
    XR_LOAD(destroyActionSet, "xrDestroyActionSet")
    XR_LOAD(createAction, "xrCreateAction")
    XR_LOAD(suggestBindings, "xrSuggestInteractionProfileBindings")
    XR_LOAD(attachActionSets, "xrAttachSessionActionSets")
    XR_LOAD(createActionSpace, "xrCreateActionSpace")
    XR_LOAD(locateSpace, "xrLocateSpace")
    XR_LOAD(syncActions, "xrSyncActions")
    XR_LOAD(getBoolean, "xrGetActionStateBoolean")
    XR_LOAD(getFloat, "xrGetActionStateFloat")
	XR_LOAD(getVector2, "xrGetActionStateVector2f")
	XR_LOAD(enumerateSwapchainFormats, "xrEnumerateSwapchainFormats")
	XR_LOAD(createSwapchain, "xrCreateSwapchain")
	XR_LOAD(destroySwapchain, "xrDestroySwapchain")
	XR_LOAD(enumerateSwapchainImages, "xrEnumerateSwapchainImages")
	XR_LOAD(acquireSwapchainImage, "xrAcquireSwapchainImage")
	XR_LOAD(waitSwapchainImage, "xrWaitSwapchainImage")
	XR_LOAD(releaseSwapchainImage, "xrReleaseSwapchainImage")
#undef XR_LOAD
    return true;
}

bool Runtime::Initialize(const char* applicationName, const char* engineName)
{
    if (IsInitialized())
        return true;
    if (!LoadLoader())
        return false;

    const Platform::ApplicationContext platformContext = Platform::GetApplicationContext();
    if (platformContext.error[0])
    {
        SetError(platformContext.error);
        return false;
    }
    char platformError[192]{};
    if (!Platform::InitializeLoader(m_getInstanceProcAddr, platformContext,
                                    platformError, sizeof(platformError)))
    {
        SetError(platformError);
        return false;
    }

    if (m_getInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
                              reinterpret_cast<PFN_xrVoidFunction*>(&m_enumerateExtensions)) != XR_SUCCESS)
    {
        SetError("OpenXR loader does not expose xrEnumerateInstanceExtensionProperties");
        return false;
    }

    uint32_t extensionCount = 0;
    if (!Check(m_enumerateExtensions(nullptr, 0, &extensionCount, nullptr), "xrEnumerateInstanceExtensionProperties"))
        return false;
    std::vector<XrExtensionProperties> properties(extensionCount);
    for (uint32_t i = 0; i < extensionCount; ++i)
        properties[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    if (!Check(m_enumerateExtensions(nullptr, extensionCount, &extensionCount, properties.data()), "xrEnumerateInstanceExtensionProperties"))
        return false;

    std::vector<const char*> extensionNames;
    extensionNames.push_back("XR_KHR_vulkan_enable2");
    Platform::AppendRequiredInstanceExtensions(extensionNames);
    m_metaTouchPlusEnabled = false;
    m_displayRefreshRateEnabled = false;
    for (uint32_t i = 0; i < extensionCount; ++i)
    {
        if (strcmp(properties[i].extensionName, "XR_META_touch_controller_plus") == 0)
            m_metaTouchPlusEnabled = true;
        if (strcmp(properties[i].extensionName, "XR_FB_display_refresh_rate") == 0)
            m_displayRefreshRateEnabled = true;
    }
    for (const char* required : extensionNames)
    {
        bool available = false;
        for (uint32_t i = 0; i < extensionCount; ++i)
            if (strcmp(properties[i].extensionName, required) == 0)
            {
                available = true;
                break;
            }
        if (!available)
        {
            char message[XR_MAX_EXTENSION_NAME_SIZE + 64];
            snprintf(message, sizeof(message), "OpenXR runtime lacks required instance extension %s", required);
            SetError(message);
            return false;
        }
    }
    if (m_metaTouchPlusEnabled)
        extensionNames.push_back("XR_META_touch_controller_plus");
    if (m_displayRefreshRateEnabled)
        extensionNames.push_back("XR_FB_display_refresh_rate");

    XrInstanceCreateInfo createInfo{};
    createInfo.type = XR_TYPE_INSTANCE_CREATE_INFO;
    strncpy(createInfo.applicationInfo.applicationName, applicationName ? applicationName : "FarCry", XR_MAX_APPLICATION_NAME_SIZE - 1);
    createInfo.applicationInfo.applicationVersion = 1;
    strncpy(createInfo.applicationInfo.engineName, engineName ? engineName : "CryEngine", XR_MAX_ENGINE_NAME_SIZE - 1);
    createInfo.applicationInfo.engineVersion = 1;
    createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensionNames.size());
    createInfo.enabledExtensionNames = extensionNames.data();
    Platform::InstanceCreateInfoStorage platformCreateInfo{};
    if (!Platform::PrepareInstanceCreateInfo(createInfo, platformContext,
                                             platformCreateInfo, platformError,
                                             sizeof(platformError)))
    {
        SetError(platformError);
        return false;
    }
    if (!LoadInstanceProc(m_getInstanceProcAddr, XR_NULL_HANDLE, "xrCreateInstance", m_createInstance))
    {
        SetError("OpenXR loader does not expose xrCreateInstance");
        return false;
    }
    if (!Check(m_createInstance(&createInfo, &m_instance), "xrCreateInstance"))
        return false;

    if (!LoadInstanceFunctions())
        return false;
    XrSystemGetInfo systemInfo{};
    systemInfo.type = XR_TYPE_SYSTEM_GET_INFO;
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!Check(m_getSystem(m_instance, &systemInfo, &m_system), "xrGetSystem"))
        return false;

    if (!Check(m_enumerateViews ? m_enumerateViews(m_instance, m_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &m_viewCount, nullptr) : -1,
               "xrEnumerateViewConfigurationViews"))
        return false;
    if (m_viewCount > 2)
        m_viewCount = 2;
    for (uint32_t i = 0; i < m_viewCount; ++i)
        m_viewConfig[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    if (!Check(m_enumerateViews(m_instance, m_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, m_viewCount, &m_viewCount, m_viewConfig),
               "xrEnumerateViewConfigurationViews"))
        return false;

    if (!CreateActions())
        return false;
    return true;
}

bool Runtime::GetVulkanRequirements(XrVersion* minApiVersion, XrVersion* maxApiVersion) const
{
    if (!m_instance || !m_getVulkanRequirements)
        return false;
    XrGraphicsRequirementsVulkan2KHR requirements{};
    requirements.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR;
    if (m_getVulkanRequirements(m_instance, m_system, &requirements) != XR_SUCCESS)
        return false;
    if (minApiVersion) *minApiVersion = requirements.minApiVersionSupported;
    if (maxApiVersion) *maxApiVersion = requirements.maxApiVersionSupported;
    return true;
}

bool Runtime::GetVulkanGraphicsDevice(void* vulkanInstance, void** physicalDevice) const
{
    if (!m_instance || !m_getVulkanGraphicsDevice || !vulkanInstance || !physicalDevice)
        return false;

    XrVulkanGraphicsDeviceGetInfoKHR getInfo{};
    getInfo.type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR;
    getInfo.systemId = m_system;
    getInfo.vulkanInstance = vulkanInstance;
    return m_getVulkanGraphicsDevice(m_instance, &getInfo, physicalDevice) == XR_SUCCESS;
}

bool Runtime::CreateVulkanInstance(void* getInstanceProcAddr, const void* createInfo, void** instance,
                                    int32_t* vulkanResult) const
{
    if (!m_createVulkanInstance || !getInstanceProcAddr || !createInfo || !instance)
        return false;
    XrVulkanInstanceCreateInfoKHR info{};
    info.type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR;
    info.systemId = m_system;
    info.createFlags = 0;
    info.pfnGetInstanceProcAddr = getInstanceProcAddr;
    info.vulkanCreateInfo = createInfo;
    int32_t ignoredResult = -1;
    return m_createVulkanInstance(m_instance, &info, instance,
                                   vulkanResult ? vulkanResult : &ignoredResult) == XR_SUCCESS;
}

bool Runtime::CreateVulkanDevice(void* getInstanceProcAddr, void* physicalDevice,
                                  const void* createInfo, void** device,
                                  int32_t* vulkanResult) const
{
    if (!m_createVulkanDevice || !getInstanceProcAddr || !physicalDevice || !createInfo || !device)
        return false;
    XrVulkanDeviceCreateInfoKHR info{};
    info.type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR;
    info.systemId = m_system;
    info.createFlags = 0;
    info.pfnGetInstanceProcAddr = getInstanceProcAddr;
    info.vulkanPhysicalDevice = physicalDevice;
    info.vulkanCreateInfo = createInfo;
    int32_t ignoredResult = -1;
    return m_createVulkanDevice(m_instance, &info, device,
                                 vulkanResult ? vulkanResult : &ignoredResult) == XR_SUCCESS;
}

bool Runtime::CreateActions()
{
    if (!LoadInstanceProc(m_getInstanceProcAddr, m_instance, "xrGetVulkanGraphicsRequirements2KHR", m_getVulkanRequirements))
        return false;
    if (!LoadInstanceProc(m_getInstanceProcAddr, m_instance, "xrGetVulkanGraphicsDevice2KHR", m_getVulkanGraphicsDevice))
        return false;
    if (!LoadInstanceProc(m_getInstanceProcAddr, m_instance, "xrCreateVulkanInstanceKHR", m_createVulkanInstance))
        return false;
    if (!LoadInstanceProc(m_getInstanceProcAddr, m_instance, "xrCreateVulkanDeviceKHR", m_createVulkanDevice))
        return false;
    XrActionSetCreateInfo setInfo{};
    setInfo.type = XR_TYPE_ACTION_SET_CREATE_INFO;
    strcpy(setInfo.actionSetName, "gameplay");
    strcpy(setInfo.localizedActionSetName, "Far Cry Gameplay");
    setInfo.priority = 0;
    if (!Check(m_createActionSet(m_instance, &setInfo, &m_gameplayActionSet), "xrCreateActionSet"))
        return false;

    if (!Check(m_stringToPath(m_instance, "/user/hand/left", &m_leftHandPath), "xrStringToPath(left hand)")) return false;
    if (!Check(m_stringToPath(m_instance, "/user/hand/right", &m_rightHandPath), "xrStringToPath(right hand)")) return false;
    const XrPath hands[] = { m_leftHandPath, m_rightHandPath };
    const XrPath* handSubactions = hands;

    XrActionCreateInfo selectInfo{};
    selectInfo.type = XR_TYPE_ACTION_CREATE_INFO;
    strcpy(selectInfo.actionName, "select");
    strcpy(selectInfo.localizedActionName, "Select");
    selectInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    selectInfo.countSubactionPaths = 2;
    selectInfo.subactionPaths = hands;
    if (!Check(m_createAction(m_gameplayActionSet, &selectInfo, &m_selectAction), "xrCreateAction(select)")) return false;

    XrActionCreateInfo triggerInfo{};
    triggerInfo.type = XR_TYPE_ACTION_CREATE_INFO;
    strcpy(triggerInfo.actionName, "trigger");
    strcpy(triggerInfo.localizedActionName, "Trigger");
    triggerInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    triggerInfo.countSubactionPaths = 2;
    triggerInfo.subactionPaths = hands;
    if (!Check(m_createAction(m_gameplayActionSet, &triggerInfo, &m_triggerAction), "xrCreateAction(trigger)")) return false;

    const auto createBooleanAction = [this, handSubactions](const char* name, const char* localizedName, XrAction* action)
    {
        XrActionCreateInfo info{};
        info.type = XR_TYPE_ACTION_CREATE_INFO;
        strcpy(info.actionName, name);
        strcpy(info.localizedActionName, localizedName);
        info.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
        info.countSubactionPaths = 2;
        info.subactionPaths = handSubactions;
        return Check(m_createAction(m_gameplayActionSet, &info, action), name);
    };
    if (!createBooleanAction("posture", "Change Stance", &m_postureAction) ||
        !createBooleanAction("jump", "Jump", &m_jumpAction))
        return false;

    XrActionCreateInfo useInfo{};
    useInfo.type = XR_TYPE_ACTION_CREATE_INFO;
    strcpy(useInfo.actionName, "use");
    strcpy(useInfo.localizedActionName, "Use");
    useInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    useInfo.countSubactionPaths = 2;
    useInfo.subactionPaths = hands;
    if (!Check(m_createAction(m_gameplayActionSet, &useInfo, &m_useAction), "xrCreateAction(use)")) return false;

    XrActionCreateInfo moveInfo{};
    moveInfo.type = XR_TYPE_ACTION_CREATE_INFO;
    strcpy(moveInfo.actionName, "move");
    strcpy(moveInfo.localizedActionName, "Move");
    moveInfo.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    moveInfo.countSubactionPaths = 2;
    moveInfo.subactionPaths = hands;
    if (!Check(m_createAction(m_gameplayActionSet, &moveInfo, &m_moveAction), "xrCreateAction(move)")) return false;

    const auto path = [this](const char* text) -> XrPath
    {
        XrPath value = 0;
        m_stringToPath(m_instance, text, &value);
        return value;
    };
    XrPath handPaths[2] = { m_leftHandPath, m_rightHandPath };
    XrActionCreateInfo poseInfo{};
    poseInfo.type = XR_TYPE_ACTION_CREATE_INFO;
    poseInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    strcpy(poseInfo.actionName, "grip_pose");
    strcpy(poseInfo.localizedActionName, "Controller grip pose");
    poseInfo.countSubactionPaths = 2;
    poseInfo.subactionPaths = handPaths;
    if (!Check(m_createAction(m_gameplayActionSet, &poseInfo, &m_gripPoseAction), "xrCreateAction(grip_pose)")) return false;
    const XrPath leftSelect = path("/user/hand/left/input/select/click");
    const XrPath rightSelect = path("/user/hand/right/input/select/click");
    const XrPath leftThumb = path("/user/hand/left/input/thumbstick");
    const XrPath rightThumb = path("/user/hand/right/input/thumbstick");
    // The Oculus Touch profile does not define the generic select/click path.
    // Use its standard face-button and analog-trigger components instead.
    const XrPath leftTouchSelect = path("/user/hand/left/input/x/click");
    const XrPath rightTouchPosture = path("/user/hand/right/input/a/click");
    const XrPath rightTouchJump = path("/user/hand/right/input/b/click");
    const XrPath leftTouchTrigger = path("/user/hand/left/input/trigger/value");
    const XrPath rightTouchTrigger = path("/user/hand/right/input/trigger/value");
    const XrPath leftTouchGrip = path("/user/hand/left/input/squeeze/value");
    const XrPath rightTouchGrip = path("/user/hand/right/input/squeeze/value");
    const auto suggest = [this](const char* profileName, const XrActionSuggestedBinding* bindings,
                                uint32_t count)
    {
        XrPath profilePath = 0;
        const XrResult pathResult = m_stringToPath(m_instance, profileName, &profilePath);
        if (pathResult < 0 || !profilePath)
        {
            LogRuntime("OpenXR input profile path failed: %s result=%d", profileName, pathResult);
            return;
        }
        XrInteractionProfileSuggestedBinding suggested{};
        suggested.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
        suggested.interactionProfile = profilePath;
        suggested.countSuggestedBindings = count;
        suggested.suggestedBindings = bindings;
        // Unsupported profiles are expected: the runtime chooses one that
        // matches connected hardware and ignores unrelated profiles.
        const XrResult result = m_suggestBindings(m_instance, &suggested);
        LogRuntime("OpenXR input profile %s suggestion result=%d", profileName, result);
    };

    const XrActionSuggestedBinding oculusBindings[] = {
        {m_gripPoseAction, path("/user/hand/left/input/grip/pose")},
        {m_gripPoseAction, path("/user/hand/right/input/grip/pose")},
        {m_selectAction, leftTouchSelect},
        {m_triggerAction, leftTouchTrigger}, {m_triggerAction, rightTouchTrigger},
        {m_moveAction, leftThumb}, {m_moveAction, rightThumb},
        {m_postureAction, rightTouchPosture}, {m_jumpAction, rightTouchJump},
        {m_useAction, leftTouchGrip}, {m_useAction, rightTouchGrip}
    };
    suggest("/interaction_profiles/oculus/touch_controller", oculusBindings, 11);

    const XrPath leftMetaTrigger = path("/user/hand/left/input/trigger/value");
    const XrPath rightMetaTrigger = path("/user/hand/right/input/trigger/value");
    const XrPath leftMetaGrip = path("/user/hand/left/input/squeeze/value");
    const XrPath rightMetaGrip = path("/user/hand/right/input/squeeze/value");
    const XrActionSuggestedBinding metaBindings[] = {
        {m_gripPoseAction, path("/user/hand/left/input/grip/pose")},
        {m_gripPoseAction, path("/user/hand/right/input/grip/pose")},
        {m_triggerAction, leftMetaTrigger}, {m_triggerAction, rightMetaTrigger},
        {m_moveAction, leftThumb}, {m_moveAction, rightThumb},
        {m_postureAction, rightTouchPosture}, {m_jumpAction, rightTouchJump},
        {m_useAction, leftMetaGrip}, {m_useAction, rightMetaGrip}
    };
    if (m_metaTouchPlusEnabled)
        suggest("/interaction_profiles/meta/touch_controller_plus", metaBindings, 10);

    const XrActionSuggestedBinding simpleBindings[] = {
        {m_selectAction, leftSelect}, {m_selectAction, rightSelect}
    };
    suggest("/interaction_profiles/khr/simple_controller", simpleBindings, 2);

    const XrPath leftTriggerClick = path("/user/hand/left/input/trigger/click");
    const XrPath rightTriggerClick = path("/user/hand/right/input/trigger/click");
    const XrPath leftTrackpad = path("/user/hand/left/input/trackpad");
    const XrPath rightTrackpad = path("/user/hand/right/input/trackpad");
    const XrActionSuggestedBinding viveBindings[] = {
        {m_selectAction, leftTriggerClick}, {m_selectAction, rightTriggerClick},
        {m_moveAction, leftTrackpad}, {m_moveAction, rightTrackpad}
    };
    suggest("/interaction_profiles/htc/vive_controller", viveBindings, 4);

    const XrActionSuggestedBinding indexBindings[] = {
        {m_selectAction, leftTriggerClick}, {m_selectAction, rightTriggerClick},
        {m_moveAction, leftThumb}, {m_moveAction, rightThumb}
    };
    suggest("/interaction_profiles/valve/index_controller", indexBindings, 4);
    return true;
}

bool Runtime::CreateVulkanSession(const VulkanBinding& binding)
{
    if (!IsInitialized() || m_session)
        return false;
    XrGraphicsBindingVulkan2KHR graphicsBinding{};
    graphicsBinding.type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
    graphicsBinding.instance = binding.instance;
    graphicsBinding.physicalDevice = binding.physicalDevice;
    graphicsBinding.device = binding.device;
    graphicsBinding.queueFamilyIndex = binding.queueFamilyIndex;
    graphicsBinding.queueIndex = binding.queueIndex;
    XrSessionCreateInfo sessionInfo{};
    sessionInfo.type = XR_TYPE_SESSION_CREATE_INFO;
    sessionInfo.systemId = m_system;
    sessionInfo.next = &graphicsBinding;
    if (!Check(m_createSession(m_instance, &sessionInfo, &m_session), "xrCreateSession"))
        return false;

    XrReferenceSpaceCreateInfo spaceInfo{};
    spaceInfo.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    spaceInfo.poseInReferenceSpace = IdentityPose();
    if (m_createReferenceSpace(m_session, &spaceInfo, &m_stageSpace) != XR_SUCCESS)
    {
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        if (!Check(m_createReferenceSpace(m_session, &spaceInfo, &m_stageSpace), "xrCreateReferenceSpace"))
            return false;
    }
    XrSessionActionSetsAttachInfo attach{};
    XrPath handPaths[2] = { m_leftHandPath, m_rightHandPath };
    for (int hand = 0; hand < 2; ++hand)
    {
        XrActionSpaceCreateInfo grip{};
        grip.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
        grip.action = m_gripPoseAction;
        grip.subactionPath = handPaths[hand];
        grip.poseInActionSpace = IdentityPose();
        if (!Check(m_createActionSpace(m_session, &grip, &m_gripSpaces[hand]), "xrCreateActionSpace(grip)")) return false;
    }
    attach.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
    attach.countActionSets = 1;
    attach.actionSets = &m_gameplayActionSet;
    if (!Check(m_attachActionSets(m_session, &attach), "xrAttachSessionActionSets"))
        return false;
    return true;
}

bool Runtime::CreateVulkanSwapchain(int64_t format, uint32_t width, uint32_t height,
                                    uint32_t arraySize, VulkanSwapchain& swapchain)
{
    if (!m_session || !m_createSwapchain || arraySize == 0)
        return false;
    XrSwapchainCreateInfo createInfo{};
    createInfo.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    createInfo.format = format;
    createInfo.sampleCount = 1;
    createInfo.width = width;
    createInfo.height = height;
    createInfo.faceCount = 1;
    createInfo.arraySize = arraySize;
    createInfo.mipCount = 1;
    // Prefer a bit-exact image transfer over a full-screen texture pass. A
    // runtime that rejects this usage retains the original rendering path.
    XrResult created = m_createSwapchain(m_session, &createInfo, &swapchain.handle);
    swapchain.transferDestination = created == XR_SUCCESS;
    if (created != XR_SUCCESS)
    {
        createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        created = m_createSwapchain(m_session, &createInfo, &swapchain.handle);
    }
    if (!Check(created, "xrCreateSwapchain")) return false;

    uint32_t imageCount = 0;
    if (!Check(m_enumerateSwapchainImages(swapchain.handle, 0, &imageCount, nullptr), "xrEnumerateSwapchainImages"))
    {
        m_destroySwapchain(swapchain.handle);
        swapchain.handle = XR_NULL_HANDLE;
        return false;
    }
    swapchain.images.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i)
        swapchain.images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
    if (!Check(m_enumerateSwapchainImages(swapchain.handle, imageCount, &imageCount,
                                          reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchain.images.data())),
               "xrEnumerateSwapchainImages"))
    {
        m_destroySwapchain(swapchain.handle);
        swapchain.handle = XR_NULL_HANDLE;
        swapchain.images.clear();
        return false;
    }
    swapchain.width = width;
    swapchain.height = height;
    swapchain.arraySize = arraySize;
    return true;
}

bool Runtime::EnumerateVulkanSwapchainFormats(std::vector<int64_t>& formats) const
{
    formats.clear();
    if (!m_session || !m_enumerateSwapchainFormats)
        return false;
    uint32_t count = 0;
    if (m_enumerateSwapchainFormats(m_session, 0, &count, nullptr) != XR_SUCCESS || count == 0)
        return false;
    formats.resize(count);
    if (m_enumerateSwapchainFormats(m_session, count, &count, formats.data()) != XR_SUCCESS)
    {
        formats.clear();
        return false;
    }
    formats.resize(count);
    return true;
}

void Runtime::DestroyVulkanSwapchain(VulkanSwapchain& swapchain)
{
    if (swapchain.handle && m_destroySwapchain)
        m_destroySwapchain(swapchain.handle);
    swapchain.images.clear();
    swapchain.handle = XR_NULL_HANDLE;
}

bool Runtime::AcquireSwapchainImage(VulkanSwapchain& swapchain, uint32_t& imageIndex)
{
    XrSwapchainImageAcquireInfo info{};
    info.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
    return Check(m_acquireSwapchainImage(swapchain.handle, &info, &imageIndex), "xrAcquireSwapchainImage");
}

bool Runtime::WaitSwapchainImage(VulkanSwapchain& swapchain)
{
    XrSwapchainImageWaitInfo info{};
    info.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
    info.timeout = 1000000000LL;
    return Check(m_waitSwapchainImage(swapchain.handle, &info), "xrWaitSwapchainImage");
}

bool Runtime::ReleaseSwapchainImage(VulkanSwapchain& swapchain)
{
    XrSwapchainImageReleaseInfo info{};
    info.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
    return Check(m_releaseSwapchainImage(swapchain.handle, &info), "xrReleaseSwapchainImage");
}

bool Runtime::BeginSession()
{
    if (m_sessionRunning)
        return true;
    XrSessionBeginInfo beginInfo{};
    beginInfo.type = XR_TYPE_SESSION_BEGIN_INFO;
    beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    if (!Check(m_beginSession(m_session, &beginInfo), "xrBeginSession"))
        return false;
    m_sessionRunning = true;
    if (m_displayRefreshRateEnabled)
    {
        using RequestRefreshRate = XrResult (*)(XrSession, float);
        RequestRefreshRate requestRefreshRate = nullptr;
        if (LoadInstanceProc(m_getInstanceProcAddr, m_instance,
                             "xrRequestDisplayRefreshRateFB", requestRefreshRate))
        {
            const XrResult result = requestRefreshRate(m_session, 72.0f);
            LogRuntime("OpenXR display refresh rate request: 72 Hz, result=%d", result);
        }
    }
    return true;
}

void Runtime::PollEvents()
{
    if (!m_instance || !m_pollEvent)
        return;
    for (;;)
    {
        XrEventDataBuffer buffer{};
        buffer.type = XR_TYPE_EVENT_DATA_BUFFER;
        XrResult result = m_pollEvent(m_instance, &buffer);
        if (result == XR_EVENT_UNAVAILABLE)
            break;
        if (result < 0)
            break;
        if (buffer.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        {
            const XrEventDataSessionStateChanged* state = reinterpret_cast<const XrEventDataSessionStateChanged*>(&buffer);
            if (state->state == XR_SESSION_STATE_READY)
                BeginSession();
            else if (state->state == XR_SESSION_STATE_STOPPING && m_sessionRunning)
            {
                m_endSession(m_session);
                m_sessionRunning = false;
            }
            else if (state->state == XR_SESSION_STATE_EXITING || state->state == XR_SESSION_STATE_LOSS_PENDING)
                m_shouldExit = true;
        }
    }
}

bool Runtime::BeginFrame(Frame& frame)
{
    static bool moving[2] = { false, false };
    static bool selecting[2] = { false, false };
    static bool loggedActionAvailability = false;
    XrBool32 selectActive[2] = { XR_FALSE, XR_FALSE };
    XrBool32 triggerActive[2] = { XR_FALSE, XR_FALSE };
    XrBool32 moveActive[2] = { XR_FALSE, XR_FALSE };
    frame = Frame{};
    // Never carry a held controller state across a lost focus/session frame.
    m_leftController = ControllerState{};
    m_rightController = ControllerState{};
    PollEvents();
    if (!m_sessionRunning || m_shouldExit)
        return false;
    XrFrameWaitInfo waitInfo{};
    waitInfo.type = XR_TYPE_FRAME_WAIT_INFO;
    XrFrameState frameState{};
    frameState.type = XR_TYPE_FRAME_STATE;
    if (!Check(m_waitFrame(m_session, &waitInfo, &frameState), "xrWaitFrame")) return false;
    XrFrameBeginInfo beginInfo{};
    beginInfo.type = XR_TYPE_FRAME_BEGIN_INFO;
    if (!Check(m_beginFrame(m_session, &beginInfo), "xrBeginFrame")) return false;
    m_predictedDisplayTime = frameState.predictedDisplayTime;
    frame.predictedDisplayTime = frameState.predictedDisplayTime;
    frame.predictedDisplayPeriod = frameState.predictedDisplayPeriod;
    frame.shouldRender = frameState.shouldRender == XR_TRUE;
    if (frame.shouldRender)
    {
        XrViewLocateInfo locate{};
        locate.type = XR_TYPE_VIEW_LOCATE_INFO;
        locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime = frame.predictedDisplayTime;
        locate.space = m_stageSpace;
        XrViewState viewState{};
        viewState.type = XR_TYPE_VIEW_STATE;
        for (uint32_t i = 0; i < 2; ++i) m_viewConfig[i].next = nullptr;
        if (!Check(m_locateViews(m_session, &locate, &viewState, 2, &frame.viewCount, frame.views), "xrLocateViews"))
        {
            // xrBeginFrame has already succeeded, so this begun frame must
            // still be paired with xrEndFrame even when view location fails.
            EndFrame(nullptr, 0);
            return false;
        }
        frame.viewsValid = frame.viewCount == 2 &&
            (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
        frame.viewPositionsValid = frame.viewsValid &&
            (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
    }

    // Input is synchronized against the same predicted display time as the
    // views. The game-facing mapping stays platform independent.
    XrActiveActionSet activeSet{};
    activeSet.actionSet = m_gameplayActionSet;
    XrActionsSyncInfo sync{};
    sync.type = XR_TYPE_ACTIONS_SYNC_INFO;
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &activeSet;
    if (m_syncActions(m_session, &sync) >= 0)
    {
        XrPath paths[2] = { m_leftHandPath, m_rightHandPath };
        ControllerState* states[2] = { &m_leftController, &m_rightController };
        for (int i = 0; i < 2; ++i)
        {
            XrSpaceLocation location{};
            location.type = XR_TYPE_SPACE_LOCATION;
            const XrSpaceLocationFlags valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            if (m_gripSpaces[i] && m_locateSpace(m_gripSpaces[i], m_stageSpace,
                    m_predictedDisplayTime, &location) >= 0 && (location.locationFlags & valid) == valid)
            {
                states[i]->gripPose = location.pose;
                states[i]->poseValid = true;
            }
            XrActionStateGetInfo getInfo{};
            getInfo.type = XR_TYPE_ACTION_STATE_GET_INFO;
            getInfo.action = m_selectAction;
            getInfo.subactionPath = paths[i];
            XrActionStateBoolean select{};
            select.type = XR_TYPE_ACTION_STATE_BOOLEAN;
            if (m_getBoolean(m_session, &getInfo, &select) >= 0)
            {
                selectActive[i] = select.isActive;
                states[i]->active = states[i]->active || select.isActive == XR_TRUE;
                states[i]->select = states[i]->select || select.currentState == XR_TRUE;
                if ((select.currentState == XR_TRUE) != selecting[i])
                    LogRuntime("OpenXR input %s select active=%d pressed=%d",
                        i == 0 ? "left" : "right", select.isActive, select.currentState);
                selecting[i] = select.currentState == XR_TRUE;
            }
            getInfo.action = m_postureAction;
            XrActionStateBoolean posture{};
            posture.type = XR_TYPE_ACTION_STATE_BOOLEAN;
            if (m_getBoolean(m_session, &getInfo, &posture) >= 0)
            {
                states[i]->posture = posture.isActive == XR_TRUE && posture.currentState == XR_TRUE;
                states[i]->active = states[i]->active || posture.isActive == XR_TRUE;
            }
            getInfo.action = m_jumpAction;
            XrActionStateBoolean jump{};
            jump.type = XR_TYPE_ACTION_STATE_BOOLEAN;
            if (m_getBoolean(m_session, &getInfo, &jump) >= 0)
            {
                states[i]->jump = jump.isActive == XR_TRUE && jump.currentState == XR_TRUE;
                states[i]->active = states[i]->active || jump.isActive == XR_TRUE;
            }
            getInfo.action = m_useAction;
            XrActionStateFloat use{};
            use.type = XR_TYPE_ACTION_STATE_FLOAT;
            if (m_getFloat(m_session, &getInfo, &use) >= 0)
            {
                states[i]->use = use.isActive == XR_TRUE && use.currentState > 0.55f;
                states[i]->gripAmount = use.isActive == XR_TRUE ?
                    std::max(0.0f, std::min(1.0f, use.currentState)) : 0.0f;
                states[i]->active = states[i]->active || use.isActive == XR_TRUE;
            }
            getInfo.action = m_triggerAction;
            XrActionStateFloat trigger{};
            trigger.type = XR_TYPE_ACTION_STATE_FLOAT;
            if (m_getFloat(m_session, &getInfo, &trigger) >= 0)
            {
                triggerActive[i] = trigger.isActive;
                states[i]->triggerAmount = trigger.isActive == XR_TRUE ?
                    std::max(0.0f, std::min(1.0f, trigger.currentState)) : 0.0f;
                states[i]->active = states[i]->active || trigger.isActive == XR_TRUE;
                states[i]->select = states[i]->select ||
                                    (trigger.isActive == XR_TRUE && trigger.currentState > 0.55f);
            }
            getInfo.action = m_moveAction;
            XrActionStateVector2f move{};
            move.type = XR_TYPE_ACTION_STATE_VECTOR2F;
            if (m_getVector2(m_session, &getInfo, &move) >= 0)
            {
                moveActive[i] = move.isActive;
                states[i]->thumbstickX = move.currentState.x;
                states[i]->thumbstickY = move.currentState.y;
                states[i]->active = states[i]->active || move.isActive == XR_TRUE;
                const bool isMoving = move.isActive == XR_TRUE &&
                    (move.currentState.x*move.currentState.x + move.currentState.y*move.currentState.y > 0.04f);
                if (isMoving != moving[i])
                    LogRuntime("OpenXR input %s stick active=%d moving=%d axis=(%.2f,%.2f)",
                        i == 0 ? "left" : "right", move.isActive, isMoving,
                        move.currentState.x, move.currentState.y);
                moving[i] = isMoving;
            }
        }
        if (!loggedActionAvailability)
        {
            LogRuntime("OpenXR input availability left(select=%d trigger=%d stick=%d) right(select=%d trigger=%d stick=%d)",
                selectActive[0], triggerActive[0], moveActive[0],
                selectActive[1], triggerActive[1], moveActive[1]);
            loggedActionAvailability = true;
        }
    }
    return true;
}

bool Runtime::EndFrame(const XrCompositionLayerBaseHeader* const* layers, uint32_t layerCount)
{
    if (!m_sessionRunning)
        return false;
    XrFrameEndInfo endInfo{};
    endInfo.type = XR_TYPE_FRAME_END_INFO;
    endInfo.displayTime = m_predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = layerCount;
    endInfo.layers = layers;
    return Check(m_endFrame(m_session, &endInfo), "xrEndFrame");
}

void Runtime::Shutdown()
{
    if (m_sessionRunning && m_endSession)
        m_endSession(m_session);
    m_sessionRunning = false;
    m_predictedDisplayTime = 0;
    for (int hand = 0; hand < 2; ++hand)
    {
        if (m_gripSpaces[hand] && m_destroySpace) m_destroySpace(m_gripSpaces[hand]);
        m_gripSpaces[hand] = XR_NULL_HANDLE;
    }
    m_gripPoseAction = XR_NULL_HANDLE;
    if (m_stageSpace && m_destroySpace) m_destroySpace(m_stageSpace);
    if (m_session && m_destroySession) m_destroySession(m_session);
    if (m_gameplayActionSet && m_destroyActionSet) m_destroyActionSet(m_gameplayActionSet);
    if (m_instance && m_destroyInstance) m_destroyInstance(m_instance);
    m_stageSpace = XR_NULL_HANDLE;
    m_session = XR_NULL_HANDLE;
    m_gameplayActionSet = XR_NULL_HANDLE;
    m_instance = XR_NULL_HANDLE;
    if (m_loader)
#if defined(_WIN32)
        FreeLibrary(static_cast<HMODULE>(m_loader));
#elif defined(__ANDROID__)
        SDL_UnloadObject(reinterpret_cast<SDL_SharedObject*>(m_loader));
#else
        dlclose(m_loader);
#endif
    m_loader = nullptr;
}
}
