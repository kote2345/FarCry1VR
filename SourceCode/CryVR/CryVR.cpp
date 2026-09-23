#include "CryVR.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_loadso.h>
#ifdef CRYVR_ANDROID
#include <SDL3/SDL_system.h>
#include <jni.h>
#include <android/log.h>
#endif
#include <stdio.h>
#include <string.h>

namespace
{
template<class T>
bool LoadInstanceProc(PFN_xrGetInstanceProcAddr getProc, XrInstance instance, const char* name, T& output)
{
    PFN_xrVoidFunction function = nullptr;
    if (getProc(instance, name, &function) != XR_SUCCESS || !function)
        return false;
    output = reinterpret_cast<T>(function);
    return true;
}

bool ContainsExtension(const char* const* extensions, uint32_t count, const char* name)
{
    for (uint32_t i = 0; i < count; ++i)
        if (strcmp(extensions[i], name) == 0)
            return true;
    return false;
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
#ifdef CRYVR_ANDROID
namespace
{
JavaVM* g_androidVm = nullptr;
jobject g_androidActivity = nullptr;
}

void GetAndroidOpenXRContext(void** jniEnv, void** activity)
{
    if (jniEnv) *jniEnv = nullptr;
    if (activity) *activity = nullptr;
    if (!g_androidVm || !g_androidActivity)
    {
        // SDL owns the canonical Android Activity. Its getters become
        // available after SDL_Init, which is the same initialization order
        // used by the Simpsons Quest port. Promote SDL's local Activity ref
        // because OpenXR consumes it during loader/instance initialization.
        JNIEnv* sdlEnv = reinterpret_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
        jobject sdlActivity = reinterpret_cast<jobject>(SDL_GetAndroidActivity());
        if (sdlEnv && sdlActivity && sdlEnv->GetJavaVM(&g_androidVm) == JNI_OK)
        {
            g_androidActivity = sdlEnv->NewGlobalRef(sdlActivity);
            sdlEnv->DeleteLocalRef(sdlActivity);
            __android_log_print(ANDROID_LOG_INFO, "CryVR", "Cached SDL Android Activity (vm=%p global=%p)", g_androidVm, g_androidActivity);
        }
    }
    if (!g_androidVm || !g_androidActivity)
    {
        __android_log_print(ANDROID_LOG_ERROR, "CryVR", "Android OpenXR context unavailable (vm=%p activity=%p; SDL: %s)", g_androidVm, g_androidActivity, SDL_GetError());
        return;
    }
    JNIEnv* env = nullptr;
    if (g_androidVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK &&
        g_androidVm->AttachCurrentThread(&env, nullptr) != JNI_OK)
    {
        __android_log_print(ANDROID_LOG_ERROR, "CryVR", "Could not attach native thread to Java VM");
        return;
    }
    if (jniEnv) *jniEnv = env;
    if (activity) *activity = g_androidActivity;
    __android_log_print(ANDROID_LOG_INFO, "CryVR", "Android OpenXR context ready (env=%p activity=%p)", env, g_androidActivity);
}

extern "C" JNIEXPORT void JNICALL
Java_com_nearchuckle_farcry_GameActivity_nativeSetOpenXRActivity(JNIEnv* env, jclass, jobject activity)
{
    if (!env || !activity) return;
    const jint vmResult = env->GetJavaVM(&g_androidVm);
    if (g_androidActivity) env->DeleteGlobalRef(g_androidActivity);
    g_androidActivity = env->NewGlobalRef(activity);
    __android_log_print(ANDROID_LOG_INFO, "CryVR", "Activity JNI handoff (result=%d vm=%p local=%p global=%p)", vmResult, g_androidVm, activity, g_androidActivity);
}
#else
void GetAndroidOpenXRContext(void** jniEnv, void** activity)
{
    if (jniEnv) *jniEnv = nullptr;
    if (activity) *activity = nullptr;
}
#endif

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
        m_loader = SDL_LoadObject(name);
        if (m_loader)
            break;
    }
    if (!m_loader)
    {
        SetError(SDL_GetError());
        return false;
    }
    m_getInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(SDL_LoadFunction(reinterpret_cast<SDL_SharedObject*>(m_loader), "xrGetInstanceProcAddr"));
    if (!m_getInstanceProcAddr)
    {
        SetError("OpenXR loader does not export xrGetInstanceProcAddr");
        return false;
    }
    return true;
}

