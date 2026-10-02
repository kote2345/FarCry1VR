project(CryVR)

find_program(CRYVR_GLSLC glslc
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
if (NOT CRYVR_GLSLC)
    message(FATAL_ERROR "glslc is required to compile the Vulkan XR panel shaders")
endif()
set(CRYVR_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(CRYVR_PANEL_VERTEX_SPV "${CRYVR_GENERATED_DIR}/vr_panel.vert.spv")
set(CRYVR_PANEL_FRAGMENT_SPV "${CRYVR_GENERATED_DIR}/vr_panel.frag.spv")
set(CRYVR_PANEL_GEOMETRY_VERTEX_SPV "${CRYVR_GENERATED_DIR}/vr_panel_geometry.vert.spv")
set(CRYVR_PANEL_GEOMETRY_FRAGMENT_SPV "${CRYVR_GENERATED_DIR}/vr_panel_geometry.frag.spv")
set(CRYVR_RESOLVE_VERTEX_SPV "${CRYVR_GENERATED_DIR}/vr_resolve.vert.spv")
set(CRYVR_RESOLVE_FRAGMENT_SPV "${CRYVR_GENERATED_DIR}/vr_resolve.frag.spv")
set(CRYVR_PANEL_SHADER_HEADER "${CRYVR_GENERATED_DIR}/VulkanPanelShaders.h")
add_custom_command(
    OUTPUT "${CRYVR_PANEL_SHADER_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CRYVR_GENERATED_DIR}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_panel.vert" -o "${CRYVR_PANEL_VERTEX_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_panel.frag" -o "${CRYVR_PANEL_FRAGMENT_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_panel_geometry.vert" -o "${CRYVR_PANEL_GEOMETRY_VERTEX_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_panel_geometry.frag" -o "${CRYVR_PANEL_GEOMETRY_FRAGMENT_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_resolve.vert" -o "${CRYVR_RESOLVE_VERTEX_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_resolve.frag" -o "${CRYVR_RESOLVE_FRAGMENT_SPV}"
    COMMAND "${CMAKE_COMMAND}"
            "-DVERTEX_SPV=${CRYVR_PANEL_VERTEX_SPV}"
            "-DFRAGMENT_SPV=${CRYVR_PANEL_FRAGMENT_SPV}"
            "-DTHIRD_VERTEX_SPV=${CRYVR_PANEL_GEOMETRY_VERTEX_SPV}"
            "-DTHIRD_VERTEX_ARRAY_NAME=kVrPanelGeometryVertexSpirv"
            "-DTHIRD_FRAGMENT_SPV=${CRYVR_PANEL_GEOMETRY_FRAGMENT_SPV}"
            "-DTHIRD_FRAGMENT_ARRAY_NAME=kVrPanelGeometryFragmentSpirv"
            "-DFOURTH_VERTEX_SPV=${CRYVR_RESOLVE_VERTEX_SPV}"
            "-DFOURTH_VERTEX_ARRAY_NAME=kVrResolveVertexSpirv"
            "-DFOURTH_FRAGMENT_SPV=${CRYVR_RESOLVE_FRAGMENT_SPV}"
            "-DFOURTH_FRAGMENT_ARRAY_NAME=kVrResolveFragmentSpirv"
            "-DOUTPUT_HEADER=${CRYVR_PANEL_SHADER_HEADER}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
    DEPENDS shaders/vr_panel.vert shaders/vr_panel.frag
            shaders/vr_panel_geometry.vert shaders/vr_panel_geometry.frag
            shaders/vr_resolve.vert shaders/vr_resolve.frag cmake/EmbedSpirv.cmake
    VERBATIM)
add_custom_target(CryVRPanelShaders DEPENDS "${CRYVR_PANEL_SHADER_HEADER}")

set(CRYVR_SCENE_VERTEX_SPV "${CRYVR_GENERATED_DIR}/vr_scene.vert.spv")
set(CRYVR_SCENE_FRAGMENT_SPV "${CRYVR_GENERATED_DIR}/vr_scene.frag.spv")
set(CRYVR_SCENE_COLOR_VERTEX_SPV "${CRYVR_GENERATED_DIR}/vr_scene_color.vert.spv")
set(CRYVR_SCENE_COLOR_FRAGMENT_SPV "${CRYVR_GENERATED_DIR}/vr_scene_color.frag.spv")
set(CRYVR_SCENE_TEXTURE_VERTEX_SPV "${CRYVR_GENERATED_DIR}/vr_scene_texture.vert.spv")
set(CRYVR_SCENE_TEXTURE_FRAGMENT_SPV "${CRYVR_GENERATED_DIR}/vr_scene_texture.frag.spv")
set(CRYVR_SCENE_WATER_FRAGMENT_SPV "${CRYVR_GENERATED_DIR}/vr_scene_water.frag.spv")
set(CRYVR_SCENE_TEXTURE_COLOR_VERTEX_SPV "${CRYVR_GENERATED_DIR}/vr_scene_texture_color.vert.spv")
set(CRYVR_SCENE_TEXTURE_COLOR_FRAGMENT_SPV "${CRYVR_GENERATED_DIR}/vr_scene_texture_color.frag.spv")
set(CRYVR_SCENE_MULTITEXTURE_COLOR_VERTEX_SPV "${CRYVR_GENERATED_DIR}/vr_scene_multitexture_color.vert.spv")
set(CRYVR_SCENE_MULTITEXTURE_COLOR_FRAGMENT_SPV "${CRYVR_GENERATED_DIR}/vr_scene_multitexture_color.frag.spv")
set(CRYVR_SCENE_THREE_TEXTURE_COLOR_FRAGMENT_SPV "${CRYVR_GENERATED_DIR}/vr_scene_three_texture_color.frag.spv")
set(CRYVR_SCENE_FOUR_TEXTURE_COLOR_FRAGMENT_SPV "${CRYVR_GENERATED_DIR}/vr_scene_four_texture_color.frag.spv")
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
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_water.frag" -o "${CRYVR_SCENE_WATER_FRAGMENT_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_texture_color.vert" -o "${CRYVR_SCENE_TEXTURE_COLOR_VERTEX_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_texture_color.frag" -o "${CRYVR_SCENE_TEXTURE_COLOR_FRAGMENT_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_multitexture_color.vert" -o "${CRYVR_SCENE_MULTITEXTURE_COLOR_VERTEX_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_multitexture_color.frag" -o "${CRYVR_SCENE_MULTITEXTURE_COLOR_FRAGMENT_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_three_texture_color.frag" -o "${CRYVR_SCENE_THREE_TEXTURE_COLOR_FRAGMENT_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_four_texture_color.frag" -o "${CRYVR_SCENE_FOUR_TEXTURE_COLOR_FRAGMENT_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_texture_lit.vert" -o "${CRYVR_SCENE_TEXTURE_LIT_VERTEX_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_texture_lit_color.vert" -o "${CRYVR_SCENE_TEXTURE_LIT_COLOR_VERTEX_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_texture_bump.vert" -o "${CRYVR_SCENE_TEXTURE_BUMP_VERTEX_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_texture_bump_color.vert" -o "${CRYVR_SCENE_TEXTURE_BUMP_COLOR_VERTEX_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O -DVR_BUMP_MAP=1
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_texture_color.frag" -o "${CRYVR_SCENE_TEXTURE_BUMP_FRAGMENT_SPV}"
    COMMAND "${CMAKE_COMMAND}" "-DVERTEX_SPV=${CRYVR_SCENE_VERTEX_SPV}" "-DFRAGMENT_SPV=${CRYVR_SCENE_FRAGMENT_SPV}"
            "-DEXTRA_VERTEX_SPV=${CRYVR_SCENE_COLOR_VERTEX_SPV}" "-DEXTRA_FRAGMENT_SPV=${CRYVR_SCENE_COLOR_FRAGMENT_SPV}"
            "-DTHIRD_VERTEX_SPV=${CRYVR_SCENE_TEXTURE_VERTEX_SPV}" "-DTHIRD_FRAGMENT_SPV=${CRYVR_SCENE_TEXTURE_FRAGMENT_SPV}"
            "-DTWELFTH_FRAGMENT_SPV=${CRYVR_SCENE_WATER_FRAGMENT_SPV}"
            "-DTWELFTH_FRAGMENT_ARRAY_NAME=kVrSceneWaterFragmentSpirv"
            "-DFOURTH_VERTEX_SPV=${CRYVR_SCENE_TEXTURE_COLOR_VERTEX_SPV}" "-DFOURTH_FRAGMENT_SPV=${CRYVR_SCENE_TEXTURE_COLOR_FRAGMENT_SPV}"
            "-DFIFTH_VERTEX_SPV=${CRYVR_SCENE_MULTITEXTURE_COLOR_VERTEX_SPV}" "-DFIFTH_FRAGMENT_SPV=${CRYVR_SCENE_MULTITEXTURE_COLOR_FRAGMENT_SPV}"
            "-DTENTH_FRAGMENT_SPV=${CRYVR_SCENE_THREE_TEXTURE_COLOR_FRAGMENT_SPV}"
            "-DELEVENTH_FRAGMENT_SPV=${CRYVR_SCENE_FOUR_TEXTURE_COLOR_FRAGMENT_SPV}"
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
            "-DTENTH_FRAGMENT_ARRAY_NAME=kVrSceneThreeTextureColorFragmentSpirv"
            "-DELEVENTH_FRAGMENT_ARRAY_NAME=kVrSceneFourTextureColorFragmentSpirv"
            "-DSIXTH_VERTEX_ARRAY_NAME=kVrSceneTextureLitVertexSpirv"
            "-DSEVENTH_VERTEX_ARRAY_NAME=kVrSceneTextureLitColorVertexSpirv"
            "-DEIGHTH_VERTEX_ARRAY_NAME=kVrSceneTextureBumpVertexSpirv"
            "-DNINTH_VERTEX_ARRAY_NAME=kVrSceneTextureBumpColorVertexSpirv"
            "-DSIXTH_FRAGMENT_ARRAY_NAME=kVrSceneTextureBumpFragmentSpirv"
            "-DFRAGMENT_ARRAY_NAME=kVrSceneFragmentSpirv" "-DHEADER_GUARD=CRY_VR_SCENE_SHADERS_H"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
    DEPENDS shaders/scene_stereo.glsl shaders/scene_lighting.glsl shaders/scene_alpha_test.glsl shaders/scene_vertex_lighting.glsl
            shaders/vr_scene.vert shaders/vr_scene.frag shaders/vr_scene_color.vert
            shaders/vr_scene_color.frag shaders/vr_scene_texture.vert shaders/vr_scene_texture.frag
            shaders/vr_scene_water.frag
            shaders/vr_scene_texture_color.vert shaders/vr_scene_texture_color.frag
            shaders/vr_scene_multitexture_color.vert shaders/vr_scene_multitexture_color.frag
            shaders/vr_scene_three_texture_color.frag
            shaders/vr_scene_four_texture_color.frag
            shaders/vr_scene_texture_lit.vert shaders/vr_scene_texture_lit_color.vert
            shaders/vr_scene_texture_bump.vert shaders/vr_scene_texture_bump_color.vert
            shaders/vr_scene_texture_color.frag
            cmake/EmbedSpirv.cmake VERBATIM)
add_custom_target(CryVRSceneShaders DEPENDS "${CRYVR_SCENE_SHADER_HEADER}")

# Linear texgen must also work for position formats without a mesh UV stream.
set(CRYVR_GENERATED_TEXGEN_SPVS "")
set(CRYVR_GENERATED_EXTERNAL_UV_SPVS "")
set(CRYVR_GENERATED_EXTERNAL_UV_NAMES "")
foreach(_generated_variant RANGE 0 23)
    set(_generated_defines "")
    math(EXPR _generated_layout_variant "${_generated_variant} % 12")
    if (_generated_variant GREATER_EQUAL 12)
        list(APPEND _generated_defines -DVR_GENERATED_EXTERNAL_UV=1)
    endif()
    if (_generated_layout_variant GREATER_EQUAL 8)
        list(APPEND _generated_defines -DVR_GENERATED_SECONDARY=1 -DVR_GENERATED_COLOR=1)
        math(EXPR _generated_base_variant "(${_generated_variant} % 2) * 2")
    else()
        math(EXPR _generated_base_variant "${_generated_variant} % 4")
    endif()
    if (_generated_base_variant GREATER_EQUAL 2)
        list(APPEND _generated_defines -DVR_GENERATED_NORMAL=1)
    endif()
    if ((_generated_layout_variant GREATER_EQUAL 4 AND _generated_layout_variant LESS 8) OR _generated_layout_variant GREATER_EQUAL 10)
        list(APPEND _generated_defines -DVR_GENERATED_MULTI=1)
    endif()
    math(EXPR _generated_has_color "${_generated_variant} % 2")
    if (_generated_has_color)
        list(APPEND _generated_defines -DVR_GENERATED_COLOR=1)
    endif()
    set(_generated_spv "${CRYVR_GENERATED_DIR}/vr_scene_generated_texture_${_generated_variant}.vert.spv")
    add_custom_command(OUTPUT "${_generated_spv}"
        COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O ${_generated_defines}
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_generated_texture.vert" -o "${_generated_spv}"
        DEPENDS shaders/scene_stereo.glsl shaders/vr_scene_generated_texture.vert shaders/scene_vertex_lighting.glsl VERBATIM)
    list(APPEND CRYVR_GENERATED_TEXGEN_SPVS "${_generated_spv}")
    if (_generated_variant GREATER_EQUAL 12)
        list(APPEND CRYVR_GENERATED_EXTERNAL_UV_SPVS "${_generated_spv}")
        list(APPEND CRYVR_GENERATED_EXTERNAL_UV_NAMES "kVrGeneratedExternalUvVertex${_generated_variant}Spirv")
    endif()
endforeach()
set(CRYVR_GENERATED_TEXGEN_HEADER "${CRYVR_GENERATED_DIR}/VulkanGeneratedTexgenShaders.h")
add_custom_command(OUTPUT "${CRYVR_GENERATED_TEXGEN_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
        "-DVERTEX_SPV=${CRYVR_GENERATED_DIR}/vr_scene_generated_texture_0.vert.spv"
        "-DEXTRA_VERTEX_SPV=${CRYVR_GENERATED_DIR}/vr_scene_generated_texture_1.vert.spv"
        "-DTHIRD_VERTEX_SPV=${CRYVR_GENERATED_DIR}/vr_scene_generated_texture_2.vert.spv"
        "-DFOURTH_VERTEX_SPV=${CRYVR_GENERATED_DIR}/vr_scene_generated_texture_3.vert.spv"
        "-DFIFTH_VERTEX_SPV=${CRYVR_GENERATED_DIR}/vr_scene_generated_texture_4.vert.spv"
        "-DSIXTH_VERTEX_SPV=${CRYVR_GENERATED_DIR}/vr_scene_generated_texture_5.vert.spv"
        "-DSEVENTH_VERTEX_SPV=${CRYVR_GENERATED_DIR}/vr_scene_generated_texture_6.vert.spv"
        "-DEIGHTH_VERTEX_SPV=${CRYVR_GENERATED_DIR}/vr_scene_generated_texture_7.vert.spv"
        "-DNINTH_VERTEX_SPV=${CRYVR_GENERATED_DIR}/vr_scene_generated_texture_8.vert.spv"
        "-DTENTH_VERTEX_SPV=${CRYVR_GENERATED_DIR}/vr_scene_generated_texture_9.vert.spv"
        "-DELEVENTH_VERTEX_SPV=${CRYVR_GENERATED_DIR}/vr_scene_generated_texture_10.vert.spv"
        "-DTWELFTH_VERTEX_SPV=${CRYVR_GENERATED_DIR}/vr_scene_generated_texture_11.vert.spv"
        "-DVERTEX_ARRAY_NAME=kVrGeneratedTextureVertexSpirv"
        "-DEXTRA_VERTEX_ARRAY_NAME=kVrGeneratedTextureColorVertexSpirv"
        "-DTHIRD_VERTEX_ARRAY_NAME=kVrGeneratedTextureNormalVertexSpirv"
        "-DFOURTH_VERTEX_ARRAY_NAME=kVrGeneratedTextureNormalColorVertexSpirv"
        "-DFIFTH_VERTEX_ARRAY_NAME=kVrGeneratedMultiTextureVertexSpirv"
        "-DSIXTH_VERTEX_ARRAY_NAME=kVrGeneratedMultiTextureColorVertexSpirv"
        "-DSEVENTH_VERTEX_ARRAY_NAME=kVrGeneratedMultiTextureNormalVertexSpirv"
        "-DEIGHTH_VERTEX_ARRAY_NAME=kVrGeneratedMultiTextureNormalColorVertexSpirv"
        "-DNINTH_VERTEX_ARRAY_NAME=kVrGeneratedSecondaryVertexSpirv"
        "-DTENTH_VERTEX_ARRAY_NAME=kVrGeneratedSecondaryNormalVertexSpirv"
        "-DELEVENTH_VERTEX_ARRAY_NAME=kVrGeneratedMultiSecondaryVertexSpirv"
        "-DTWELFTH_VERTEX_ARRAY_NAME=kVrGeneratedMultiSecondaryNormalVertexSpirv"
        "-DFRAGMENT_SPV=${CRYVR_SCENE_TEXTURE_FRAGMENT_SPV}"
        "-DFRAGMENT_ARRAY_NAME=kVrGeneratedTextureFragmentSpirv"
        "-DOUTPUT_HEADER=${CRYVR_GENERATED_TEXGEN_HEADER}"
        "-DADDITIONAL_VERTEX_SPVS=${CRYVR_GENERATED_EXTERNAL_UV_SPVS}"
        "-DADDITIONAL_VERTEX_NAMES=${CRYVR_GENERATED_EXTERNAL_UV_NAMES}"
        -DHEADER_GUARD=CRY_VR_GENERATED_TEXGEN_SHADERS_H
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
    DEPENDS ${CRYVR_GENERATED_TEXGEN_SPVS} "${CRYVR_SCENE_SHADER_HEADER}" cmake/EmbedSpirv.cmake VERBATIM)
add_custom_target(CryVRGeneratedTexgenShaders ALL DEPENDS "${CRYVR_GENERATED_TEXGEN_HEADER}")

# Multitexture stages previously bypassed the normal-lighting vertex path and
# read vertex-color attributes even for formats that do not provide them.
set(CRYVR_MULTITEXTURE_LIT_COLOR_SPV "${CRYVR_GENERATED_DIR}/vr_scene_multitexture_lit.vert.spv")
set(CRYVR_MULTITEXTURE_LIT_NOCOLOR_SPV "${CRYVR_GENERATED_DIR}/vr_scene_multitexture_lit_nocolor.vert.spv")
set(CRYVR_MULTITEXTURE_NOCOLOR_SPV "${CRYVR_GENERATED_DIR}/vr_scene_multitexture_nocolor.vert.spv")
set(CRYVR_MULTITEXTURE_LIT_SHADER_HEADER "${CRYVR_GENERATED_DIR}/VulkanMultiTextureLitShaders.h")
add_custom_command(
    OUTPUT "${CRYVR_MULTITEXTURE_LIT_SHADER_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CRYVR_GENERATED_DIR}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_multitexture_lit.vert"
            -o "${CRYVR_MULTITEXTURE_LIT_COLOR_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_multitexture_lit_nocolor.vert"
            -o "${CRYVR_MULTITEXTURE_LIT_NOCOLOR_SPV}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_multitexture_nocolor.vert"
            -o "${CRYVR_MULTITEXTURE_NOCOLOR_SPV}"
    COMMAND "${CMAKE_COMMAND}"
            "-DVERTEX_SPV=${CRYVR_MULTITEXTURE_LIT_COLOR_SPV}"
            "-DFRAGMENT_SPV=${CRYVR_SCENE_MULTITEXTURE_COLOR_FRAGMENT_SPV}"
            "-DEXTRA_VERTEX_SPV=${CRYVR_MULTITEXTURE_LIT_NOCOLOR_SPV}"
            "-DTHIRD_VERTEX_SPV=${CRYVR_MULTITEXTURE_NOCOLOR_SPV}"
            "-DOUTPUT_HEADER=${CRYVR_MULTITEXTURE_LIT_SHADER_HEADER}"
            "-DVERTEX_ARRAY_NAME=kVrMultiTextureLitColorVertexSpirv"
            "-DFRAGMENT_ARRAY_NAME=kVrMultiTextureUnusedFragmentSpirv"
            "-DEXTRA_VERTEX_ARRAY_NAME=kVrMultiTextureLitNoColorVertexSpirv"
            "-DTHIRD_VERTEX_ARRAY_NAME=kVrMultiTextureNoColorVertexSpirv"
            "-DHEADER_GUARD=CRY_VR_MULTITEXTURE_LIT_SHADERS_H"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
    DEPENDS shaders/scene_vertex_lighting.glsl shaders/scene_stereo.glsl
            shaders/vr_scene_multitexture_lit.vert
            shaders/vr_scene_multitexture_lit_nocolor.vert
            shaders/vr_scene_multitexture_nocolor.vert
            cmake/EmbedSpirv.cmake VERBATIM)
add_custom_target(CryVRMultiTextureLitShaders DEPENDS "${CRYVR_MULTITEXTURE_LIT_SHADER_HEADER}")
add_dependencies(CryVRMultiTextureLitShaders CryVRSceneShaders)

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
    DEPENDS shaders/scene_stereo.glsl shaders/scene_lighting.glsl shaders/scene_alpha_test.glsl shaders/scene_vertex_lighting.glsl
            shaders/vr_scene_lit.vert shaders/vr_scene_lit_color.vert shaders/vr_scene_lit.frag
            cmake/EmbedSpirv.cmake VERBATIM)
add_custom_target(CryVRLitSceneShaders DEPENDS "${CRYVR_LIT_SHADER_HEADER}")

set(CRYVR_WATER_SHADER_HEADER "${CRYVR_GENERATED_DIR}/VulkanWaterShaders.h")
add_custom_command(
    OUTPUT "${CRYVR_WATER_SHADER_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CRYVR_GENERATED_DIR}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_water.vert" -o "${CRYVR_GENERATED_DIR}/water.vert.spv"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O -DWATER_COLOR=1 "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_water.vert" -o "${CRYVR_GENERATED_DIR}/water_color.vert.spv"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O -DWATER_COLOR=1 -DWATER_OCEAN=1 "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_water.vert" -o "${CRYVR_GENERATED_DIR}/ocean.vert.spv"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O -DWATER_COLOR=1 -DWATER_UV=1 "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_water.vert" -o "${CRYVR_GENERATED_DIR}/beach.vert.spv"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O -DWATER_SEA=1 "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_scene_water.frag" -o "${CRYVR_GENERATED_DIR}/sea.frag.spv"
    COMMAND "${CMAKE_COMMAND}"
        "-DVERTEX_SPV=${CRYVR_GENERATED_DIR}/water.vert.spv"
        "-DFRAGMENT_SPV=${CRYVR_SCENE_WATER_FRAGMENT_SPV}"
        "-DEXTRA_VERTEX_SPV=${CRYVR_GENERATED_DIR}/water_color.vert.spv"
        "-DTHIRD_VERTEX_SPV=${CRYVR_GENERATED_DIR}/ocean.vert.spv"
        "-DFOURTH_VERTEX_SPV=${CRYVR_GENERATED_DIR}/beach.vert.spv"
        "-DFOURTH_VERTEX_ARRAY_NAME=kVrBeachVertexSpirv"
        "-DEXTRA_FRAGMENT_SPV=${CRYVR_GENERATED_DIR}/sea.frag.spv"
        "-DEXTRA_FRAGMENT_ARRAY_NAME=kVrSeaFragmentSpirv"
        "-DVERTEX_ARRAY_NAME=kVrWaterVertexSpirv"
        "-DFRAGMENT_ARRAY_NAME=kVrWaterUnusedFragmentSpirv"
        "-DEXTRA_VERTEX_ARRAY_NAME=kVrWaterColorVertexSpirv"
        "-DTHIRD_VERTEX_ARRAY_NAME=kVrOceanVertexSpirv"
        "-DHEADER_GUARD=CRY_VR_WATER_SHADERS_H"
        "-DOUTPUT_HEADER=${CRYVR_WATER_SHADER_HEADER}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
    DEPENDS shaders/scene_stereo.glsl shaders/vr_water.vert shaders/scene_water.glsl shaders/scene_alpha_test.glsl "${CRYVR_SCENE_SHADER_HEADER}" cmake/EmbedSpirv.cmake VERBATIM)
add_custom_target(CryVRWaterShaders DEPENDS "${CRYVR_WATER_SHADER_HEADER}")

set(CRYVR_STOCK_PROGRAM_HEADER "${CRYVR_GENERATED_DIR}/VulkanStockPrograms.h")
add_custom_command(
    OUTPUT "${CRYVR_STOCK_PROGRAM_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CRYVR_GENERATED_DIR}"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_terrain_layer.vert" -o "${CRYVR_GENERATED_DIR}/terrain_layer.vert.spv"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_terrain_layer.frag" -o "${CRYVR_GENERATED_DIR}/terrain_layer.frag.spv"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_caustics.vert" -o "${CRYVR_GENERATED_DIR}/caustics.vert.spv"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_caustics.frag" -o "${CRYVR_GENERATED_DIR}/caustics.frag.spv"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_plants.vert" -o "${CRYVR_GENERATED_DIR}/plants.vert.spv"
    COMMAND "${CRYVR_GLSLC}" --target-env=vulkan1.0 -O
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vr_plants.frag" -o "${CRYVR_GENERATED_DIR}/plants.frag.spv"
    COMMAND "${CMAKE_COMMAND}"
        "-DVERTEX_SPV=${CRYVR_GENERATED_DIR}/terrain_layer.vert.spv"
        "-DFRAGMENT_SPV=${CRYVR_GENERATED_DIR}/terrain_layer.frag.spv"
        "-DVERTEX_ARRAY_NAME=kVrTerrainLayerVertexSpirv"
        "-DFRAGMENT_ARRAY_NAME=kVrTerrainLayerFragmentSpirv"
        "-DEXTRA_VERTEX_SPV=${CRYVR_GENERATED_DIR}/caustics.vert.spv"
        "-DEXTRA_FRAGMENT_SPV=${CRYVR_GENERATED_DIR}/caustics.frag.spv"
        "-DEXTRA_VERTEX_ARRAY_NAME=kVrCausticsVertexSpirv"
        "-DEXTRA_FRAGMENT_ARRAY_NAME=kVrCausticsFragmentSpirv"
        "-DTHIRD_VERTEX_SPV=${CRYVR_GENERATED_DIR}/plants.vert.spv"
        "-DTHIRD_FRAGMENT_SPV=${CRYVR_GENERATED_DIR}/plants.frag.spv"
        "-DTHIRD_VERTEX_ARRAY_NAME=kVrPlantsVertexSpirv"
        "-DTHIRD_FRAGMENT_ARRAY_NAME=kVrPlantsFragmentSpirv"
        "-DHEADER_GUARD=CRY_VR_STOCK_PROGRAMS_H"
        "-DOUTPUT_HEADER=${CRYVR_STOCK_PROGRAM_HEADER}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
    DEPENDS shaders/vr_terrain_layer.vert shaders/vr_terrain_layer.frag
        shaders/vr_plants.vert shaders/vr_plants.frag shaders/scene_plants_uniforms.glsl
        shaders/vr_caustics.vert shaders/vr_caustics.frag
        shaders/scene_stereo.glsl shaders/scene_water.glsl shaders/scene_alpha_test.glsl cmake/EmbedSpirv.cmake VERBATIM)
add_custom_target(CryVRStockPrograms DEPENDS "${CRYVR_STOCK_PROGRAM_HEADER}")

add_library(${PROJECT_NAME} STATIC
    CryVR.cpp
    OpenXRPlatform.cpp
    VulkanContext.cpp
    VulkanFrameRenderer.cpp
    VulkanGeometryBuffer.cpp
    VulkanPipelineFactory.cpp
    VulkanPipelineState.cpp
    VulkanResourceManager.cpp
    VulkanVertexFormat.cpp
    VulkanShaderLibrary.cpp
    VulkanFrameCapture.cpp
    "${CRYVR_STOCK_PROGRAM_HEADER}"
    "${CRYVR_PANEL_SHADER_HEADER}"
    "${CRYVR_SCENE_SHADER_HEADER}"
    "${CRYVR_GENERATED_TEXGEN_HEADER}"
    "${CRYVR_LIT_SHADER_HEADER}"
)
add_dependencies(${PROJECT_NAME} CryVRPanelShaders CryVRSceneShaders CryVRLitSceneShaders CryVRMultiTextureLitShaders CryVRWaterShaders CryVRStockPrograms)

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

if (ANDROID)
    target_compile_definitions(${PROJECT_NAME} PRIVATE CRYVR_ANDROID=1)
    target_link_libraries(${PROJECT_NAME} PUBLIC SDL3::SDL3)
elseif (UNIX)
    target_link_libraries(${PROJECT_NAME} PUBLIC ${CMAKE_DL_LIBS})
endif()
