#include "OpenXRPlatform.h"

#include <stdio.h>
#include <string.h>

#ifdef CRYVR_ANDROID
#include <SDL3/SDL_system.h>
#include <jni.h>
#include <android/log.h>
#endif

namespace CryVR
{
namespace Platform
{
#ifdef CRYVR_ANDROID
namespace
{
JavaVM* g_javaVm = nullptr;
jobject g_activity = nullptr;

void SetError(char* error, size_t errorSize, const char* message)
{
    if (error && errorSize)
        snprintf(error, errorSize, "%s", message ? message : "Android OpenXR setup failed");
}
}

ApplicationContext GetApplicationContext()
{
    ApplicationContext context;
    if (!g_javaVm || !g_activity)
    {
        // SDL's getters are the canonical source after SDL_Init. Promote its
        // local Activity reference because the OpenXR loader retains context.
        JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
        jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
        if (env && activity && env->GetJavaVM(&g_javaVm) == JNI_OK)
        {
            g_activity = env->NewGlobalRef(activity);
            env->DeleteLocalRef(activity);
        }
    }
    if (!g_javaVm || !g_activity)
    {
        SetError(context.error, sizeof(context.error), "Android Activity/JavaVM unavailable from SDL and Activity handoff");
        return context;
    }
    context.javaVm = g_javaVm;
    context.activity = g_activity;
    return context;
}

extern "C" JNIEXPORT void JNICALL
Java_com_nearchuckle_farcry_GameActivity_nativeSetOpenXRActivity(JNIEnv* env, jclass, jobject activity)
{
    if (!env || !activity)
        return;
    env->GetJavaVM(&g_javaVm);
    if (g_activity)
        env->DeleteGlobalRef(g_activity);
    g_activity = env->NewGlobalRef(activity);
    __android_log_print(ANDROID_LOG_INFO, "CryVR", "OpenXR Activity context registered (vm=%p activity=%p)", g_javaVm, g_activity);
}

bool InitializeLoader(PFN_xrGetInstanceProcAddr getProc,
                      const ApplicationContext& context, char* error, size_t errorSize)
{
    if (!context.javaVm || !context.activity)
    {
        SetError(error, errorSize, context.error);
        return false;
    }
    PFN_xrVoidFunction function = nullptr;
    if (!getProc || getProc(XR_NULL_HANDLE, "xrInitializeLoaderKHR", &function) != XR_SUCCESS || !function)
    {
        // The Android loader-init entry point is optional in the loader API;
        // when exposed it must run before any instance enumeration.
        return true;
    }
    struct LoaderInitInfoAndroidKHR
    {
        XrStructureType type;
        const void* next;
        void* applicationVM;
        void* applicationContext;
    } info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR, nullptr, context.javaVm, context.activity};
    using InitializeLoaderKHR = XrResult (*)(const void*);
    const XrResult result = reinterpret_cast<InitializeLoaderKHR>(function)(&info);
    if (result == XR_SUCCESS)
        return true;
    char message[128];
    snprintf(message, sizeof(message), "xrInitializeLoaderKHR failed (%d)", result);
    SetError(error, errorSize, message);
    return false;
}

void AppendRequiredInstanceExtensions(std::vector<const char*>& extensions)
{
    extensions.push_back("XR_KHR_android_create_instance");
}

bool PrepareInstanceCreateInfo(XrInstanceCreateInfo& createInfo,
                               const ApplicationContext& context,
                               InstanceCreateInfoStorage& storage,
                               char* error, size_t errorSize)
{
    if (!context.javaVm || !context.activity)
    {
        SetError(error, errorSize, context.error);
        return false;
    }
    storage.android = {};
    storage.android.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
    storage.android.applicationVM = context.javaVm;
    storage.android.applicationActivity = context.activity;
    createInfo.next = &storage.android;
    return true;
}
#else
ApplicationContext GetApplicationContext()
{
    return ApplicationContext{};
}

bool InitializeLoader(PFN_xrGetInstanceProcAddr, const ApplicationContext&, char*, size_t)
{
    return true;
}

void AppendRequiredInstanceExtensions(std::vector<const char*>&)
{
}

bool PrepareInstanceCreateInfo(XrInstanceCreateInfo& createInfo,
                               const ApplicationContext&,
                               InstanceCreateInfoStorage&, char*, size_t)
{
    createInfo.next = nullptr;
    return true;
}
#endif
}
}
