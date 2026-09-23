#ifndef CRYVR_OPENXR_API_H
#define CRYVR_OPENXR_API_H

// Deliberately small ABI-compatible subset of the Khronos OpenXR headers.
// The engine loads the OpenXR loader at runtime, so neither Quest nor PCVR
// requires a link-time dependency on a particular runtime.

#include <stdint.h>

#define XR_MAKE_VERSION(major, minor, patch) \
    ((((uint64_t)(major) & 0xffffULL) << 48) | (((uint64_t)(minor) & 0xffffULL) << 32) | ((uint64_t)(patch) & 0xffffffffULL))
#define XR_API_VERSION_1_0 XR_MAKE_VERSION(1, 0, 0)
#define XR_TRUE 1
#define XR_FALSE 0
#define XR_NULL_HANDLE 0
#define XR_MAX_EXTENSION_NAME_SIZE 128
#define XR_MAX_API_LAYER_NAME_SIZE 256
#define XR_MAX_APPLICATION_NAME_SIZE 128
#define XR_MAX_ENGINE_NAME_SIZE 128
#define XR_MAX_ACTION_SET_NAME_SIZE 64
#define XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE 128
#define XR_MAX_ACTION_NAME_SIZE 64
#define XR_MAX_LOCALIZED_ACTION_NAME_SIZE 128

typedef struct XrInstance_T* XrInstance;
typedef struct XrSession_T* XrSession;
typedef struct XrSpace_T* XrSpace;
typedef struct XrAction_T* XrAction;
typedef struct XrActionSet_T* XrActionSet;
typedef struct XrSwapchain_T* XrSwapchain;
typedef uint64_t XrVersion;
typedef uint64_t XrFlags64;
typedef uint64_t XrSystemId;
typedef uint64_t XrPath;
typedef int64_t XrTime;
typedef int64_t XrDuration;
typedef uint32_t XrBool32;
typedef int32_t XrResult;
typedef int32_t XrStructureType;
typedef void (*PFN_xrVoidFunction)(void);

enum {
    XR_SUCCESS = 0,
    XR_TIMEOUT_EXPIRED = 1,
    XR_SESSION_LOSS_PENDING = 3,
    XR_EVENT_UNAVAILABLE = 4,
    XR_SESSION_NOT_FOCUSED = 8,
    XR_ERROR_FUNCTION_UNSUPPORTED = -7,
    XR_ERROR_EXTENSION_NOT_PRESENT = -9,
    XR_ERROR_SESSION_RUNNING = -14,
    XR_ERROR_SESSION_NOT_RUNNING = -16,
    XR_ERROR_SESSION_LOST = -17,
    XR_ERROR_SYSTEM_INVALID = -18,
    XR_ERROR_FORM_FACTOR_UNSUPPORTED = -34,
    XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED = -41,
};

enum {
    XR_TYPE_INSTANCE_CREATE_INFO = 3,
    XR_TYPE_SYSTEM_GET_INFO = 4,
    XR_TYPE_VIEW_LOCATE_INFO = 6,
    XR_TYPE_VIEW = 7,
    XR_TYPE_SESSION_CREATE_INFO = 8,
    XR_TYPE_SWAPCHAIN_CREATE_INFO = 9,
    XR_TYPE_SESSION_BEGIN_INFO = 10,
    XR_TYPE_VIEW_STATE = 11,
    XR_TYPE_FRAME_END_INFO = 12,
    XR_TYPE_EVENT_DATA_BUFFER = 16,
    XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED = 18,
    XR_TYPE_ACTION_STATE_BOOLEAN = 23,
    XR_TYPE_ACTION_STATE_FLOAT = 24,
    XR_TYPE_ACTION_STATE_VECTOR2F = 25,
    XR_TYPE_ACTION_STATE_POSE = 27,
    XR_TYPE_ACTION_SET_CREATE_INFO = 28,
    XR_TYPE_ACTION_CREATE_INFO = 29,
    XR_TYPE_FRAME_WAIT_INFO = 33,
    XR_TYPE_COMPOSITION_LAYER_PROJECTION = 35,
    XR_TYPE_REFERENCE_SPACE_CREATE_INFO = 37,
    XR_TYPE_ACTION_SPACE_CREATE_INFO = 38,
    XR_TYPE_VIEW_CONFIGURATION_VIEW = 41,
    XR_TYPE_SPACE_LOCATION = 42,
    XR_TYPE_FRAME_STATE = 44,
    XR_TYPE_FRAME_BEGIN_INFO = 46,
    XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW = 48,
    XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING = 51,
    XR_TYPE_ACTION_STATE_GET_INFO = 58,
    XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO = 60,
    XR_TYPE_ACTIONS_SYNC_INFO = 61,
    XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO = 55,
    XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO = 56,
    XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO = 57,
    XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR = 1000008000,
    XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR = 1000089000,
    XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR = 1000025000,
    XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR = 1000025001,
    XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR = 1000025002,
    XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR = 1000090000,
    XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR = 1000090001,
    XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR = 1000090003,
};

