project(XRenderVulkan)

add_library(${PROJECT_NAME} SHARED VulkanRenderer.cpp VulkanTextureDecode.cpp)
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/SourceCode/CryCommon
    ${CMAKE_SOURCE_DIR}/SourceCode/RenderDll
    ${CMAKE_SOURCE_DIR}/SourceCode/CryVR
)
target_link_libraries(${PROJECT_NAME} PRIVATE XRenderNULL CryVR SDL3::SDL3)
