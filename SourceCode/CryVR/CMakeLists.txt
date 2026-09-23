project(CryVR)

find_program(CRYVR_GLSLC glslc
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
if (NOT CRYVR_GLSLC)
    message(FATAL_ERROR "glslc is required to compile the Vulkan XR panel shaders")
endif()
set(CRYVR_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(CRYVR_PANEL_VERTEX_SPV "${CRYVR_GENERATED_DIR}/vr_panel.vert.spv")
set(CRYVR_PANEL_FRAGMENT_SPV "${CRYVR_GENERATED_DIR}/vr_panel.frag.spv")
set(CRYVR_PANEL_SHADER_HEADER "${CRYVR_GENERATED_DIR}/VulkanPanelShaders.h")
add_custom_command(
    OUTPUT "${CRYVR_PANEL_SHADER_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CRYVR_GENERATED_DIR}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_panel.vert" -o "${CRYVR_PANEL_VERTEX_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_panel.frag" -o "${CRYVR_PANEL_FRAGMENT_SPV}"
    COMMAND "${CMAKE_COMMAND}"
            "-DVERTEX_SPV=${CRYVR_PANEL_VERTEX_SPV}"
            "-DFRAGMENT_SPV=${CRYVR_PANEL_FRAGMENT_SPV}"
            "-DOUTPUT_HEADER=${CRYVR_PANEL_SHADER_HEADER}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
    DEPENDS shaders/vr_panel.vert shaders/vr_panel.frag cmake/EmbedSpirv.cmake
    VERBATIM)
add_custom_target(CryVRPanelShaders DEPENDS "${CRYVR_PANEL_SHADER_HEADER}")

set(CRYVR_SCENE_VERTEX_SPV "${CRYVR_GENERATED_DIR}/vr_scene.vert.spv")
set(CRYVR_SCENE_FRAGMENT_SPV "${CRYVR_GENERATED_DIR}/vr_scene.frag.spv")
set(CRYVR_SCENE_COLOR_VERTEX_SPV "${CRYVR_GENERATED_DIR}/vr_scene_color.vert.spv")
set(CRYVR_SCENE_COLOR_FRAGMENT_SPV "${CRYVR_GENERATED_DIR}/vr_scene_color.frag.spv")
set(CRYVR_SCENE_TEXTURE_VERTEX_SPV "${CRYVR_GENERATED_DIR}/vr_scene_texture.vert.spv")
set(CRYVR_SCENE_TEXTURE_FRAGMENT_SPV "${CRYVR_GENERATED_DIR}/vr_scene_texture.frag.spv")
set(CRYVR_SCENE_TEXTURE_COLOR_VERTEX_SPV "${CRYVR_GENERATED_DIR}/vr_scene_texture_color.vert.spv")
set(CRYVR_SCENE_TEXTURE_COLOR_FRAGMENT_SPV "${CRYVR_GENERATED_DIR}/vr_scene_texture_color.frag.spv")
set(CRYVR_SCENE_MULTITEXTURE_COLOR_VERTEX_SPV "${CRYVR_GENERATED_DIR}/vr_scene_multitexture_color.vert.spv")
set(CRYVR_SCENE_MULTITEXTURE_COLOR_FRAGMENT_SPV "${CRYVR_GENERATED_DIR}/vr_scene_multitexture_color.frag.spv")
set(CRYVR_SCENE_TEXTURE_LIT_VERTEX_SPV "${CRYVR_GENERATED_DIR}/vr_scene_texture_lit.vert.spv")
set(CRYVR_SCENE_TEXTURE_LIT_COLOR_VERTEX_SPV "${CRYVR_GENERATED_DIR}/vr_scene_texture_lit_color.vert.spv")
set(CRYVR_SCENE_TEXTURE_BUMP_VERTEX_SPV "${CRYVR_GENERATED_DIR}/vr_scene_texture_bump.vert.spv")
set(CRYVR_SCENE_TEXTURE_BUMP_COLOR_VERTEX_SPV "${CRYVR_GENERATED_DIR}/vr_scene_texture_bump_color.vert.spv")
set(CRYVR_SCENE_TEXTURE_BUMP_FRAGMENT_SPV "${CRYVR_GENERATED_DIR}/vr_scene_texture_bump.frag.spv")
set(CRYVR_SCENE_SHADER_HEADER "${CRYVR_GENERATED_DIR}/VulkanSceneShaders.h")
add_custom_command(
    OUTPUT "${CRYVR_SCENE_SHADER_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CRYVR_GENERATED_DIR}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene.vert" -o "${CRYVR_SCENE_VERTEX_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene.frag" -o "${CRYVR_SCENE_FRAGMENT_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_color.vert" -o "${CRYVR_SCENE_COLOR_VERTEX_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_color.frag" -o "${CRYVR_SCENE_COLOR_FRAGMENT_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_texture.vert" -o "${CRYVR_SCENE_TEXTURE_VERTEX_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_texture.frag" -o "${CRYVR_SCENE_TEXTURE_FRAGMENT_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_texture_color.vert" -o "${CRYVR_SCENE_TEXTURE_COLOR_VERTEX_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_texture_color.frag" -o "${CRYVR_SCENE_TEXTURE_COLOR_FRAGMENT_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_multitexture_color.vert" -o "${CRYVR_SCENE_MULTITEXTURE_COLOR_VERTEX_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_multitexture_color.frag" -o "${CRYVR_SCENE_MULTITEXTURE_COLOR_FRAGMENT_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_texture_lit.vert" -o "${CRYVR_SCENE_TEXTURE_LIT_VERTEX_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_texture_lit_color.vert" -o "${CRYVR_SCENE_TEXTURE_LIT_COLOR_VERTEX_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_texture_bump.vert" -o "${CRYVR_SCENE_TEXTURE_BUMP_VERTEX_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_texture_bump_color.vert" -o "${CRYVR_SCENE_TEXTURE_BUMP_COLOR_VERTEX_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O -DVR_BUMP_MAP=1
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_texture_color.frag" -o "${CRYVR_SCENE_TEXTURE_BUMP_FRAGMENT_SPV}"
    COMMAND "${CMAKE_COMMAND}" "-DVERTEX_SPV=${CRYVR_SCENE_VERTEX_SPV}" "-DFRAGMENT_SPV=${CRYVR_SCENE_FRAGMENT_SPV}"
            "-DEXTRA_VERTEX_SPV=${CRYVR_SCENE_COLOR_VERTEX_SPV}" "-DEXTRA_FRAGMENT_SPV=${CRYVR_SCENE_COLOR_FRAGMENT_SPV}"
            "-DTHIRD_VERTEX_SPV=${CRYVR_SCENE_TEXTURE_VERTEX_SPV}" "-DTHIRD_FRAGMENT_SPV=${CRYVR_SCENE_TEXTURE_FRAGMENT_SPV}"
            "-DFOURTH_VERTEX_SPV=${CRYVR_SCENE_TEXTURE_COLOR_VERTEX_SPV}" "-DFOURTH_FRAGMENT_SPV=${CRYVR_SCENE_TEXTURE_COLOR_FRAGMENT_SPV}"
            "-DFIFTH_VERTEX_SPV=${CRYVR_SCENE_MULTITEXTURE_COLOR_VERTEX_SPV}" "-DFIFTH_FRAGMENT_SPV=${CRYVR_SCENE_MULTITEXTURE_COLOR_FRAGMENT_SPV}"
            "-DSIXTH_VERTEX_SPV=${CRYVR_SCENE_TEXTURE_LIT_VERTEX_SPV}"
            "-DSEVENTH_VERTEX_SPV=${CRYVR_SCENE_TEXTURE_LIT_COLOR_VERTEX_SPV}"
            "-DEIGHTH_VERTEX_SPV=${CRYVR_SCENE_TEXTURE_BUMP_VERTEX_SPV}"
            "-DNINTH_VERTEX_SPV=${CRYVR_SCENE_TEXTURE_BUMP_COLOR_VERTEX_SPV}"
            "-DSIXTH_FRAGMENT_SPV=${CRYVR_SCENE_TEXTURE_BUMP_FRAGMENT_SPV}"
            "-DOUTPUT_HEADER=${CRYVR_SCENE_SHADER_HEADER}" "-DVERTEX_ARRAY_NAME=kVrSceneVertexSpirv"
            "-DEXTRA_VERTEX_ARRAY_NAME=kVrSceneColorVertexSpirv" "-DEXTRA_FRAGMENT_ARRAY_NAME=kVrSceneColorFragmentSpirv"
            "-DTHIRD_VERTEX_ARRAY_NAME=kVrSceneTextureVertexSpirv" "-DTHIRD_FRAGMENT_ARRAY_NAME=kVrSceneTextureFragmentSpirv"
            "-DFOURTH_VERTEX_ARRAY_NAME=kVrSceneTextureColorVertexSpirv" "-DFOURTH_FRAGMENT_ARRAY_NAME=kVrSceneTextureColorFragmentSpirv"
            "-DFIFTH_VERTEX_ARRAY_NAME=kVrSceneMultiTextureColorVertexSpirv" "-DFIFTH_FRAGMENT_ARRAY_NAME=kVrSceneMultiTextureColorFragmentSpirv"
            "-DSIXTH_VERTEX_ARRAY_NAME=kVrSceneTextureLitVertexSpirv"
            "-DSEVENTH_VERTEX_ARRAY_NAME=kVrSceneTextureLitColorVertexSpirv"
            "-DEIGHTH_VERTEX_ARRAY_NAME=kVrSceneTextureBumpVertexSpirv"
            "-DNINTH_VERTEX_ARRAY_NAME=kVrSceneTextureBumpColorVertexSpirv"
            "-DSIXTH_FRAGMENT_ARRAY_NAME=kVrSceneTextureBumpFragmentSpirv"
            "-DFRAGMENT_ARRAY_NAME=kVrSceneFragmentSpirv" "-DHEADER_GUARD=CRY_VR_SCENE_SHADERS_H"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
    DEPENDS shaders/vr_scene.vert shaders/vr_scene.frag shaders/vr_scene_color.vert
            shaders/vr_scene_color.frag shaders/vr_scene_texture.vert shaders/vr_scene_texture.frag
            shaders/vr_scene_texture_color.vert shaders/vr_scene_texture_color.frag
            shaders/vr_scene_multitexture_color.vert shaders/vr_scene_multitexture_color.frag
            shaders/vr_scene_texture_lit.vert shaders/vr_scene_texture_lit_color.vert
            shaders/vr_scene_texture_bump.vert shaders/vr_scene_texture_bump_color.vert
            shaders/vr_scene_texture_color.frag
            cmake/EmbedSpirv.cmake VERBATIM)
add_custom_target(CryVRSceneShaders DEPENDS "${CRYVR_SCENE_SHADER_HEADER}")

set(CRYVR_LIT_VERTEX_SPV "${CRYVR_GENERATED_DIR}/vr_scene_lit.vert.spv")
set(CRYVR_LIT_COLOR_VERTEX_SPV "${CRYVR_GENERATED_DIR}/vr_scene_lit_color.vert.spv")
set(CRYVR_LIT_FRAGMENT_SPV "${CRYVR_GENERATED_DIR}/vr_scene_lit.frag.spv")
set(CRYVR_LIT_SHADER_HEADER "${CRYVR_GENERATED_DIR}/VulkanLitSceneShaders.h")
add_custom_command(
    OUTPUT "${CRYVR_LIT_SHADER_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CRYVR_GENERATED_DIR}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_lit.vert" -o "${CRYVR_LIT_VERTEX_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_lit_color.vert" -o "${CRYVR_LIT_COLOR_VERTEX_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_lit.frag" -o "${CRYVR_LIT_FRAGMENT_SPV}"
    COMMAND "${CMAKE_COMMAND}" "-DVERTEX_SPV=${CRYVR_LIT_VERTEX_SPV}" "-DFRAGMENT_SPV=${CRYVR_LIT_FRAGMENT_SPV}"
            "-DEXTRA_VERTEX_SPV=${CRYVR_LIT_COLOR_VERTEX_SPV}" "-DOUTPUT_HEADER=${CRYVR_LIT_SHADER_HEADER}"
            "-DVERTEX_ARRAY_NAME=kVrLitVertexSpirv" "-DEXTRA_VERTEX_ARRAY_NAME=kVrLitColorVertexSpirv"
            "-DFRAGMENT_ARRAY_NAME=kVrLitFragmentSpirv" "-DHEADER_GUARD=CRY_VR_LIT_SCENE_SHADERS_H"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
    DEPENDS shaders/vr_scene_lit.vert shaders/vr_scene_lit_color.vert shaders/vr_scene_lit.frag
            cmake/EmbedSpirv.cmake VERBATIM)
add_custom_target(CryVRLitSceneShaders DEPENDS "${CRYVR_LIT_SHADER_HEADER}")

add_library(${PROJECT_NAME} STATIC
    CryVR.cpp
    VulkanContext.cpp
    VulkanFrameRenderer.cpp
    VulkanGeometryBuffer.cpp
    VulkanPipelineFactory.cpp
    VulkanPipelineState.cpp
    VulkanResourceManager.cpp
    VulkanVertexFormat.cpp
    VulkanShaderLibrary.cpp
    VulkanFrameCapture.cpp
    "${CRYVR_PANEL_SHADER_HEADER}"
    "${CRYVR_SCENE_SHADER_HEADER}"
    "${CRYVR_LIT_SHADER_HEADER}"
)
add_dependencies(${PROJECT_NAME} CryVRPanelShaders CryVRSceneShaders CryVRLitSceneShaders)

set_target_properties(${PROJECT_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_11)

target_include_directories(${PROJECT_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    "${CRYVR_GENERATED_DIR}"
    ${CMAKE_SOURCE_DIR}/Externals/SDL/include
    ${CMAKE_SOURCE_DIR}/SourceCode/CryCommon
)

if (NOT ANDROID)
    find_package(Vulkan QUIET)
    if (Vulkan_INCLUDE_DIRS)
        target_include_directories(${PROJECT_NAME} PUBLIC ${Vulkan_INCLUDE_DIRS})
    elseif (Vulkan_INCLUDE_DIR)
        target_include_directories(${PROJECT_NAME} PUBLIC ${Vulkan_INCLUDE_DIR})
    else()
        find_path(CRY_VULKAN_INCLUDE_DIR vulkan/vulkan.h
            PATHS
                "$ENV{VULKAN_SDK}/Include"
                "C:/VulkanSDK/1.4.350.0/Include"
        )
        if (CRY_VULKAN_INCLUDE_DIR)
            target_include_directories(${PROJECT_NAME} PUBLIC ${CRY_VULKAN_INCLUDE_DIR})
        endif()
    endif()
endif()

target_link_libraries(${PROJECT_NAME} PUBLIC SDL3::SDL3)

if (ANDROID)
    target_compile_definitions(${PROJECT_NAME} PRIVATE CRYVR_ANDROID=1)
endif()