enum {
    XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY = 1,
    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO = 2,
    XR_REFERENCE_SPACE_TYPE_VIEW = 1,
    XR_REFERENCE_SPACE_TYPE_LOCAL = 2,
    XR_REFERENCE_SPACE_TYPE_STAGE = 3,
    XR_ENVIRONMENT_BLEND_MODE_OPAQUE = 1,
    XR_SESSION_STATE_READY = 2,
    XR_SESSION_STATE_SYNCHRONIZED = 3,
    XR_SESSION_STATE_VISIBLE = 4,
    XR_SESSION_STATE_FOCUSED = 5,
    XR_SESSION_STATE_STOPPING = 6,
    XR_SESSION_STATE_LOSS_PENDING = 7,
    XR_SESSION_STATE_EXITING = 8,
    XR_ACTION_TYPE_BOOLEAN_INPUT = 1,
    XR_ACTION_TYPE_FLOAT_INPUT = 2,
    XR_ACTION_TYPE_VECTOR2F_INPUT = 3,
    XR_ACTION_TYPE_POSE_INPUT = 4,
    XR_SPACE_LOCATION_POSITION_VALID_BIT = 1,
    XR_SPACE_LOCATION_ORIENTATION_VALID_BIT = 2,
    XR_VIEW_STATE_ORIENTATION_VALID_BIT = 1,
    XR_VIEW_STATE_POSITION_VALID_BIT = 2,
    XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT = 1,
};

typedef struct XrApplicationInfo {
    char applicationName[XR_MAX_APPLICATION_NAME_SIZE];
    uint32_t applicationVersion;
    char engineName[XR_MAX_ENGINE_NAME_SIZE];
    uint32_t engineVersion;
    XrVersion apiVersion;
} XrApplicationInfo;

typedef struct XrInstanceCreateInfo {
    XrStructureType type;
    const void* next;
    XrFlags64 createFlags;
    XrApplicationInfo applicationInfo;
    uint32_t enabledApiLayerCount;
    const char* const* enabledApiLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* enabledExtensionNames;
} XrInstanceCreateInfo;

typedef struct XrInstanceCreateInfoAndroidKHR {
    XrStructureType type;
    const void* next;
    void* applicationVM;
    void* applicationActivity;
} XrInstanceCreateInfoAndroidKHR;

typedef struct XrExtensionProperties {
    XrStructureType type;
    void* next;
    char extensionName[XR_MAX_EXTENSION_NAME_SIZE];
    uint32_t extensionVersion;
} XrExtensionProperties;

typedef struct XrSystemGetInfo {
    XrStructureType type;
    const void* next;
    int32_t formFactor;
} XrSystemGetInfo;

typedef struct XrViewConfigurationView {
    XrStructureType type;
    void* next;
    uint32_t recommendedImageRectWidth;
    uint32_t maxImageRectWidth;
    uint32_t recommendedImageRectHeight;
    uint32_t maxImageRectHeight;
    uint32_t recommendedSwapchainSampleCount;
    uint32_t maxSwapchainSampleCount;
} XrViewConfigurationView;

typedef struct XrVector3f { float x, y, z; } XrVector3f;
typedef struct XrQuaternionf { float x, y, z, w; } XrQuaternionf;
typedef struct XrPosef { XrQuaternionf orientation; XrVector3f position; } XrPosef;
typedef struct XrFovf { float angleLeft, angleRight, angleUp, angleDown; } XrFovf;
typedef struct XrView {
    XrStructureType type;
    void* next;
    XrPosef pose;
    XrFovf fov;
} XrView;