bool Runtime::LoadInstanceFunctions()
{
#define XR_LOAD(name) if (!LoadInstanceProc(m_getInstanceProcAddr, m_instance, #name, m_##name)) { SetError("Missing OpenXR function: " #name); return false; }
    XR_LOAD(destroyInstance)
    XR_LOAD(getSystem)
    XR_LOAD(enumerateViews)
    XR_LOAD(createSession)
    XR_LOAD(destroySession)
    XR_LOAD(beginSession)
    XR_LOAD(endSession)
    XR_LOAD(pollEvent)
    XR_LOAD(waitFrame)
    XR_LOAD(beginFrame)
    XR_LOAD(endFrame)
    XR_LOAD(locateViews)
    XR_LOAD(createReferenceSpace)
    XR_LOAD(destroySpace)
    XR_LOAD(stringToPath)
    XR_LOAD(createActionSet)
    XR_LOAD(destroyActionSet)
    XR_LOAD(createAction)
    XR_LOAD(suggestBindings)
    XR_LOAD(attachActionSets)
    XR_LOAD(createActionSpace)
    XR_LOAD(syncActions)
    XR_LOAD(getBoolean)
    XR_LOAD(getFloat)
    XR_LOAD(getVector2)
	XR_LOAD(enumerateSwapchainFormats)
	XR_LOAD(createSwapchain)
	XR_LOAD(destroySwapchain)
	XR_LOAD(enumerateSwapchainImages)
	XR_LOAD(acquireSwapchainImage)
	XR_LOAD(waitSwapchainImage)
	XR_LOAD(releaseSwapchainImage)
#undef XR_LOAD
    return true;
}

bool Runtime::Initialize(const char* applicationName, const char* engineName, void* androidJniEnv, void* androidActivity)
{
    if (IsInitialized())
        return true;
    if (!LoadLoader())
        return false;

#ifdef CRYVR_ANDROID
    JavaVM* vm = nullptr;
    if (!androidJniEnv || !androidActivity ||
        reinterpret_cast<JNIEnv*>(androidJniEnv)->GetJavaVM(&vm) != JNI_OK || !vm)
    {
        SetError("Android OpenXR initialization requires a valid JNI environment and Activity");
        return false;
    }
    PFN_xrVoidFunction initializeLoaderFunction = nullptr;
    if (m_getInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", &initializeLoaderFunction) != XR_SUCCESS ||
        !initializeLoaderFunction)
    {
        SetError("OpenXR loader does not expose xrInitializeLoaderKHR on Android");
        return false;
    }
    struct LoaderInitInfoAndroidKHR
    {
        XrStructureType type;
        const void* next;
        void* applicationVM;
        void* applicationContext;
    } loaderInitInfo{};
    loaderInitInfo.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;
    loaderInitInfo.applicationVM = vm;
    loaderInitInfo.applicationContext = androidActivity;
    typedef XrResult (*PFN_InitializeLoaderKHR)(const void*);
    const XrResult initializeResult = reinterpret_cast<PFN_InitializeLoaderKHR>(initializeLoaderFunction)(&loaderInitInfo);
    if (!Check(initializeResult, "xrInitializeLoaderKHR"))
        return false;
#else
    (void)androidJniEnv;
    (void)androidActivity;
#endif

    if (m_getInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
                              reinterpret_cast<PFN_xrVoidFunction*>(&m_enumerateExtensions)) != XR_SUCCESS)
        return false;

    uint32_t extensionCount = 0;
    if (!Check(m_enumerateExtensions(nullptr, 0, &extensionCount, nullptr), "xrEnumerateInstanceExtensionProperties"))
        return false;
    std::vector<XrExtensionProperties> properties(extensionCount);
    for (uint32_t i = 0; i < extensionCount; ++i)
        properties[i].type = 2;
    if (!Check(m_enumerateExtensions(nullptr, extensionCount, &extensionCount, properties.data()), "xrEnumerateInstanceExtensionProperties"))
        return false;

    const char* extensionNames[5]{};
    uint32_t enabledExtensionCount = 0;
    bool hasVulkan = false;
    bool hasAndroid = false;
    bool hasAndroidLoaderInit = false;
    m_metaTouchPlusEnabled = false;
    for (uint32_t i = 0; i < extensionCount; ++i)
    {
        if (strcmp(properties[i].extensionName, "XR_KHR_vulkan_enable2") == 0)
            hasVulkan = true;
        if (strcmp(properties[i].extensionName, "XR_KHR_android_create_instance") == 0)
            hasAndroid = true;
        if (strcmp(properties[i].extensionName, "XR_KHR_loader_init_android") == 0)
            hasAndroidLoaderInit = true;
        if (strcmp(properties[i].extensionName, "XR_META_touch_controller_plus") == 0)
            m_metaTouchPlusEnabled = true;
    }
    if (!hasVulkan)
    {
        SetError("OpenXR runtime does not support XR_KHR_vulkan_enable2");
        return false;
    }
    extensionNames[enabledExtensionCount++] = "XR_KHR_vulkan_enable2";

