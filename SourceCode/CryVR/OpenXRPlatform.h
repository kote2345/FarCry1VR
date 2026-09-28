#ifndef CRY_VR_OPENXR_PLATFORM_H
#define CRY_VR_OPENXR_PLATFORM_H

#include "openxr_api.h"
#include <vector>

namespace CryVR
{
namespace Platform
{
// Platform-owned inputs used only while creating the OpenXR loader/instance.
// Desktop backends leave these fields null; Android supplies VM + Activity.
struct ApplicationContext
{
    void* javaVm = nullptr;
    void* activity = nullptr;
    char error[192]{};
};

struct InstanceCreateInfoStorage
{
    XrInstanceCreateInfoAndroidKHR android{};
};

ApplicationContext GetApplicationContext();
bool InitializeLoader(PFN_xrGetInstanceProcAddr getProc,
                      const ApplicationContext& context, char* error, size_t errorSize);
void AppendRequiredInstanceExtensions(std::vector<const char*>& extensions);
bool PrepareInstanceCreateInfo(XrInstanceCreateInfo& createInfo,
                               const ApplicationContext& context,
                               InstanceCreateInfoStorage& storage,
                               char* error, size_t errorSize);
}
}

#endif