typedef struct XrSessionCreateInfo {
    XrStructureType type;
    const void* next;
    XrFlags64 createFlags;
    XrSystemId systemId;
} XrSessionCreateInfo;
typedef struct XrSessionBeginInfo {
    XrStructureType type;
    const void* next;
    int32_t primaryViewConfigurationType;
} XrSessionBeginInfo;
typedef struct XrReferenceSpaceCreateInfo {
    XrStructureType type;
    const void* next;
    int32_t referenceSpaceType;
    XrPosef poseInReferenceSpace;
} XrReferenceSpaceCreateInfo;
typedef struct XrViewLocateInfo {
    XrStructureType type;
    const void* next;
    int32_t viewConfigurationType;
    XrTime displayTime;
    XrSpace space;
} XrViewLocateInfo;
typedef struct XrViewState {
    XrStructureType type;
    void* next;
    XrFlags64 viewStateFlags;
} XrViewState;
typedef struct XrFrameWaitInfo { XrStructureType type; const void* next; } XrFrameWaitInfo;
typedef struct XrFrameState {
    XrStructureType type;
    void* next;
    XrTime predictedDisplayTime;
    XrDuration predictedDisplayPeriod;
    XrBool32 shouldRender;
} XrFrameState;
typedef struct XrFrameBeginInfo { XrStructureType type; const void* next; } XrFrameBeginInfo;

typedef struct XrSwapchainCreateInfo {
    XrStructureType type;
    const void* next;
    XrFlags64 createFlags;
    XrFlags64 usageFlags;
    int64_t format;
    uint32_t sampleCount;
    uint32_t width;
    uint32_t height;
    uint32_t faceCount;
    uint32_t arraySize;
    uint32_t mipCount;
} XrSwapchainCreateInfo;
typedef struct XrSwapchainImageBaseHeader { XrStructureType type; void* next; } XrSwapchainImageBaseHeader;
typedef struct XrSwapchainImageVulkan2KHR {
    XrStructureType type;
    void* next;
    void* image;
} XrSwapchainImageVulkan2KHR;
typedef struct XrSwapchainImageAcquireInfo { XrStructureType type; const void* next; } XrSwapchainImageAcquireInfo;
typedef struct XrSwapchainImageWaitInfo { XrStructureType type; const void* next; XrDuration timeout; } XrSwapchainImageWaitInfo;
typedef struct XrSwapchainImageReleaseInfo { XrStructureType type; const void* next; } XrSwapchainImageReleaseInfo;
typedef struct XrOffset2Di { int32_t x, y; } XrOffset2Di;
typedef struct XrExtent2Di { int32_t width, height; } XrExtent2Di;
typedef struct XrRect2Di { XrOffset2Di offset; XrExtent2Di extent; } XrRect2Di;
typedef struct XrSwapchainSubImage {
    XrSwapchain swapchain;
    XrRect2Di imageRect;
    uint32_t imageArrayIndex;
} XrSwapchainSubImage;
typedef struct XrCompositionLayerProjectionView {
    XrStructureType type;
    const void* next;
    XrPosef pose;
    XrFovf fov;
    XrSwapchainSubImage subImage;
} XrCompositionLayerProjectionView;
typedef struct XrCompositionLayerProjection {
    XrStructureType type;
    const void* next;
    XrFlags64 layerFlags;
    XrSpace space;
    uint32_t viewCount;
    const XrCompositionLayerProjectionView* views;
} XrCompositionLayerProjection;

typedef struct XrEventDataBuffer {
    XrStructureType type;
    const void* next;
    uint8_t varying[4000];
} XrEventDataBuffer;
typedef struct XrEventDataSessionStateChanged {
    XrStructureType type;
    const void* next;
    XrSession session;
    int32_t state;
    XrTime time;
} XrEventDataSessionStateChanged;

typedef struct XrCompositionLayerBaseHeader {
    XrStructureType type;
    const void* next;
    XrFlags64 layerFlags;
    XrSpace space;
} XrCompositionLayerBaseHeader;
typedef struct XrFrameEndInfo {
    XrStructureType type;
    const void* next;
    XrTime displayTime;
    int32_t environmentBlendMode;
    uint32_t layerCount;
    const XrCompositionLayerBaseHeader* const* layers;
} XrFrameEndInfo;