#ifdef CRYVR_ANDROID
    if (!hasAndroid || !hasAndroidLoaderInit)
    {
        SetError("OpenXR runtime lacks required Android create-instance or loader-init extension");
        return false;
    }
    extensionNames[enabledExtensionCount++] = "XR_KHR_android_create_instance";
    extensionNames[enabledExtensionCount++] = "XR_KHR_loader_init_android";
#else
#endif
    if (m_metaTouchPlusEnabled)
        extensionNames[enabledExtensionCount++] = "XR_META_touch_controller_plus";

    XrInstanceCreateInfo createInfo{};
    createInfo.type = XR_TYPE_INSTANCE_CREATE_INFO;
    strncpy(createInfo.applicationInfo.applicationName, applicationName ? applicationName : "FarCry", XR_MAX_APPLICATION_NAME_SIZE - 1);
    createInfo.applicationInfo.applicationVersion = 1;
    strncpy(createInfo.applicationInfo.engineName, engineName ? engineName : "CryEngine", XR_MAX_ENGINE_NAME_SIZE - 1);
    createInfo.applicationInfo.engineVersion = 1;
    createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    createInfo.enabledExtensionCount = enabledExtensionCount;
    createInfo.enabledExtensionNames = extensionNames;

#ifdef CRYVR_ANDROID
    if (androidJniEnv && androidActivity && hasAndroid)
    {
        JNIEnv* env = reinterpret_cast<JNIEnv*>(androidJniEnv);
        XrInstanceCreateInfoAndroidKHR androidInfo{};
        androidInfo.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
        androidInfo.applicationVM = vm;
        androidInfo.applicationActivity = androidActivity;
        createInfo.next = &androidInfo;
        if (!Check(m_getInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction*>(&m_createInstance)), "xrGetInstanceProcAddr(xrCreateInstance)"))
            return false;
        if (!Check(m_createInstance(&createInfo, &m_instance), "xrCreateInstance"))
            return false;
    }
    else
