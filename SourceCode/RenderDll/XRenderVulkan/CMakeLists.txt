project(XRenderVulkan)

add_library(${PROJECT_NAME} SHARED VulkanRenderer.cpp VulkanTextureDecode.cpp)
target_compile_definitions(${PROJECT_NAME} PRIVATE XRENDERNULL_IMPORTS)
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/SourceCode/CryCommon
    ${CMAKE_SOURCE_DIR}/SourceCode/RenderDll
    ${CMAKE_SOURCE_DIR}/SourceCode/CryVR
)
target_link_libraries(${PROJECT_NAME} PRIVATE XRenderNULL CryVR)
if (ANDROID)
    target_link_libraries(${PROJECT_NAME} PRIVATE log)
    # Reject missing common-renderer symbols at build time, before dlopen.
    target_link_options(${PROJECT_NAME} PRIVATE "-Wl,--no-undefined")
endif()