typedef struct XrGraphicsBindingVulkan2KHR {
    XrStructureType type;
    const void* next;
    void* instance;
    void* physicalDevice;
    void* device;
    uint32_t queueFamilyIndex;
    uint32_t queueIndex;
} XrGraphicsBindingVulkan2KHR;
typedef struct XrGraphicsRequirementsVulkan2KHR {
    XrStructureType type;
    void* next;
    XrVersion minApiVersionSupported;
    XrVersion maxApiVersionSupported;
} XrGraphicsRequirementsVulkan2KHR;
typedef struct XrVulkanGraphicsDeviceGetInfoKHR {
    XrStructureType type;
    const void* next;
    XrSystemId systemId;
    void* vulkanInstance;
} XrVulkanGraphicsDeviceGetInfoKHR;
typedef struct XrVulkanInstanceCreateInfoKHR {
    XrStructureType type;
    const void* next;
    XrSystemId systemId;
    XrFlags64 createFlags;
    void* pfnGetInstanceProcAddr;
    const void* vulkanCreateInfo;
    const void* vulkanAllocator;
} XrVulkanInstanceCreateInfoKHR;
typedef struct XrVulkanDeviceCreateInfoKHR {
    XrStructureType type;
    const void* next;
    XrSystemId systemId;
    XrFlags64 createFlags;
    void* pfnGetInstanceProcAddr;
    void* vulkanPhysicalDevice;
    const void* vulkanCreateInfo;
    const void* vulkanAllocator;
} XrVulkanDeviceCreateInfoKHR;

typedef struct XrActionSetCreateInfo {
    XrStructureType type;
    const void* next;
    char actionSetName[XR_MAX_ACTION_SET_NAME_SIZE];
    char localizedActionSetName[XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE];
    uint32_t priority;
} XrActionSetCreateInfo;
typedef struct XrActionCreateInfo {
    XrStructureType type;
    const void* next;
    char actionName[XR_MAX_ACTION_NAME_SIZE];
    int32_t actionType;
    uint32_t countSubactionPaths;
    const XrPath* subactionPaths;
    char localizedActionName[XR_MAX_LOCALIZED_ACTION_NAME_SIZE];
} XrActionCreateInfo;
typedef struct XrActionSuggestedBinding { XrAction action; XrPath binding; } XrActionSuggestedBinding;
typedef struct XrInteractionProfileSuggestedBinding {
    XrStructureType type;
    const void* next;
    XrPath interactionProfile;
    uint32_t countSuggestedBindings;
    const XrActionSuggestedBinding* suggestedBindings;
} XrInteractionProfileSuggestedBinding;
typedef struct XrSessionActionSetsAttachInfo {
    XrStructureType type;
    const void* next;
    uint32_t countActionSets;
    const XrActionSet* actionSets;
} XrSessionActionSetsAttachInfo;
typedef struct XrActionSpaceCreateInfo {
    XrStructureType type;
    const void* next;
    XrAction action;
    XrPath subactionPath;
    XrPosef poseInActionSpace;
} XrActionSpaceCreateInfo;
typedef struct XrActiveActionSet {
    XrActionSet actionSet;
    XrPath subactionPath;
} XrActiveActionSet;
typedef struct XrActionsSyncInfo {
    XrStructureType type;
    const void* next;
    uint32_t countActiveActionSets;
    const XrActiveActionSet* activeActionSets;
} XrActionsSyncInfo;

typedef struct XrActionStateGetInfo {
    XrStructureType type;
    const void* next;
    XrAction action;
    XrPath subactionPath;
} XrActionStateGetInfo;
typedef struct XrActionStateBoolean {
    XrStructureType type;
    void* next;
    XrBool32 currentState;
    XrBool32 changedSinceLastSync;
    XrTime lastChangeTime;
    XrBool32 isActive;
} XrActionStateBoolean;
typedef struct XrActionStateFloat {
    XrStructureType type;
    void* next;
    float currentState;
    XrBool32 changedSinceLastSync;
    XrTime lastChangeTime;
    XrBool32 isActive;
} XrActionStateFloat;
typedef struct XrActionStateVector2f {
    XrStructureType type;
    void* next;
    struct { float x, y; } currentState;
    XrBool32 changedSinceLastSync;
    XrTime lastChangeTime;
    XrBool32 isActive;
} XrActionStateVector2f;