#endif
    {
        if (!LoadInstanceProc(m_getInstanceProcAddr, XR_NULL_HANDLE, "xrCreateInstance", m_createInstance))
            return false;
        if (!Check(m_createInstance(&createInfo, &m_instance), "xrCreateInstance"))
            return false;
    }

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
    const XrPath leftSelect = path("/user/hand/left/input/select/click");
    const XrPath rightSelect = path("/user/hand/right/input/select/click");
    const XrPath leftThumb = path("/user/hand/left/input/thumbstick");
    const XrPath rightThumb = path("/user/hand/right/input/thumbstick");
    // The Oculus Touch profile does not define the generic select/click path.
    // Use its standard face-button and analog-trigger components instead.
    const XrPath leftTouchSelect = path("/user/hand/left/input/x/click");
    const XrPath rightTouchSelect = path("/user/hand/right/input/a/click");
    const XrPath leftTouchTrigger = path("/user/hand/left/input/trigger/value");
    const XrPath rightTouchTrigger = path("/user/hand/right/input/trigger/value");
    const auto suggest = [this](const char* profileName, const XrActionSuggestedBinding* bindings,
                                uint32_t count)
    {
        XrPath profilePath = 0;
        if (m_stringToPath(m_instance, profileName, &profilePath) < 0 || !profilePath)
            return;
        XrInteractionProfileSuggestedBinding suggested{};
        suggested.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
        suggested.interactionProfile = profilePath;
        suggested.countSuggestedBindings = count;
        suggested.suggestedBindings = bindings;
        // Unsupported profiles are expected: the runtime chooses one that
        // matches connected hardware and ignores unrelated profiles.
        m_suggestBindings(m_instance, &suggested);
    };

    const XrActionSuggestedBinding oculusBindings[] = {
        {m_selectAction, leftTouchSelect}, {m_selectAction, rightTouchSelect},
        {m_triggerAction, leftTouchTrigger}, {m_triggerAction, rightTouchTrigger},
        {m_moveAction, leftThumb}, {m_moveAction, rightThumb}
    };
    suggest("/interaction_profiles/oculus/touch_controller", oculusBindings, 6);

    const XrPath leftMetaTrigger = path("/user/hand/left/input/trigger/value");
    const XrPath rightMetaTrigger = path("/user/hand/right/input/trigger/value");
    const XrActionSuggestedBinding metaBindings[] = {
        {m_triggerAction, leftMetaTrigger}, {m_triggerAction, rightMetaTrigger},
        {m_moveAction, leftThumb}, {m_moveAction, rightThumb}
    };
    if (m_metaTouchPlusEnabled)
        suggest("/interaction_profiles/meta/touch_controller_plus", metaBindings, 4);

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
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.format = format;
    createInfo.sampleCount = 1;
    createInfo.width = width;
    createInfo.height = height;
    createInfo.faceCount = 1;
    createInfo.arraySize = arraySize;
    createInfo.mipCount = 1;
    if (!Check(m_createSwapchain(m_session, &createInfo, &swapchain.handle), "xrCreateSwapchain"))
        return false;

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
        const XrFlags64 requiredViewFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT |
                                            XR_VIEW_STATE_POSITION_VALID_BIT;
        frame.viewsValid = frame.viewCount == 2 &&
                           (viewState.viewStateFlags & requiredViewFlags) == requiredViewFlags;
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
            XrActionStateGetInfo getInfo{};
            getInfo.type = XR_TYPE_ACTION_STATE_GET_INFO;
            getInfo.action = m_selectAction;
            getInfo.subactionPath = paths[i];
            XrActionStateBoolean select{};
            select.type = XR_TYPE_ACTION_STATE_BOOLEAN;
            if (m_getBoolean(m_session, &getInfo, &select) >= 0)
            {
                states[i]->active = states[i]->active || select.isActive == XR_TRUE;
                states[i]->select = states[i]->select || select.currentState == XR_TRUE;
            }
            getInfo.action = m_triggerAction;
            XrActionStateFloat trigger{};
            trigger.type = XR_TYPE_ACTION_STATE_FLOAT;
            if (m_getFloat(m_session, &getInfo, &trigger) >= 0)
            {
                states[i]->active = states[i]->active || trigger.isActive == XR_TRUE;
                states[i]->select = states[i]->select ||
                                    (trigger.isActive == XR_TRUE && trigger.currentState > 0.55f);
            }
            getInfo.action = m_moveAction;
            XrActionStateVector2f move{};
            move.type = XR_TYPE_ACTION_STATE_VECTOR2F;
            if (m_getVector2(m_session, &getInfo, &move) >= 0)
            {
                states[i]->thumbstickX = move.currentState.x;
                states[i]->thumbstickY = move.currentState.y;
            }
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
    if (m_stageSpace && m_destroySpace) m_destroySpace(m_stageSpace);
    if (m_session && m_destroySession) m_destroySession(m_session);
    if (m_gameplayActionSet && m_destroyActionSet) m_destroyActionSet(m_gameplayActionSet);
    if (m_instance && m_destroyInstance) m_destroyInstance(m_instance);
    m_stageSpace = XR_NULL_HANDLE;
    m_session = XR_NULL_HANDLE;
    m_gameplayActionSet = XR_NULL_HANDLE;
    m_instance = XR_NULL_HANDLE;
    if (m_loader)
        SDL_UnloadObject(reinterpret_cast<SDL_SharedObject*>(m_loader));
    m_loader = nullptr;
}
}