typedef XrResult (*PFN_xrGetInstanceProcAddr)(XrInstance, const char*, PFN_xrVoidFunction*);
typedef XrResult (*PFN_xrEnumerateInstanceExtensionProperties)(const char*, uint32_t, uint32_t*, XrExtensionProperties*);
typedef XrResult (*PFN_xrCreateInstance)(const XrInstanceCreateInfo*, XrInstance*);
typedef XrResult (*PFN_xrDestroyInstance)(XrInstance);
typedef XrResult (*PFN_xrGetSystem)(XrInstance, const XrSystemGetInfo*, XrSystemId*);
typedef XrResult (*PFN_xrEnumerateViewConfigurationViews)(XrInstance, XrSystemId, int32_t, uint32_t, uint32_t*, XrViewConfigurationView*);
typedef XrResult (*PFN_xrCreateSession)(XrInstance, const XrSessionCreateInfo*, XrSession*);
typedef XrResult (*PFN_xrDestroySession)(XrSession);
typedef XrResult (*PFN_xrBeginSession)(XrSession, const XrSessionBeginInfo*);
typedef XrResult (*PFN_xrEndSession)(XrSession);
typedef XrResult (*PFN_xrPollEvent)(XrInstance, XrEventDataBuffer*);
typedef XrResult (*PFN_xrWaitFrame)(XrSession, const XrFrameWaitInfo*, XrFrameState*);
typedef XrResult (*PFN_xrBeginFrame)(XrSession, const XrFrameBeginInfo*);
typedef XrResult (*PFN_xrEndFrame)(XrSession, const XrFrameEndInfo*);
typedef XrResult (*PFN_xrLocateViews)(XrSession, const XrViewLocateInfo*, XrViewState*, uint32_t, uint32_t*, XrView*);
typedef XrResult (*PFN_xrCreateReferenceSpace)(XrSession, const XrReferenceSpaceCreateInfo*, XrSpace*);
typedef XrResult (*PFN_xrDestroySpace)(XrSpace);
typedef XrResult (*PFN_xrStringToPath)(XrInstance, const char*, XrPath*);
typedef XrResult (*PFN_xrCreateActionSet)(XrInstance, const XrActionSetCreateInfo*, XrActionSet*);
typedef XrResult (*PFN_xrDestroyActionSet)(XrActionSet);
typedef XrResult (*PFN_xrCreateAction)(XrActionSet, const XrActionCreateInfo*, XrAction*);
typedef XrResult (*PFN_xrSuggestInteractionProfileBindings)(XrInstance, const XrInteractionProfileSuggestedBinding*);
typedef XrResult (*PFN_xrAttachSessionActionSets)(XrSession, const XrSessionActionSetsAttachInfo*);
typedef XrResult (*PFN_xrCreateActionSpace)(XrSession, const XrActionSpaceCreateInfo*, XrSpace*);
typedef XrResult (*PFN_xrSyncActions)(XrSession, const XrActionsSyncInfo*);
typedef XrResult (*PFN_xrGetActionStateBoolean)(XrSession, const XrActionStateGetInfo*, XrActionStateBoolean*);
typedef XrResult (*PFN_xrGetActionStateFloat)(XrSession, const XrActionStateGetInfo*, XrActionStateFloat*);
typedef XrResult (*PFN_xrGetActionStateVector2f)(XrSession, const XrActionStateGetInfo*, XrActionStateVector2f*);
typedef XrResult (*PFN_xrGetVulkanGraphicsRequirements2KHR)(XrInstance, XrSystemId, XrGraphicsRequirementsVulkan2KHR*);
typedef XrResult (*PFN_xrGetVulkanGraphicsDevice2KHR)(XrInstance, const XrVulkanGraphicsDeviceGetInfoKHR*, void**);
typedef XrResult (*PFN_xrCreateVulkanInstanceKHR)(XrInstance, const XrVulkanInstanceCreateInfoKHR*, void**, int32_t*);
typedef XrResult (*PFN_xrCreateVulkanDeviceKHR)(XrInstance, const XrVulkanDeviceCreateInfoKHR*, void**, int32_t*);
typedef XrResult (*PFN_xrEnumerateSwapchainFormats)(XrSession, uint32_t, uint32_t*, int64_t*);
typedef XrResult (*PFN_xrCreateSwapchain)(XrSession, const XrSwapchainCreateInfo*, XrSwapchain*);
typedef XrResult (*PFN_xrDestroySwapchain)(XrSwapchain);
typedef XrResult (*PFN_xrEnumerateSwapchainImages)(XrSwapchain, uint32_t, uint32_t*, XrSwapchainImageBaseHeader*);
typedef XrResult (*PFN_xrAcquireSwapchainImage)(XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t*);
typedef XrResult (*PFN_xrWaitSwapchainImage)(XrSwapchain, const XrSwapchainImageWaitInfo*);
typedef XrResult (*PFN_xrReleaseSwapchainImage)(XrSwapchain, const XrSwapchainImageReleaseInfo*);

#endif
