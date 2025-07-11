#ifndef RLIGHTS_H
#define RLIGHTS_H

#include <raylib.h>

/*
 * TODO: Implement `RLG_MAX_LIGHTS`, which would be a separate array containing all user-defined lights.
 *       Then, during the rendering of a mesh, we will perform an AABB test to determine which light illuminates which mesh,
 *       with a limit per shader defined by `RLG_MAX_LIGHTS_PER_MATERIAL`.
 *
 *       However, this would require a separate RLG_Model and/or RLG_Mesh struct...
 *
 *       With this, we would need to create a system where we can create multiple materials, each with its own specifications
 *       defined by flags like `RLG_USE_NORMAL_MAP`, `RLG_RECEIVE_SHADOW`, etc. This would avoid all the branching
 *       in the shaders since everything would be decided at compilation. This would replace the need for creating different contexts.
 *
 *       But again, this would probably require us to create our own RLG_Material...
 */


/* Config Defintions */

//#ifndef RLG_MAX_LIGHTS
//#   define RLG_MAX_LIGHTS                32   // Indicates the total number of lights manageable by a context
//#endif

#ifndef RLG_MAX_LIGHTS_PER_MATERIAL
#   define RLG_MAX_LIGHTS_PER_MATERIAL     8    // Indicates the total number of lights that can illuminate a mesh
#endif

/* Definitions for managing OpenGL */

#ifndef GL_HEADER
#   if defined(GRAPHICS_API_OPENGL_ES2)
#       define GL_HEADER "external/glad_gles2.h"    // Required for: OpenGL functionality
#   else
#       if defined(__APPLE__)
#           define GL_SILENCE_DEPRECATION           // Silence Opengl API deprecation warnings 
#           define GL_HEADER <OpenGL/gl3.h>         // OpenGL 3 library for OSX
#           define GL_EXT_HEADER <OpenGL/gl3ext.h>  // OpenGL 3 extensions library for OSX
#       else
#           define GL_HEADER "external/glad.h"      // Required for: OpenGL functionality 
#       endif
#   endif
#endif

#define _RLIGHTS_RL55_MIPMAP_COUNT 0

#include GL_HEADER

#ifdef GL_EXT_HEADER
#   include GL_EXT_HEADER
#endif

#ifndef GLSL_VERSION
#   ifdef PLATFORM_DESKTOP
#       define GLSL_VERSION 330
#   else
#       define GLSL_VERSION 100
#   endif //PLATFORM
#endif //GLSL_VERSION

/* Enum/Function Definitions */

/**
 * @brief Enum representing different types of lights.
 */
typedef enum {
    RLG_DIRLIGHT = 0,                       ///< Enum representing a directional light type.
    RLG_OMNILIGHT,                          ///< Enum representing an omnilight type.
    RLG_SPOTLIGHT                           ///< Enum representing a spotlight type.
} RLG_LightType;

/**
 * @brief Enum representing different types of shaders.
 */
typedef enum {
    RLG_SHADER_MODEL = 0,                   ///< Enum representing the main lighting shader.
    RLG_SHADER_DEPTH,                       ///< Enum representing the depth writing shader for shadow maps.
    RLG_SHADER_DEPTH_CUBEMAP,               ///< Enum representing the depth writing shader for shadow cubemaps.
    RLG_SHADER_EQUIRECTANGULAR_TO_CUBEMAP,  ///< Enum representing the shader for generating skyboxes from HDR textures.
    RLG_SHADER_IRRADIANCE_CONVOLUTION,      ///< Enum representing the shader for generating irradiance maps from skyboxes.
    RLG_SHADER_SKYBOX                       ///< Enum representing the shader for rendering skyboxes.
} RLG_Shader;

/**
 * @brief Enum representing different properties of a light.
 */
typedef enum {
    RLG_LIGHT_POSITION = 0,                 ///< Position of the light.
    RLG_LIGHT_DIRECTION,                    ///< Direction of the light.
    RLG_LIGHT_COLOR,                        ///< Diffuse color of the light.
    RLG_LIGHT_ENERGY,                       ///< Energy factor of the light.
    RLG_LIGHT_SPECULAR,                     ///< Specular tint color of the light.
    RLG_LIGHT_SIZE,                         ///< Light size, affects fade and shadow blur (spotlight, omnilight only).
    RLG_LIGHT_INNER_CUTOFF,                 ///< Inner cutoff angle of a spotlight.
    RLG_LIGHT_OUTER_CUTOFF,                 ///< Outer cutoff angle of a spotlight.
    RLG_LIGHT_DISTANCE,                     ///< Max distance up to which the spotlights and omnilights shine.
    RLG_LIGHT_ATTENUATION,                  ///< Light attenuation factor along the illumination distance of spotlights and omnilights.
} RLG_LightProperty;

/**
 * @brief Enum representing all shader locations used by rlights.
 */
typedef enum {

    /* Same as raylib */

    RLG_LOC_VERTEX_POSITION = 0,
    RLG_LOC_VERTEX_TEXCOORD01,
    RLG_LOC_VERTEX_TEXCOORD02,
    RLG_LOC_VERTEX_NORMAL,
    RLG_LOC_VERTEX_TANGENT,
    RLG_LOC_VERTEX_COLOR,
    RLG_LOC_MATRIX_MVP,
    RLG_LOC_MATRIX_VIEW,
    RLG_LOC_MATRIX_PROJECTION,
    RLG_LOC_MATRIX_MODEL,
    RLG_LOC_MATRIX_NORMAL,
    RLG_LOC_VECTOR_VIEW,
    RLG_LOC_COLOR_DIFFUSE,
    RLG_LOC_COLOR_SPECULAR,
    RLG_LOC_COLOR_AMBIENT,
    RLG_LOC_MAP_ALBEDO,
    RLG_LOC_MAP_METALNESS,
    RLG_LOC_MAP_NORMAL,
    RLG_LOC_MAP_ROUGHNESS,
    RLG_LOC_MAP_OCCLUSION,
    RLG_LOC_MAP_EMISSION,
    RLG_LOC_MAP_HEIGHT,
    RLG_LOC_MAP_CUBEMAP,
    RLG_LOC_MAP_IRRADIANCE,
    RLG_LOC_MAP_PREFILTER,
    RLG_LOC_MAP_BRDF,

    /* Specific to rlights.h */

    RLG_LOC_COLOR_EMISSION,
    RLG_LOC_METALNESS_SCALE,
    RLG_LOC_ROUGHNESS_SCALE,
    RLG_LOC_AO_LIGHT_AFFECT,
    RLG_LOC_HEIGHT_SCALE,

    /* Internal use */

    RLG_COUNT_LOCS

} RLG_ShaderLocationIndex;

/**
 * @brief Structure representing a skybox with associated textures and buffers.
 *
 * This structure contains the textures and buffer IDs necessary for rendering
 * a skybox. It includes the cubemap texture, the irradiance texture, vertex
 * buffer object (VBO) IDs, vertex array object (VAO) ID, and a flag indicating
 * whether the skybox is in high dynamic range (HDR).
 */
typedef struct {
    TextureCubemap cubemap;       ///< The cubemap texture representing the skybox.
    TextureCubemap irradiance;    ///< The irradiance cubemap texture for diffuse lighting.
    bool isHDR;                   ///< Flag indicating if the skybox is HDR (high dynamic range).
} RLG_Skybox;

/**
 * @brief Opaque type for a lighting context handle.
 * 
 * This type represents a handle to a lighting context,
 * which is used to manage and update lighting-related data.
 */
typedef void* RLG_Context;

/**
 * @brief Type definition for a rendering function.
 * 
 * This function type is used for rendering purposes,
 * particularly for updating the shadow map in the RLG_UpdateShadowMap function.
 * 
 * @param shader The shader to use for rendering.
 */
typedef void (*RLG_DrawFunc)(Shader);


#if defined(__cplusplus)
extern "C" {
#endif

/**
 * @brief Create a new rlights management context.
 *
 * @return A new RLG_Context object representing the created lighting context.
 */
RLG_Context RLG_CreateContext(void);

/**
 * @brief Destroy a previously created lighting context and release associated resources.
 * 
 * @param ctx The lighting context to destroy.
 */
void RLG_DestroyContext(RLG_Context ctx);

/**
 * @brief Set the active lighting context for rlights.
 * 
 * @param ctx The lighting context to set as active.
 */
void RLG_SetContext(RLG_Context ctx);

/**
 * @brief Get the currently active lighting context for rlights.
 * 
 * @return The current RLG_Context object representing the active lighting context.
 */
RLG_Context RLG_GetContext(void);

/**
 * @brief Set custom shader code for a specific shader type.
 * 
 * @note This function should be called before RLG_Init to define your own shaders.
 * 
 * @param shader The type of shader to set the custom code for.
 * @param vsCode Vertex shader code for the specified shader type.
 * @param fsCode Fragment shader code for the specified shader type.
 */
void RLG_SetCustomShaderCode(RLG_Shader shader, const char *vsCode, const char *fsCode);

/**
 * @brief Get the current shader of the specified type.
 * 
 * @param shader The type of shader to retrieve.
 * 
 * @return A pointer to the current Shader object used for the specified shader type.
 *         Returns NULL if the shader type is not loaded.
 */
const Shader* RLG_GetShader(RLG_Shader shader);

/**
 * @brief Set the view position, corresponds to the position of your camera.
 * 
 * @param x The x-coordinate of the view position.
 * @param y The y-coordinate of the view position.
 * @param z The z-coordinate of the view position.
 */
void RLG_SetViewPosition(float x, float y, float z);

/**
 * @brief Set the view position using a Vector3 structure.
 * 
 * @param position The view position as a Vector3 structure.
 */
void RLG_SetViewPositionV(Vector3 position);

/**
 * @brief Get the current view position.
 * 
 * @return The current view position as a Vector3 structure.
 */
Vector3 RLG_GetViewPosition(void);

/**
 * @brief Set the ambient color.
 * 
 * @param color The ambient color as a Color structure.
 */
void RLG_SetAmbientColor(Color color);

/**
 * @brief Get the current ambient color.
 * 
 * @return The current ambient color as a Color structure.
 */
Color RLG_GetAmbientColor(void);

/**
 * @brief Set the minimum and maximum layers for parallax mapping.
 * 
 * @param min The minimum layer index.
 * @param max The maximum layer index.
 */
void RLG_SetParallaxLayers(int min, int max);

/**
 * @brief Get the current minimum and maximum layers for parallax mapping.
 * 
 * @param min Pointer to store the minimum layer index.
 * @param max Pointer to store the maximum layer index.
 */
void RLG_GetParallaxLayers(int* min, int* max);

/**
 * @brief Activate or deactivate texture sampling in the materials of models.
 * 
 * @param mapIndex The material map to modify.
 * @param active Boolean value indicating whether to activate (true) or deactivate (false) texture sampling.
 */
void RLG_UseMap(MaterialMapIndex mapIndex, bool active);

/**
 * @brief Check if texture sampling is enabled for a given material map.
 * 
 * @param mapIndex The material map to check.
 * @return True if texture sampling is enabled, false otherwise.
 */
bool RLG_IsMapUsed(MaterialMapIndex mapIndex);

/**
 * @brief Use the default material map if true, otherwise use the material maps defined in the models.
 * 
 * @param mapIndex The material map to modify.
 * @param active Boolean value indicating whether to use the default material map (true) or the material maps defined in the models (false).
 */
void RLG_UseDefaultMap(MaterialMapIndex mapIndex, bool active);

/**
 * @brief Set the default material map for a given material map index.
 * 
 * @param mapIndex The material map index to set.
 * @param map The material map to set as default.
 */
void RLG_SetDefaultMap(MaterialMapIndex mapIndex, MaterialMap map);

/**
 * @brief Get the default material map for a given material map index.
 * 
 * @param mapIndex The material map index to retrieve.
 * @return The default material map.
 */
MaterialMap RLG_GetDefaultMap(MaterialMapIndex mapIndex);

/**
 * @brief Check if the default material map is used for a given material map index.
 * 
 * @param mapIndex The material map index to check.
 * @return True if the default material map is used, false otherwise.
 */
bool RLG_IsDefaultMapUsed(MaterialMapIndex mapIndex);

/**
 * @brief Activate or deactivate a specific light.
 * 
 * @param light The index of the light to modify.
 * @param active Boolean value indicating whether to activate (true) or deactivate (false) the light.
 */
void RLG_UseLight(unsigned int light, bool active);

/**
 * @brief Check if a specific light is enabled.
 * 
 * @param light The index of the light to check.
 * @return true if the light is enabled, false otherwise.
 */
bool RLG_IsLightUsed(unsigned int light);

/**
 * @brief Toggle the state of a specific light.
 * 
 * @param light The index of the light to toggle.
 */
void RLG_ToggleLight(unsigned int light);

/**
 * @brief Set the type of a specific light.
 * 
 * @param light The index of the light to set the type for.
 * @param type The type of light to set.
 */
void RLG_SetLightType(unsigned int light, RLG_LightType type);

/**
 * @brief Get the type of a specific light.
 * 
 * @param light The index of the light to get the type for.
 * @return The type of light as RLG_LightType enumeration.
 */
RLG_LightType RLG_GetLightType(unsigned int light);

/**
 * @brief Set a float value for a specific light property.
 * 
 * @param light The index of the light to modify.
 * @param property The light property to set the value for.
 * @param value The float value to assign to the light property.
 */
void RLG_SetLightValue(unsigned int light, RLG_LightProperty property, float value);

/**
 * @brief Set XYZ coordinates for a specific light property.
 * 
 * @param light The index of the light to modify.
 * @param property The light property to set the coordinates for.
 * @param x The X coordinate value.
 * @param y The Y coordinate value.
 * @param z The Z coordinate value.
 */
void RLG_SetLightXYZ(unsigned int light, RLG_LightProperty property, float x, float y, float z);

/**
 * @brief Set a Vector3 value for a specific light property.
 * 
 * @param light The index of the light to modify.
 * @param property The light property to set the Vector3 value for.
 * @param value The Vector3 value to assign to the light property.
 */
void RLG_SetLightVec3(unsigned int light, RLG_LightProperty property, Vector3 value);

/**
 * @brief Set a color value for a specific light.
 * 
 * @param light The index of the light to modify.
 * @param color The color to assign to the light.
 */
void RLG_SetLightColor(unsigned int light, Color color);

/**
 * @brief Get the float value of a specific light property.
 * 
 * @param light The index of the light to retrieve the value from.
 * @param property The light property to retrieve the value for.
 * @return The float value of the specified light property.
 */
float RLG_GetLightValue(unsigned int light, RLG_LightProperty property);

/**
 * @brief Get the Vector3 value of a specific light property.
 * 
 * @param light The index of the light to retrieve the value from.
 * @param property The light property to retrieve the Vector3 value for.
 * @return The Vector3 value of the specified light property.
 */
Vector3 RLG_GetLightVec3(unsigned int light, RLG_LightProperty property);

/**
 * @brief Get the color value of a specific light.
 * 
 * @param light The index of the light to retrieve the value from.
 * @return The color value of the specified light.
 */
Color RLG_GetLightColor(unsigned int light);

/**
 * @brief Translate the position of a specific light by the given offsets.
 * 
 * This function adjusts the position of the specified light by adding the
 * provided x, y, and z offsets to its current position.
 *
 * @param light The index of the light to translate.
 * @param x The offset to add to the x-coordinate of the light position.
 * @param y The offset to add to the y-coordinate of the light position.
 * @param z The offset to add to the z-coordinate of the light position.
 */
void RLG_LightTranslate(unsigned int light, float x, float y, float z);

/**
 * @brief Translate the position of a specific light by the given vector.
 * 
 * This function adjusts the position of the specified light by adding the
 * provided vector to its current position.
 *
 * @param light The index of the light to translate.
 * @param v The vector to add to the light position.
 */
void RLG_LightTranslateV(unsigned int light, Vector3 v);

/**
 * @brief Rotate the direction of a specific light around the X-axis.
 * 
 * This function rotates the direction vector of the specified light by the given
 * degrees around the X-axis.
 *
 * @param light The index of the light to rotate.
 * @param degrees The angle in degrees to rotate the light direction.
 */
void RLG_LightRotateX(unsigned int light, float degrees);

/**
 * @brief Rotate the direction of a specific light around the Y-axis.
 * 
 * This function rotates the direction vector of the specified light by the given
 * degrees around the Y-axis.
 *
 * @param light The index of the light to rotate.
 * @param degrees The angle in degrees to rotate the light direction.
 */
void RLG_LightRotateY(unsigned int light, float degrees);

/**
 * @brief Rotate the direction of a specific light around the Z-axis.
 * 
 * This function rotates the direction vector of the specified light by the given
 * degrees around the Z-axis.
 *
 * @param light The index of the light to rotate.
 * @param degrees The angle in degrees to rotate the light direction.
 */
void RLG_LightRotateZ(unsigned int light, float degrees);

/**
 * @brief Rotate the direction of a specific light around an arbitrary axis.
 * 
 * This function rotates the direction vector of the specified light by the given
 * degrees around the specified axis.
 *
 * @param light The index of the light to rotate.
 * @param axis The axis to rotate around.
 * @param degrees The angle in degrees to rotate the light direction.
 */
void RLG_LightRotate(unsigned int light, Vector3 axis, float degrees);

/**
 * @brief Set the target position of a specific light.
 * 
 * @param light The index of the light to set the target position for.
 * @param x The x-coordinate of the target position.
 * @param y The y-coordinate of the target position.
 * @param z The z-coordinate of the target position.
 */
void RLG_SetLightTarget(unsigned int light, float x, float y, float z);

/**
 * @brief Set the target position of a specific light using a Vector3 structure.
 * 
 * @param light The index of the light to set the target position for.
 * @param targetPosition The target position of the light as a Vector3 structure.
 */
void RLG_SetLightTargetV(unsigned int light, Vector3 targetPosition);

/**
 * @brief Get the target position of a specific light.
 * 
 * @param light The index of the light to get the target position for.
 * @return The target position of the light as a Vector3 structure.
 */
Vector3 RLG_GetLightTarget(unsigned int light);

/**
 * @brief Enable shadow casting for a light.
 *
 * @warning Shadow casting is not fully functional for omnilights yet. Please specify the light direction.
 * 
 * @param light The index of the light to enable shadow casting for.
 * @param shadowMapResolution The resolution of the shadow map.
 * 
 * @todo Implement shadow casting feature for omnilights using cubemaps.
 *       Previous attempts have been made, but it might be optimal to
 *       directly call OpenGL functions bypassing RLGL, though this
 *       approach could pose issues for some users...
 */
void RLG_EnableShadow(unsigned int light, int shadowMapResolution);

/**
 * @brief Disable shadow casting for a light.
 * 
 * @param light The index of the light to disable shadow casting for.
 */
void RLG_DisableShadow(unsigned int light);

/**
 * @brief Check if shadow casting is enabled for a light.
 * 
 * @param light The index of the light to check for shadow casting.
 * @return true if shadow casting is enabled, false otherwise.
 */
bool RLG_IsShadowEnabled(unsigned int light);

/**
 * @brief Set the bias value for shadow mapping of a light.
 * 
 * @param light The index of the light to set the shadow bias for.
 * @param value The bias value to set.
 */
void RLG_SetShadowBias(unsigned int light, float value);

/**
 * @brief Get the bias value for shadow mapping of a light.
 * 
 * @param light The index of the light to get the shadow bias for.
 * @return The shadow bias value.
 */
float RLG_GetShadowBias(unsigned int light);

/**
 * @brief Updates the shadow map for a given light source.
 * 
 * This function updates the shadow map for the specified light by calling the provided draw function.
 * It sets the active shadow map for the light and uses the draw function to render the scene.
 * 
 * @param light The identifier of the light source for which to update the shadow map.
 * @param drawFunc The function to draw the scene for shadow rendering.
 */
void RLG_UpdateShadowMap(unsigned int light, RLG_DrawFunc drawFunc);

/**
 * @brief Retrieves the shadow map texture for a given light source.
 * 
 * @param light The identifier of the light source for which to retrieve the shadow map.
 * @return The shadow map texture for the specified light source.
 */
Texture RLG_GetShadowMap(unsigned int light);

/**
 * @brief Casts a mesh for shadow rendering.
 * 
 * @param shader The shader to use for rendering the mesh.
 * @param mesh The mesh to cast.
 * @param transform The transformation matrix to apply to the mesh.
 */
void RLG_CastMesh(Shader shader, Mesh mesh, Matrix transform);

/**
 * @brief Casts a model for shadow rendering.
 * 
 * @param shader The shader to use for rendering the model.
 * @param model The model to cast.
 * @param position The position at which to cast the model for shadow rendering.
 * @param scale The scale at which to cast the model for shadow rendering.
 */
void RLG_CastModel(Shader shader, Model model, Vector3 position, float scale);

/**
 * @brief Casts a model for shadow rendering with extended parameters.
 * 
 * @param shader The shader to use for rendering the model.
 * @param model The model to cast.
 * @param position The position at which to cast the model for shadow rendering.
 * @param rotationAxis The axis around which to rotate the model for shadow rendering.
 * @param rotationAngle The angle by which to rotate the model for shadow rendering.
 * @param scale The scale at which to cast the model for shadow rendering.
 */
void RLG_CastModelEx(Shader shader, Model model, Vector3 position, Vector3 rotationAxis, float rotationAngle, Vector3 scale);

/**
 * @brief Draw a mesh with a specified material and transformation.
 * 
 * This function draws a mesh with the specified material and transformation.
 * 
 * @param mesh The mesh to draw.
 * @param material The material to apply to the mesh.
 * @param transform The transformation matrix to apply to the mesh.
 */
void RLG_DrawMesh(Mesh mesh, Material material, Matrix transform);

/**
 * @brief Draw a model at a specified position with a specified scale and tint.
 * 
 * This function draws a model at the specified position with the specified scale and tint.
 * 
 * @param model The model to draw.
 * @param position The position at which to draw the model.
 * @param scale The scale at which to draw the model.
 * @param tint The tint color to apply to the model.
 */
void RLG_DrawModel(Model model, Vector3 position, float scale, Color tint);

/**
 * @brief Draw a model at a specified position with a specified rotation, scale, and tint.
 * 
 * This function draws a model at the specified position with the specified rotation, scale, and tint.
 * 
 * @param model The model to draw.
 * @param position The position at which to draw the model.
 * @param rotationAxis The axis around which to rotate the model.
 * @param rotationAngle The angle by which to rotate the model.
 * @param scale The scale at which to draw the model.
 * @param tint The tint color to apply to the model.
 */
void RLG_DrawModelEx(Model model, Vector3 position, Vector3 rotationAxis, float rotationAngle, Vector3 scale, Color tint);

/**
 * @brief Loads a skybox from a file.
 *
 * This function loads a skybox texture from the specified file and returns
 * a RLG_Skybox structure containing the cubemap texture.
 *
 * @param skyboxFileName The path to the skybox texture file.
 * @return RLG_Skybox The loaded skybox.
 */
RLG_Skybox RLG_LoadSkybox(const char* skyboxFileName);

/**
 * @brief Loads a HDR skybox from a file with specified size and format.
 *
 * This function loads a high dynamic range (HDR) skybox texture from the
 * specified file. It creates a cubemap texture with the given size and format.
 *
 * @note On some Android devices with WebGL, framebuffer objects (FBO)
 * do not properly support a FLOAT-based attachment, so the function uses
 * PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 instead of PIXELFORMAT_UNCOMPRESSED_R32G32B32A32.
 *
 * @param skyboxFileName The path to the skybox texture file.
 * @param size The size of the cubemap texture.
 * @param format The pixel format of the cubemap texture.
 * @return RLG_Skybox The loaded HDR skybox.
 */
RLG_Skybox RLG_LoadSkyboxHDR(const char* skyboxFileName, int size, int format);

/**
 * @brief Unloads a skybox.
 *
 * This function unloads the specified skybox, freeing the associated
 * resources.
 *
 * @param skybox The skybox to be unloaded.
 */
void RLG_UnloadSkybox(RLG_Skybox skybox);

/**
 * @brief Draws a skybox.
 *
 * This function renders the specified skybox.
 *
 * @param skybox The skybox to be drawn.
 */
void RLG_DrawSkybox(RLG_Skybox skybox);


/* Misc Helper Functions */

unsigned int EXT_LoadShaderEx(const char** vsCodes, const char** fsCodes, int vsCount, int fsCount);


#if defined(__cplusplus)
}
#endif

#ifdef RLIGHTS_IMPLEMENTATION

#include <raymath.h>
#include <stdlib.h>
#include <stdio.h>
#include <rlgl.h>

/* Helper macros */

#define STRINGIFY(x) #x             ///< NOTE: Undefined at the end of the header
#define TOSTRING(x) STRINGIFY(x)    ///< NOTE: Undefined at the end of the header

// General macro to initialize a structure with specific values
// NOTE: Undefine at the end of the header
// In C++, use the brace initializer list syntax: type{...}
// In C, use the designated initializer syntax: (type){...}
#ifdef __cplusplus
    #define INIT_STRUCT_ZERO(type) {}
    #define INIT_STRUCT(type, ...) { __VA_ARGS__ }
#else
    #define INIT_STRUCT_ZERO(type) (type) { 0 }
    #define INIT_STRUCT(type, ...) (type) { __VA_ARGS__ }
#endif

/* Helper defintions */

#define RLG_COUNT_MATERIAL_MAPS 12  ///< Same as MAX_MATERIAL_MAPS defined in raylib/config.h
#define RLG_COUNT_SHADERS 6         ///< Total shader used by rlights.h internally

/* Uniform names definitions */

#define RLG_SHADER_ATTRIB_POSITION              "vertexPosition"
#define RLG_SHADER_ATTRIB_TEXCOORD              "vertexTexCoord"
#define RLG_SHADER_ATTRIB_TEXCOORD2             "vertexTexCoord2"
#define RLG_SHADER_ATTRIB_NORMAL                "vertexNormal"
#define RLG_SHADER_ATTRIB_TANGENT               "vertexTangent"
#define RLG_SHADER_ATTRIB_COLOR                 "vertexColor"

#define RLG_SHADER_UNIFORM_MATRIX_MVP           "mvp"
#define RLG_SHADER_UNIFORM_MATRIX_VIEW          "matView"
#define RLG_SHADER_UNIFORM_MATRIX_PROJECTION    "matProjection"
#define RLG_SHADER_UNIFORM_MATRIX_MODEL         "matModel"
#define RLG_SHADER_UNIFORM_MATRIX_NORMAL        "matNormal"

#define RLG_SHADER_UNIFORM_COLOR_AMBIENT        "colAmbient"
#define RLG_SHADER_UNIFORM_VIEW_POSITION        "viewPos"

/* Embedded shaders definition */

#ifndef NO_EMBEDDED_SHADERS

#define GLSL_VERSION_DEF \
    "#version " TOSTRING(GLSL_VERSION) "\n"

#define GLSL_NUM_LIGHTS \
    "#define NUM_LIGHTS " TOSTRING(RLG_MAX_LIGHTS_PER_MATERIAL) "\n"

#if GLSL_VERSION < 330

#   define GLSL_TEXTURE_DEF         "#define TEX texture2D\n"
#   define GLSL_TEXTURE_CUBE_DEF    "#define TEXCUBE textureCube\n"

#   define GLSL_FS_OUT_DEF          ""

#   define GLSL_FINAL_COLOR(x)      "gl_FragColor = " x ";"
#   define GLSL_PRECISION(x)        "precision " x ";"

#   define GLSL_VS_IN(x)            "attribute " x ";"
#   define GLSL_FS_IN(x)            "varying " x ";"
#   define GLSL_VS_OUT(x)           "varying " x ";"

#else

#   define GLSL_TEXTURE_DEF         "#define TEX texture\n"
#   define GLSL_TEXTURE_CUBE_DEF    "#define TEXCUBE texture\n"

#   define GLSL_FS_OUT_DEF          "out vec4 _;"

#   define GLSL_FINAL_COLOR(x)      "_ = " x ";"
#   define GLSL_PRECISION(x)        ""

#   define GLSL_VS_IN(x)            "in " x ";"
#   define GLSL_FS_IN(x)            "in " x ";"
#   define GLSL_VS_OUT(x)           "out " x ";"

#endif

/* Shader */

static const char G_VS_Model[] =
{
#   if GLSL_VERSION > 100
    "uniform mat4 matLights[NUM_LIGHTS];"
    GLSL_VS_OUT("vec4 fragPosLightSpace[NUM_LIGHTS]")
#   endif

    GLSL_VS_IN("vec3 " RLG_SHADER_ATTRIB_POSITION)
    GLSL_VS_IN("vec2 " RLG_SHADER_ATTRIB_TEXCOORD)
    GLSL_VS_IN("vec4 " RLG_SHADER_ATTRIB_TANGENT)
    GLSL_VS_IN("vec3 " RLG_SHADER_ATTRIB_NORMAL)
    GLSL_VS_IN("vec4 " RLG_SHADER_ATTRIB_COLOR)

    "uniform lowp int useNormalMap;"
    "uniform mat4 " RLG_SHADER_UNIFORM_MATRIX_NORMAL ";"
    "uniform mat4 " RLG_SHADER_UNIFORM_MATRIX_MODEL ";"
    "uniform mat4 " RLG_SHADER_UNIFORM_MATRIX_MVP ";"

    GLSL_VS_OUT("vec3 fragPosition")
    GLSL_VS_OUT("vec2 fragTexCoord")
    GLSL_VS_OUT("vec3 fragNormal")
    GLSL_VS_OUT("vec4 fragColor")
    GLSL_VS_OUT("mat3 TBN")

    "void main()"
    "{"
        "fragPosition = vec3(" RLG_SHADER_UNIFORM_MATRIX_MODEL "*vec4(" RLG_SHADER_ATTRIB_POSITION ", 1.0));"
        "fragNormal = (" RLG_SHADER_UNIFORM_MATRIX_NORMAL "*vec4(" RLG_SHADER_ATTRIB_NORMAL ", 0.0)).xyz;"

        "fragTexCoord = " RLG_SHADER_ATTRIB_TEXCOORD ";"
        "fragColor = " RLG_SHADER_ATTRIB_COLOR ";"

        // The TBN matrix is used to transform vectors from tangent space to world space
        // It is currently used to transform normals from a normal map to world space normals
        "vec3 T = normalize(vec3(" RLG_SHADER_UNIFORM_MATRIX_MODEL "*vec4(" RLG_SHADER_ATTRIB_TANGENT ".xyz, 0.0)));"
        "vec3 B = cross(fragNormal, T)*" RLG_SHADER_ATTRIB_TANGENT ".w;"
        "TBN = mat3(T, B, fragNormal);"

#       if GLSL_VERSION > 100
        "for (int i = 0; i < NUM_LIGHTS; i++)"
        "{"
            "fragPosLightSpace[i] = matLights[i]*vec4(fragPosition, 1.0);"
        "}"
#       endif

        "gl_Position = " RLG_SHADER_UNIFORM_MATRIX_MVP "*vec4(" RLG_SHADER_ATTRIB_POSITION ", 1.0);"
    "}"
};

static const char G_FS_Model[] =
{
    GLSL_TEXTURE_DEF
    GLSL_TEXTURE_CUBE_DEF

    "#define NUM_MATERIAL_MAPS"         " 7\n"
    "#define NUM_MATERIAL_CUBEMAPS"     " 2\n"

    "#define DIRLIGHT"                  " 0\n"
    "#define OMNILIGHT"                 " 1\n"
    "#define SPOTLIGHT"                 " 2\n"

    "#define ALBEDO"                    " 0\n"
    "#define METALNESS"                 " 1\n"
    "#define NORMAL"                    " 2\n"
    "#define ROUGHNESS"                 " 3\n"
    "#define OCCLUSION"                 " 4\n"
    "#define EMISSION"                  " 5\n"
    "#define HEIGHT"                    " 6\n"

    "#define CUBEMAP"                   " 0\n"
    "#define IRRADIANCE"                " 1\n"

    "#define PI 3.1415926535897932384626433832795028\n"

    GLSL_PRECISION("mediump float")

#   if GLSL_VERSION > 100
    GLSL_FS_IN("vec4 fragPosLightSpace[NUM_LIGHTS]")
#   else
    "uniform mat4 matLights[NUM_LIGHTS];"
#   endif

    GLSL_FS_IN("vec3 fragPosition")
    GLSL_FS_IN("vec2 fragTexCoord")
    GLSL_FS_IN("vec3 fragNormal")
    GLSL_FS_IN("vec4 fragColor")
    GLSL_FS_IN("mat3 TBN")

    GLSL_FS_OUT_DEF

    "struct MaterialMap {"
        "sampler2D texture;"
        "mediump vec4 color;"
        "mediump float value;"
        "lowp int active;"
    "};"

    "struct MaterialCubemap {"
        "samplerCube texture;"
        "mediump vec4 color;"
        "mediump float value;"
        "lowp int active;"
    "};"

    "struct Light {"
        "samplerCube shadowCubemap;"    ///< Sampler for the shadow map texture
        "sampler2D shadowMap;"          ///< Sampler for the shadow map texture
        "vec3 position;"                ///< Position of the light in world coordinates
        "vec3 direction;"               ///< Direction vector of the light (for directional and spotlights)
        "vec3 color;"                   ///< Diffuse color of the light
        "float energy;"                 ///< Energy factor of the diffuse light color
        "float specular;"               ///< Specular amount of the light
        "float size;"                   ///< Light size (spotlight, omnilight only)
        "float innerCutOff;"            ///< Inner cutoff angle for spotlights (cosine of the angle)
        "float outerCutOff;"            ///< Outer cutoff angle for spotlights (cosine of the angle)
        "float distance;"               ///< Indicates the distance up to which the spotlights and omnilights shine
        "float attenuation;"            ///< Light attenuation factor along the illumination distance of spotlights and omnilights
        "float shadowMapTxlSz;"         ///< Texel size of the shadow map
        "float depthBias;"              ///< Bias value to avoid self-shadowing artifacts
        "lowp int type;"                ///< Type of the light (e.g., point, directional, spotlight)
        "lowp int shadow;"              ///< Indicates if the light casts shadows (1 for true, 0 for false)
        "lowp int enabled;"             ///< Indicates if the light is active (1 for true, 0 for false)
    "};"

    "uniform MaterialCubemap cubemaps[NUM_MATERIAL_CUBEMAPS];"
    "uniform MaterialMap maps[NUM_MATERIAL_MAPS];"
    "uniform Light lights[NUM_LIGHTS];"

    "uniform lowp int parallaxMinLayers;"
    "uniform lowp int parallaxMaxLayers;"

    "uniform float farPlane;"   ///< Used to scale depth values ​​when reading the depth cubemap (point shadows)

    "uniform vec3 " RLG_SHADER_UNIFORM_COLOR_AMBIENT ";"
    "uniform vec3 " RLG_SHADER_UNIFORM_VIEW_POSITION ";"

    "float DistributionGGX(float cosTheta, float alpha)"
    "{"
        "float a = cosTheta*alpha;"
        "float k = alpha/(1.0 - cosTheta*cosTheta + a*a);"
        "return k*k*(1.0/PI);"
    "}"

    // From Earl Hammon, Jr. "PBR Diffuse Lighting for GGX+Smith Microsurfaces"
    // SEE: https://www.gdcvault.com/play/1024478/PBR-Diffuse-Lighting-for-GGX
    "float GeometrySmith(float NdotL, float NdotV, float alpha)"
    "{"
        "return 0.5/mix(2.0*NdotL*NdotV, NdotL + NdotV, alpha);"
    "}"

    "float SchlickFresnel(float u)"
    "{"
        "float m = 1.0 - u;"
        "float m2 = m*m;"
        "return m2*m2*m;" // pow(m,5)
    "}"

    "vec3 ComputeF0(float metallic, float specular, vec3 albedo)"
    "{"
        "float dielectric = 0.16*specular*specular;"
        // use albedo*metallic as colored specular reflectance at 0 angle for metallic materials
        // SEE: https://google.github.io/filament/Filament.md.html
        "return mix(vec3(dielectric), albedo, vec3(metallic));"
    "}"

    "vec2 Parallax(vec2 uv, vec3 V)"
    "{"
        "float height = 1.0 - TEX(maps[HEIGHT].texture, uv).r;"
        "return uv - vec2(V.xy/V.z)*height*maps[HEIGHT].value;"
    "}"

    "vec2 DeepParallax(vec2 uv, vec3 V)"
    "{"
        "float numLayers = mix("
            "float(parallaxMaxLayers),"
            "float(parallaxMinLayers),"
            "abs(dot(vec3(0.0, 0.0, 1.0), V)));"

        "float layerDepth = 1.0/numLayers;"
        "float currentLayerDepth = 0.0;"

        "vec2 P = V.xy/V.z*maps[HEIGHT].value;"
        "vec2 deltaTexCoord = P/numLayers;"
    
        "vec2 currentUV = uv;"
        "float currentDepthMapValue = 1.0 - TEX(maps[HEIGHT].texture, currentUV).y;"
        
        "while(currentLayerDepth < currentDepthMapValue)"
        "{"
            "currentUV += deltaTexCoord;"
            "currentLayerDepth += layerDepth;"
            "currentDepthMapValue = 1.0 - TEX(maps[HEIGHT].texture, currentUV).y;"
        "}"

        "vec2 prevTexCoord = currentUV - deltaTexCoord;"
        "float afterDepth  = currentDepthMapValue + currentLayerDepth;"
        "float beforeDepth = 1.0 - TEX(maps[HEIGHT].texture,"
            "prevTexCoord).y - currentLayerDepth - layerDepth;"

        "float weight = afterDepth/(afterDepth - beforeDepth);"
        "return prevTexCoord*weight + currentUV*(1.0 - weight);"
    "}"

    "float ShadowOmni(int i, float cNdotL)"
    "{"
        "vec3 fragToLight = fragPosition - lights[i].position;"
        "float closestDepth = TEXCUBE(lights[i].shadowCubemap, fragToLight).r;"
        "closestDepth *= farPlane;" // Rescale depth
        "float currentDepth = length(fragToLight);"
        "float bias = lights[i].depthBias*max(1.0 - cNdotL, 0.05);"
        "return currentDepth - bias > closestDepth ? 0.0 : 1.0;"
    "}"

    "float Shadow(int i, float cNdotL)"
    "{"
#       if GLSL_VERSION > 100
        "vec4 p = fragPosLightSpace[i];"
#       else
        "vec4 p = matLights[i]*vec4(fragPosition, 1.0);"
#       endif

        "vec3 projCoords = p.xyz/p.w;"
        "projCoords = projCoords*0.5 + 0.5;"

        "float bias = max(lights[i].depthBias*(1.0 - cNdotL), 0.00002) + 0.00001;"
        "projCoords.z -= bias;"

        "if (projCoords.z > 1.0 || projCoords.x > 1.0 || projCoords.y > 1.0)"
        "{"
            "return 1.0;"
        "}"

        "float depth = projCoords.z;"
        "float shadow = 0.0;"

        // NOTE: You can increase iterations to improve PCF quality
        "for (int x = -1; x <= 1; x++)"
        "{"
            "for (int y = -1; y <= 1; y++)"
            "{"
                "float pcfDepth = TEX(lights[i].shadowMap, projCoords.xy + vec2(x, y)*lights[i].shadowMapTxlSz).r;"
                "shadow += step(depth, pcfDepth);"
            "}"
        "}"

        "return shadow/9.0;"
    "}"

    "void main()"
    "{"
        // Compute the view direction vector for this fragment
        "vec3 V = normalize(" RLG_SHADER_UNIFORM_VIEW_POSITION " - fragPosition);"

        // Compute fragTexCoord (UV), apply parallax if height map is enabled
        "vec2 uv = fragTexCoord;"
        "if (maps[HEIGHT].active != 0)"
        "{"
            "uv = (parallaxMinLayers > 0 && parallaxMaxLayers > 1)"
                "? DeepParallax(uv, V) : Parallax(uv, V);"

            "if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0)"
            "{"
                "discard;"
            "}"
        "}"

        // Compute albedo (base color) by sampling the texture and multiplying by the diffuse color
        "vec3 albedo = maps[ALBEDO].color.rgb*fragColor.rgb;"
        "if (maps[ALBEDO].active != 0)"
            "albedo *= TEX(maps[ALBEDO].texture, uv).rgb;"

        // Compute metallic factor; if a metalness map is used, sample it
        "float metalness = maps[METALNESS].value;"
        "if (maps[METALNESS].active != 0)"
            "metalness *= TEX(maps[METALNESS].texture, uv).b;"

        // Compute roughness factor; if a roughness map is used, sample it
        "float roughness = maps[ROUGHNESS].value;"
        "if (maps[ROUGHNESS].active != 0)"
            "roughness *= TEX(maps[ROUGHNESS].texture, uv).g;"

        // Compute F0 (reflectance at normal incidence) based on the metallic factor
        "vec3 F0 = ComputeF0(metalness, 0.5, albedo);"

        // Compute the normal vector; if a normal map is used, transform it to tangent space
        "vec3 N = (maps[NORMAL].active == 0) ? normalize(fragNormal)"
            ": normalize(TBN*(TEX(maps[NORMAL].texture, uv).rgb*2.0 - 1.0));"

        // Compute the dot product of the normal and view direction
        "float NdotV = dot(N, V);"
        "float cNdotV = max(NdotV, 1e-4);"  // Clamped to avoid division by zero

        // Initialize diffuse and specular lighting accumulators
        "vec3 diffLighting = vec3(0.0);"
        "vec3 specLighting = vec3(0.0);"

        // Loop through all lights
        "for (int i = 0; i < NUM_LIGHTS; i++)"
        "{"
            "if (lights[i].enabled != 0)"
            "{"
                "float size_A = 0.0;"
                "vec3 L = vec3(0.0);"

                // Compute the light direction vector
                "if (lights[i].type != DIRLIGHT)"
                "{"
                    "vec3 LV = lights[i].position - fragPosition;"
                    "L = normalize(LV);"

                    // If the light has a size, compute the attenuation factor based on the distance
                    "if (lights[i].size > 0.0)"
                    "{"
                        "float t = lights[i].size/max(0.001, length(LV));"
                        "size_A = max(0.0, 1.0 - 1.0/sqrt(1.0 + t*t));"
                    "}"
                "}"
                "else"
                "{"
                    // For directional lights, use the negative direction as the light direction
                    "L = normalize(-lights[i].direction);"
                "}"

                // Compute the dot product of the normal and light direction, adjusted by size_A
                "float NdotL = min(size_A + dot(N, L), 1.0);"
                "float cNdotL = max(NdotL, 0.0);" // clamped NdotL

                // Compute the halfway vector between the view and light directions
                "vec3 H = normalize(V + L);"
                "float cNdotH = clamp(size_A + dot(N, H), 0.0, 1.0);"
                "float cLdotH = clamp(size_A + dot(L, H), 0.0, 1.0);"

                // Compute light color energy
                "vec3 lightColE = lights[i].color*lights[i].energy;"

                // Compute diffuse lighting (Burley model) if the material is not fully metallic
                "vec3 diffLight = vec3(0.0);"
                "if (metalness < 1.0)"
                "{"
                    "float FD90_minus_1 = 2.0*cLdotH*cLdotH*roughness - 0.5;"
                    "float FdV = 1.0 + FD90_minus_1*SchlickFresnel(cNdotV);"
                    "float FdL = 1.0 + FD90_minus_1*SchlickFresnel(cNdotL);"

                    "float diffBRDF = (1.0/PI)*FdV*FdL*cNdotL;"
                    "diffLight = diffBRDF*lightColE;"
                "}"

                // Compute specular lighting using the Schlick-GGX model
                // NOTE: When roughness is 0, specular light should not be entirely disabled.
                // TODO: Handle perfect mirror reflection when roughness is 0.
                "vec3 specLight = vec3(0.0);"
                "if (roughness > 0.0)"
                "{"
                    "float alphaGGX = roughness*roughness;"
                    "float D = DistributionGGX(cNdotH, alphaGGX);"
                    "float G = GeometrySmith(cNdotL, cNdotV, alphaGGX);"

                    "float cLdotH5 = SchlickFresnel(cLdotH);"
                    "float F90 = clamp(50.0*F0.g, 0.0, 1.0);"
                    "vec3 F = F0 + (F90 - F0)*cLdotH5;"

                    "vec3 specBRDF = cNdotL*D*F*G;"
                    "specLight = specBRDF*lightColE*lights[i].specular;"
                "}"

                // Apply shadow factor if the light casts shadows
                "float shadow = 1.0;"
                "if (lights[i].shadow != 0)"
                "{"
                    "shadow = (lights[i].type == OMNILIGHT)"
                        "? ShadowOmni(i, cNdotL) : Shadow(i, cNdotL);"
                "}"

                // Apply attenuation based on the distance from the light
                "if (lights[i].type != DIRLIGHT) {"
                    "float distance = length(lights[i].position - fragPosition);"
                    "float atten = 1.0 - clamp(distance / lights[i].distance, 0.0, 1.0);"
                    "shadow *= atten*lights[i].attenuation;"
                "}"

                // Apply spotlight effect if the light is a spotlight
                "if (lights[i].type == SPOTLIGHT)"
                "{"
                    "float theta = dot(L, normalize(-lights[i].direction));"
                    "float epsilon = (lights[i].innerCutOff - lights[i].outerCutOff);"
                    "shadow *= smoothstep(0.0, 1.0, (theta - lights[i].outerCutOff)/epsilon);"
                "}"

                // Accumulate the diffuse and specular lighting contributions
                "diffLighting += diffLight*shadow;"
                "specLighting += specLight*shadow;"
            "}"
        "}"

        // Compute ambient
        "vec3 ambient = " RLG_SHADER_UNIFORM_COLOR_AMBIENT ";"
        "if (cubemaps[IRRADIANCE].active != 0)"
        "{"
            "vec3 kS = F0 + (1.0 - F0)*SchlickFresnel(cNdotV);"
            "vec3 kD = (1.0 - kS)*(1.0 - metalness);"
            "ambient = kD*TEXCUBE(cubemaps[IRRADIANCE].texture, N).rgb;"
        "}"

        // Compute ambient occlusion
        "if (maps[OCCLUSION].active != 0)"
        "{"
            "float ao = TEX(maps[OCCLUSION].texture, uv).r;"
            "ambient *= ao;"

            "float lightAffect = mix(1.0, ao, maps[OCCLUSION].value);"
            "diffLighting *= lightAffect;"
            "specLighting *= lightAffect;"
        "}"

        // Skybox reflection
        "if (cubemaps[CUBEMAP].active != 0)"
        "{"
            "vec3 reflectCol = TEXCUBE(cubemaps[CUBEMAP].texture, reflect(-V, N)).rgb;"
            "specLighting = mix(specLighting, reflectCol, 1.0 - roughness);"
        "}"

        // Compute the final diffuse color, including ambient and diffuse lighting contributions
        "vec3 diffuse = albedo*(ambient + diffLighting);"

        // Compute emission color; if an emissive map is used, sample it
        "vec3 emission = maps[EMISSION].color.rgb;"
        "if (maps[EMISSION].active != 0)"
        "{"
            "emission *= TEX(maps[EMISSION].texture, uv).rgb;"
        "}"

        // Compute the final fragment color by combining diffuse, specular, and emission contributions
        GLSL_FINAL_COLOR("vec4(diffuse + specLighting + emission, 1.0)")
    "}"
};

static const char G_VS_Depth[] =
{
    GLSL_VERSION_DEF
    GLSL_VS_IN("vec3 vertexPosition")
    "uniform mat4 mvp;"
    "void main()"
    "{"
        "gl_Position = mvp*vec4(vertexPosition, 1.0);"
    "}"
};

static const char G_FS_Depth[] =
{
    GLSL_VERSION_DEF
    GLSL_PRECISION("mediump float")
    "void main(){}"
};

static const char G_VS_DepthCubemap[] =
{
    GLSL_VERSION_DEF

    GLSL_VS_IN("vec3 vertexPosition")
    GLSL_VS_OUT("vec3 fragPosition")

    "uniform mat4 matModel;"
    "uniform mat4 mvp;"

    "void main()"
    "{"
        "fragPosition = vec3(matModel*vec4(vertexPosition, 1.0));"
        "gl_Position = mvp*vec4(vertexPosition, 1.0);"
    "}"
};

static const char G_FS_DepthCubemap[] =
{
    GLSL_VERSION_DEF

    GLSL_PRECISION("mediump float")
    GLSL_FS_IN("vec3 fragPosition")

    "uniform vec3 lightPos;"
    "uniform float farPlane;"

    "void main()"
    "{"
        "float lightDistance = length(fragPosition - lightPos);"
        "lightDistance = lightDistance/farPlane;"
        "gl_FragDepth = lightDistance;"
    "}"
};

// NOTE: This shader is no longer used currently, I keep it just in case
static const char G_FS_DEBUG_DepthMapDrawing[] =
{
    GLSL_VERSION_DEF

    GLSL_PRECISION("mediump float")
    GLSL_FS_IN("vec2 fragTexCoord")

    "uniform sampler2D texture0;"
    "uniform float near;"
    "uniform float far;"

    GLSL_FS_OUT_DEF

    "void main()"
    "{"
        "float depth = TEX(texture0, vec2(fragTexCoord.x, 1.0 - fragTexCoord.y)).r;"
        "depth = (2.0*near*far)/(far + near - (depth*2.0 - 1.0)*(far - near));"
        GLSL_FINAL_COLOR("vec4(vec3(depth/far), 1.0)")
    "}"
};

static const char G_VS_Cubemap[] =
{
    GLSL_VERSION_DEF

    GLSL_VS_IN("vec3 vertexPosition")
    GLSL_VS_OUT("vec3 fragPosition")

    "uniform mat4 matProjection;"
    "uniform mat4 matView;"

    "void main()"
    "{"
        "fragPosition = vertexPosition;"
        "gl_Position = matProjection*matView*vec4(vertexPosition, 1.0);"
    "}"
};

static const char G_FS_EquirectangularToCubemap[] =
{
    GLSL_VERSION_DEF
    GLSL_TEXTURE_DEF

    GLSL_PRECISION("mediump float")
    GLSL_FS_IN("vec3 fragPosition")
    GLSL_FS_OUT_DEF

    "uniform sampler2D equirectangularMap;"

    "vec2 SampleSphericalMap(vec3 v)"
    "{"
        "vec2 uv = vec2(atan(v.z, v.x), asin(v.y));"
        "uv *= vec2(0.1591, -0.3183);" // negative Y, to flip axis
        "uv += 0.5;"
        "return uv;"
    "}"

    "void main()"
    "{"
        "vec2 uv = SampleSphericalMap(normalize(fragPosition));"
        "vec3 color = TEX(equirectangularMap, uv).rgb;"
        GLSL_FINAL_COLOR("vec4(color, 1.0)")
    "}"
};

static const char G_FS_IrradianceConvolution[] =
{
    GLSL_VERSION_DEF
    GLSL_TEXTURE_CUBE_DEF

    "#define PI 3.14159265359\n"

    GLSL_PRECISION("mediump float")
    GLSL_FS_IN("vec3 fragPosition")
    GLSL_FS_OUT_DEF

    "uniform samplerCube environmentMap;"

    "void main()"
    "{"
        // The world vector acts as the normal of a tangent surface
        // from the origin, aligned to WorldPos. Given this normal, calculate all
        // incoming radiance of the environment. The result of this radiance
        // is the radiance of light coming from -Normal direction, which is what
        // we use in the PBR shader to sample irradiance.
        "vec3 N = normalize(fragPosition);"

        "vec3 irradiance = vec3(0.0);"
        
        // tangent space calculation from origin point
        "vec3 up = vec3(0.0, 1.0, 0.0);"
        "vec3 right = normalize(cross(up, N));"
        "up = normalize(cross(N, right));"

        "float sampleDelta = 0.025;"
        "float nrSamples = 0.0;"

        "for (float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta)"
        "{"
            "for (float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta)"
            "{"
                // spherical to cartesian (in tangent space)
                "vec3 tangentSample = vec3(sin(theta) * cos(phi),  sin(theta) * sin(phi), cos(theta));"

                // tangent space to world
                "vec3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N; "

                "irradiance += TEXCUBE(environmentMap, sampleVec).rgb * cos(theta) * sin(theta);"
                "nrSamples++;"
            "}"
        "}"

        "irradiance = PI * irradiance * (1.0 / float(nrSamples));"
        GLSL_FINAL_COLOR("vec4(irradiance, 1.0)")
    "}"
};

static const char G_VS_Skybox[] =
{
    GLSL_VERSION_DEF

    GLSL_VS_IN("vec3 vertexPosition")
    GLSL_VS_OUT("vec3 fragPosition")

    "uniform mat4 matProjection;"
    "uniform mat4 matView;"

    "void main()"
    "{"
        "fragPosition = vertexPosition;"
        "mat4 rotView = mat4(mat3(matView));"
        "vec4 clipPos = matProjection*rotView*vec4(vertexPosition, 1.0);"
        "gl_Position = clipPos;"
    "}"
};

static const char G_FS_Skybox[] =
{
    GLSL_VERSION_DEF
    GLSL_TEXTURE_CUBE_DEF

    GLSL_PRECISION("mediump float")
    GLSL_FS_IN("vec3 fragPosition")
    GLSL_FS_OUT_DEF

    "uniform samplerCube environmentMap;"
    "uniform bool doGamma;"

    "void main()"
    "{"
        "vec3 color = TEXCUBE(environmentMap, fragPosition).rgb;"

        "if (doGamma)" // Apply gamma correction
        "{"
            "color = color/(color + vec3(1.0));"
            "color = pow(color, vec3(1.0/2.2));"
        "}"

        GLSL_FINAL_COLOR("vec4(color, 1.0)")
    "}"
};

#endif //NO_EMBEDDED_SHADERS

/* Types definitions */

struct RLG_ShadowMap
{
    Texture2D depth;
    unsigned int id;
    int width, height;
};

struct RLG_Material ///< NOTE: This struct is used to handle data that cannot be stored in the MaterialMap struct of raylib.
{
    struct
    {
        int useMaps[RLG_COUNT_MATERIAL_MAPS];
        int parallaxMinLayers;
        int parallaxMaxLayers;
    }
    locs;

    struct
    {
        int useMaps[RLG_COUNT_MATERIAL_MAPS];
        int parallaxMinLayers;
        int parallaxMaxLayers;
    }
    data;
};

struct RLG_Light
{
    struct
    {
        int vpMatrix;       ///< NOTE: Not present in the Light shader struct but in a separate uniform
        int shadowCubemap;
        int shadowMap;
        int position;
        int direction;
        int color;
        int energy;
        int specular;
        int size;
        int innerCutOff;
        int outerCutOff;
        int distance;
        int attenuation;
        int shadowMapTxlSz;
        int depthBias;
        int type;
        int shadow;
        int enabled;
    }
    locs;

    struct
    {
        struct RLG_ShadowMap shadowMap;
        Vector3 position;
        Vector3 direction;
        Vector3 color;
        float energy;
        float specular;
        float size;
        float innerCutOff;
        float outerCutOff;
        float distance;
        float attenuation;
        float shadowMapTxlSz;
        float depthBias;
        int type;
        int shadow;
        int enabled;
    }
    data;
};

struct RLG_SkyboxHandler
{
    unsigned int vbo;   ///< VBO of cube positions
    unsigned int ebo;   ///< EBO (elements) of the cube
    unsigned int vao;   ///< VAO -> VBO + EBO

    unsigned int previousCubemapID;  /*< Indicates whether to update the data sent to the skybox
                                         shader if different from the ID of the skybox to render */
    int locDoGamma;
};

static struct RLG_Core
{
    /* Default material maps */

    MaterialMap defaultMaps[RLG_COUNT_MATERIAL_MAPS];
    bool usedDefaultMaps[RLG_COUNT_MATERIAL_MAPS];

    /* Shaders */

    Shader shaders[RLG_COUNT_SHADERS];

    /* Skybox handling data */

    struct RLG_SkyboxHandler skybox;

    /* Lighting shader data*/

    struct RLG_Light lights[RLG_MAX_LIGHTS_PER_MATERIAL];
    struct RLG_Material material;

    Vector3 colAmbient;
    Vector3 viewPos;

    /* Special values ​​and uniforms */

    float zNear;
    float zFar;

    int locDepthCubemapLightPos;
    int locDepthCubemapFar;
    int locLightingFar;
}
*rlgCtx = NULL;

#ifndef NO_EMBEDDED_SHADERS
    static const char
        *G_VS_CACHE_Model = G_VS_Model,
        *G_FS_CACHE_Model = G_FS_Model;
    static const char
        *G_VS_CACHE_Depth = G_VS_Depth,
        *G_FS_CACHE_Depth = G_FS_Depth;
    static const char
        *G_VS_CACHE_DepthCubemap = G_VS_DepthCubemap,
        *G_FS_CACHE_DepthCubemap = G_FS_DepthCubemap;
    static const char
        *G_VS_CACHE_IrradianceConvolution = G_VS_Cubemap,
        *G_FS_CACHE_IrradianceConvolution = G_FS_IrradianceConvolution;
    static const char
        *G_VS_CACHE_EquirectangularToCubemap = G_VS_Cubemap,
        *G_FS_CACHE_EquirectangularToCubemap = G_FS_EquirectangularToCubemap;
    static const char
        *G_VS_CACHE_Skybox = G_VS_Skybox,
        *G_FS_CACHE_Skybox = G_FS_Skybox;
#else
    static const char
        *G_FS_CACHE_Model                       = NULL,
        *G_VS_CACHE_Model                       = NULL,
        *G_VS_CACHE_Depth                       = NULL,
        *G_FS_CACHE_Depth                       = NULL,
        *G_VS_CACHE_DepthCubemap                = NULL,
        *G_FS_CACHE_DepthCubemap                = NULL,
        *G_VS_CACHE_IrradianceConvolution       = NULL,
        *G_FS_CACHE_IrradianceConvolution       = NULL,
        *G_VS_CACHE_EquirectangularToCubemap    = NULL,
        *G_FS_CACHE_EquirectangularToCubemap    = NULL,
        *G_VS_CACHE_Skybox                      = NULL,
        *G_FS_CACHE_Skybox                      = NULL;
#endif //NO_EMBEDDED_SHADERS

/* Public API */

RLG_Context RLG_CreateContext(void)
{
    // On-heap allocation for the context's core structure, initializing it with zeros
    struct RLG_Core *rlgCtx = (struct RLG_Core*)calloc(1, sizeof(struct RLG_Core));

    if (!rlgCtx)
    {
        TraceLog(LOG_FATAL, "Heap allocation for RLG context failed!");
        return NULL;
    }

    // We check if all the shader codes are well defined
    if (G_FS_CACHE_Model == NULL) TraceLog(LOG_WARNING, "The lighting vertex shader has not been defined.");
    if (G_VS_CACHE_Model == NULL) TraceLog(LOG_WARNING, "The lighting fragment shader has not been defined.");
    if (G_VS_CACHE_Depth == NULL) TraceLog(LOG_WARNING, "The depth vertex shader has not been defined.");
    if (G_FS_CACHE_Depth == NULL) TraceLog(LOG_WARNING, "The depth fragment shader has not been defined.");

    // Setup model shader code arrays
#   ifndef NO_EMBEDDED_SHADERS

    const int vsModelCount = 3;
    const int fsModelCount = 3;
    const char *vsModel[3] = { 0 };
    const char *fsModel[3] = { 0 };

    vsModel[0] = GLSL_VERSION_DEF;
    vsModel[1] = GLSL_NUM_LIGHTS;
    vsModel[2] = G_VS_CACHE_Model;

    fsModel[0] = GLSL_VERSION_DEF;
    fsModel[1] = GLSL_NUM_LIGHTS;
    fsModel[2] = G_FS_CACHE_Model;

#   else
    const int vsModelCount = 1;
    const int fsModelCount = 1;
    const char *vsModel[1] = { G_VS_CACHE_Model };
    const char *fsModel[1] = { G_FS_CACHE_Model };
#   endif //NO_EMBEDDED_SHADERS

    // Load shader and get locations
    Shader lightShader = { 0 };
    lightShader.id = EXT_LoadShaderEx(vsModel, fsModel, vsModelCount, fsModelCount);

    // After shader loading, we TRY to set default location names
    if (lightShader.id > 0)
    {
        // NOTE: Locations that cannot be retrieved are set to -1 by 'rlGetLocationAttrib'
        lightShader.locs = (int*)malloc(RLG_COUNT_LOCS*sizeof(int));

        // Get handles to GLSL input attribute locations
        lightShader.locs[RLG_LOC_VERTEX_POSITION]    = rlGetLocationAttrib(lightShader.id, RLG_SHADER_ATTRIB_POSITION);
        lightShader.locs[RLG_LOC_VERTEX_TEXCOORD01]  = rlGetLocationAttrib(lightShader.id, RLG_SHADER_ATTRIB_TEXCOORD);
        lightShader.locs[RLG_LOC_VERTEX_TEXCOORD02]  = rlGetLocationAttrib(lightShader.id, RLG_SHADER_ATTRIB_TEXCOORD2);
        lightShader.locs[RLG_LOC_VERTEX_NORMAL]      = rlGetLocationAttrib(lightShader.id, RLG_SHADER_ATTRIB_NORMAL);
        lightShader.locs[RLG_LOC_VERTEX_TANGENT]     = rlGetLocationAttrib(lightShader.id, RLG_SHADER_ATTRIB_TANGENT);
        lightShader.locs[RLG_LOC_VERTEX_COLOR]       = rlGetLocationAttrib(lightShader.id, RLG_SHADER_ATTRIB_COLOR);

        // Get handles to GLSL uniform locations (vertex shader)
        lightShader.locs[RLG_LOC_MATRIX_MVP]         = rlGetLocationUniform(lightShader.id, RLG_SHADER_UNIFORM_MATRIX_MVP);
        lightShader.locs[RLG_LOC_MATRIX_VIEW]        = rlGetLocationUniform(lightShader.id, RLG_SHADER_UNIFORM_MATRIX_VIEW);
        lightShader.locs[RLG_LOC_MATRIX_PROJECTION]  = rlGetLocationUniform(lightShader.id, RLG_SHADER_UNIFORM_MATRIX_PROJECTION);
        lightShader.locs[RLG_LOC_MATRIX_MODEL]       = rlGetLocationUniform(lightShader.id, RLG_SHADER_UNIFORM_MATRIX_MODEL);
        lightShader.locs[RLG_LOC_MATRIX_NORMAL]      = rlGetLocationUniform(lightShader.id, RLG_SHADER_UNIFORM_MATRIX_NORMAL);

        // Get handles to GLSL uniform locations (fragment shader)
        lightShader.locs[RLG_LOC_COLOR_AMBIENT]      = rlGetLocationUniform(lightShader.id, RLG_SHADER_UNIFORM_COLOR_AMBIENT);
        lightShader.locs[RLG_LOC_VECTOR_VIEW]        = rlGetLocationUniform(lightShader.id, RLG_SHADER_UNIFORM_VIEW_POSITION);

        lightShader.locs[RLG_LOC_COLOR_DIFFUSE]      = rlGetLocationUniform(lightShader.id, TextFormat("maps[%i].color", MATERIAL_MAP_ALBEDO));
        lightShader.locs[RLG_LOC_COLOR_SPECULAR]     = rlGetLocationUniform(lightShader.id, TextFormat("maps[%i].color", MATERIAL_MAP_METALNESS));
        lightShader.locs[RLG_LOC_COLOR_EMISSION]     = rlGetLocationUniform(lightShader.id, TextFormat("maps[%i].color", MATERIAL_MAP_EMISSION));

        lightShader.locs[RLG_LOC_MAP_ALBEDO]         = rlGetLocationUniform(lightShader.id, TextFormat("maps[%i].texture", MATERIAL_MAP_ALBEDO));
        lightShader.locs[RLG_LOC_MAP_METALNESS]      = rlGetLocationUniform(lightShader.id, TextFormat("maps[%i].texture", MATERIAL_MAP_METALNESS));
        lightShader.locs[RLG_LOC_MAP_NORMAL]         = rlGetLocationUniform(lightShader.id, TextFormat("maps[%i].texture", MATERIAL_MAP_NORMAL));
        lightShader.locs[RLG_LOC_MAP_ROUGHNESS]      = rlGetLocationUniform(lightShader.id, TextFormat("maps[%i].texture", MATERIAL_MAP_ROUGHNESS));
        lightShader.locs[RLG_LOC_MAP_OCCLUSION]      = rlGetLocationUniform(lightShader.id, TextFormat("maps[%i].texture", MATERIAL_MAP_OCCLUSION));
        lightShader.locs[RLG_LOC_MAP_EMISSION]       = rlGetLocationUniform(lightShader.id, TextFormat("maps[%i].texture", MATERIAL_MAP_EMISSION));
        lightShader.locs[RLG_LOC_MAP_HEIGHT]         = rlGetLocationUniform(lightShader.id, TextFormat("maps[%i].texture", MATERIAL_MAP_HEIGHT));
        lightShader.locs[RLG_LOC_MAP_BRDF]           = rlGetLocationUniform(lightShader.id, TextFormat("maps[%i].texture", MATERIAL_MAP_HEIGHT + 1));

        lightShader.locs[RLG_LOC_MAP_CUBEMAP]        = rlGetLocationUniform(lightShader.id, TextFormat("cubemaps[%i].texture", 0));
        lightShader.locs[RLG_LOC_MAP_IRRADIANCE]     = rlGetLocationUniform(lightShader.id, TextFormat("cubemaps[%i].texture", 1));
        lightShader.locs[RLG_LOC_MAP_PREFILTER]      = rlGetLocationUniform(lightShader.id, TextFormat("cubemaps[%i].texture", 2));

        lightShader.locs[RLG_LOC_METALNESS_SCALE]    = rlGetLocationUniform(lightShader.id, TextFormat("maps[%i].value", MATERIAL_MAP_METALNESS));
        lightShader.locs[RLG_LOC_ROUGHNESS_SCALE]    = rlGetLocationUniform(lightShader.id, TextFormat("maps[%i].value", MATERIAL_MAP_ROUGHNESS));
        lightShader.locs[RLG_LOC_AO_LIGHT_AFFECT]    = rlGetLocationUniform(lightShader.id, TextFormat("maps[%i].value", MATERIAL_MAP_OCCLUSION));
        lightShader.locs[RLG_LOC_HEIGHT_SCALE]       = rlGetLocationUniform(lightShader.id, TextFormat("maps[%i].value", MATERIAL_MAP_HEIGHT));

        // Definition of the lighting shader once initialization is successful
        rlgCtx->shaders[RLG_SHADER_MODEL] = lightShader;
    }

    // Init default view position and ambient color
    rlgCtx->colAmbient = INIT_STRUCT(Vector3, 0.1f, 0.1f, 0.1f);
    rlgCtx->viewPos = INIT_STRUCT_ZERO(Vector3);

    SetShaderValue(lightShader,
        lightShader.locs[RLG_LOC_COLOR_AMBIENT],
        &rlgCtx->colAmbient, RL_SHADER_UNIFORM_VEC3);

    // Retrieving lighting shader uniforms indicating which textures we should sample
    for (int i = 0, mapID = 0, cubemapID = 0; i < RLG_COUNT_MATERIAL_MAPS; i++)
    {
        if (i == MATERIAL_MAP_CUBEMAP || i == MATERIAL_MAP_IRRADIANCE || i == MATERIAL_MAP_PREFILTER)
        {
            rlgCtx->material.locs.useMaps[i] = rlGetLocationUniform(lightShader.id, TextFormat("cubemaps[%i].active", cubemapID));
            cubemapID++;
        }
        else
        {
            rlgCtx->material.locs.useMaps[i] = rlGetLocationUniform(lightShader.id, TextFormat("maps[%i].active", mapID));
            mapID++;
        }
    }

    // Default activation of diffuse texture sampling
    rlgCtx->material.data.useMaps[MATERIAL_MAP_ALBEDO] = true;
    SetShaderValue(lightShader, rlgCtx->material.locs.useMaps[MATERIAL_MAP_ALBEDO],
        &rlgCtx->material.data.useMaps[MATERIAL_MAP_ALBEDO], SHADER_UNIFORM_INT);

    // Recovery of “special” lighting shader uniforms
    rlgCtx->material.locs.parallaxMinLayers = rlGetLocationUniform(lightShader.id, "parallaxMinLayers");
    rlgCtx->material.locs.parallaxMaxLayers = rlGetLocationUniform(lightShader.id, "parallaxMaxLayers");
    rlgCtx->locLightingFar = rlGetLocationUniform(lightShader.id, "farPlane");

    // Allocation and initialization of the desired number of lights
    for (int i = 0; i < RLG_MAX_LIGHTS_PER_MATERIAL; i++)
    {
        struct RLG_Light *light = &rlgCtx->lights[i];

        light->data.shadowMap      = INIT_STRUCT_ZERO(struct RLG_ShadowMap);
        light->data.position       = INIT_STRUCT_ZERO(Vector3);
        light->data.direction      = INIT_STRUCT_ZERO(Vector3);
        light->data.color          = INIT_STRUCT(Vector3, 1.0f, 1.0f, 1.0f);
        light->data.energy         = 1.0f;
        light->data.specular       = 1.0f;
        light->data.size           = 0.0f;
        light->data.innerCutOff    = 180;
        light->data.outerCutOff    = 180;
        light->data.distance       = 8.0f;
        light->data.attenuation    = 1.0f;
        light->data.shadowMapTxlSz = 0.0f;
        light->data.depthBias      = 0.0f;
        light->data.type           = RLG_DIRLIGHT;
        light->data.shadow         = 0;
        light->data.enabled        = 0;

        light->locs.vpMatrix       = rlGetLocationUniform(lightShader.id, TextFormat("matLights[%i]", i));
        light->locs.shadowCubemap  = rlGetLocationUniform(lightShader.id, TextFormat("lights[%i].shadowCubemap", i));
        light->locs.shadowMap      = rlGetLocationUniform(lightShader.id, TextFormat("lights[%i].shadowMap", i));
        light->locs.position       = rlGetLocationUniform(lightShader.id, TextFormat("lights[%i].position", i));
        light->locs.direction      = rlGetLocationUniform(lightShader.id, TextFormat("lights[%i].direction", i));
        light->locs.color          = rlGetLocationUniform(lightShader.id, TextFormat("lights[%i].color", i));
        light->locs.energy         = rlGetLocationUniform(lightShader.id, TextFormat("lights[%i].energy", i));
        light->locs.specular       = rlGetLocationUniform(lightShader.id, TextFormat("lights[%i].specular", i));
        light->locs.size           = rlGetLocationUniform(lightShader.id, TextFormat("lights[%i].size", i));
        light->locs.innerCutOff    = rlGetLocationUniform(lightShader.id, TextFormat("lights[%i].innerCutOff", i));
        light->locs.outerCutOff    = rlGetLocationUniform(lightShader.id, TextFormat("lights[%i].outerCutOff", i));
        light->locs.distance       = rlGetLocationUniform(lightShader.id, TextFormat("lights[%i].distance", i));
        light->locs.attenuation    = rlGetLocationUniform(lightShader.id, TextFormat("lights[%i].attenuation", i));
        light->locs.shadowMapTxlSz = rlGetLocationUniform(lightShader.id, TextFormat("lights[%i].shadowMapTxlSz", i));
        light->locs.depthBias      = rlGetLocationUniform(lightShader.id, TextFormat("lights[%i].depthBias", i));
        light->locs.type           = rlGetLocationUniform(lightShader.id, TextFormat("lights[%i].type", i));
        light->locs.shadow         = rlGetLocationUniform(lightShader.id, TextFormat("lights[%i].shadow", i));
        light->locs.enabled        = rlGetLocationUniform(lightShader.id, TextFormat("lights[%i].enabled", i));

        SetShaderValue(lightShader, light->locs.color, &light->data.color, SHADER_UNIFORM_VEC3);
        SetShaderValue(lightShader, light->locs.energy, &light->data.energy, SHADER_UNIFORM_FLOAT);
        SetShaderValue(lightShader, light->locs.specular, &light->data.specular, SHADER_UNIFORM_FLOAT);
        SetShaderValue(lightShader, light->locs.innerCutOff, (float[1]) { -1 }, SHADER_UNIFORM_FLOAT);
        SetShaderValue(lightShader, light->locs.outerCutOff, (float[1]) { -1 }, SHADER_UNIFORM_FLOAT);
        SetShaderValue(lightShader, light->locs.distance, &light->data.distance, SHADER_UNIFORM_FLOAT);
        SetShaderValue(lightShader, light->locs.attenuation, &light->data.attenuation, SHADER_UNIFORM_FLOAT);
    }

    // Init default material maps
    Texture defaultTexture  = INIT_STRUCT_ZERO(Texture);
    defaultTexture.id       = rlGetTextureIdDefault();
    defaultTexture.width    = 1;
    defaultTexture.height   = 1;
    defaultTexture.mipmaps  = 1;
    defaultTexture.format   = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;

    rlgCtx->defaultMaps[MATERIAL_MAP_ALBEDO].texture = defaultTexture;
    rlgCtx->defaultMaps[MATERIAL_MAP_ALBEDO].color = WHITE;
    rlgCtx->defaultMaps[MATERIAL_MAP_METALNESS].texture = defaultTexture;
    rlgCtx->defaultMaps[MATERIAL_MAP_METALNESS].value = 0.5f;
    rlgCtx->defaultMaps[MATERIAL_MAP_ROUGHNESS].texture = defaultTexture;
    rlgCtx->defaultMaps[MATERIAL_MAP_ROUGHNESS].value = 0.5f;
    rlgCtx->defaultMaps[MATERIAL_MAP_OCCLUSION].texture = defaultTexture;
    rlgCtx->defaultMaps[MATERIAL_MAP_OCCLUSION].value = 0.0f;
    rlgCtx->defaultMaps[MATERIAL_MAP_EMISSION].texture = defaultTexture;
    rlgCtx->defaultMaps[MATERIAL_MAP_EMISSION].color = BLACK;
    rlgCtx->defaultMaps[MATERIAL_MAP_HEIGHT].texture = defaultTexture;
    rlgCtx->defaultMaps[MATERIAL_MAP_HEIGHT].value = 0.05f;

    // Load depth shader (used for shadow casting)
    rlgCtx->shaders[RLG_SHADER_DEPTH] = LoadShaderFromMemory(G_VS_CACHE_Depth, G_FS_CACHE_Depth);

    // Load depth cubemap shader (used for omnilight shadow casting)
    rlgCtx->shaders[RLG_SHADER_DEPTH_CUBEMAP] = LoadShaderFromMemory(G_VS_CACHE_DepthCubemap, G_FS_CACHE_DepthCubemap);
    rlgCtx->locDepthCubemapLightPos = rlGetLocationUniform(rlgCtx->shaders[RLG_SHADER_DEPTH_CUBEMAP].id, "lightPos");
    rlgCtx->locDepthCubemapFar = rlGetLocationUniform(rlgCtx->shaders[RLG_SHADER_DEPTH_CUBEMAP].id, "farPlane");

    // Get Near/Far render values
    rlgCtx->zNear = 0.01f;  // TODO: replace with rlGetCullDistanceNear()
    rlgCtx->zFar = 1000.0f; // TODO: replace with rlGetCullDistanceFar()

    // Send Near/Far to shaders who need it
    SetShaderValue(rlgCtx->shaders[RLG_SHADER_DEPTH_CUBEMAP],
        rlgCtx->locDepthCubemapFar, &rlgCtx->zFar, SHADER_UNIFORM_FLOAT);

    // Load equirectangular to cubemap shader (used for skybox cubemap generation)
    rlgCtx->shaders[RLG_SHADER_EQUIRECTANGULAR_TO_CUBEMAP] = LoadShaderFromMemory(
        G_VS_CACHE_EquirectangularToCubemap, G_FS_CACHE_EquirectangularToCubemap);

    // Load irradiance convolution shader (used to generate irradiance map of the skybox cubemap)
    rlgCtx->shaders[RLG_SHADER_IRRADIANCE_CONVOLUTION] = LoadShaderFromMemory(
        G_VS_CACHE_IrradianceConvolution, G_FS_CACHE_IrradianceConvolution);

    // Load skybox shader
    rlgCtx->shaders[RLG_SHADER_SKYBOX] = LoadShaderFromMemory(G_VS_CACHE_Skybox, G_FS_CACHE_Skybox);
    rlgCtx->skybox.locDoGamma = rlGetLocationUniform(rlgCtx->shaders[RLG_SHADER_SKYBOX].id, "doGamma");

    // Load skybox vertex array
    // Define the positions of the vertices for a cube
    static const float skyboxPositions[] =
    {
        // Front face
        -0.5f, -0.5f,  0.5f,    // Vertex 0
         0.5f, -0.5f,  0.5f,    // Vertex 1
         0.5f,  0.5f,  0.5f,    // Vertex 2
        -0.5f,  0.5f,  0.5f,    // Vertex 3

        // Back face
        -0.5f, -0.5f, -0.5f,    // Vertex 4
         0.5f, -0.5f, -0.5f,    // Vertex 5
         0.5f,  0.5f, -0.5f,    // Vertex 6
        -0.5f,  0.5f, -0.5f     // Vertex 7
    };

    // Define the indices for drawing the cube faces
    const unsigned short skyboxIndices[] =
    {
        // Front face
        0, 1, 2,
        2, 3, 0,

        // Right face
        1, 5, 6,
        6, 2, 1,

        // Back face
        5, 4, 7,
        7, 6, 5,

        // Left face
        4, 0, 3,
        3, 7, 4,

        // Top face
        3, 2, 6,
        6, 7, 3,

        // Bottom face
        4, 5, 1,
        1, 0, 4
    };

    // Load vertex array object (VAO) and bind it
    rlgCtx->skybox.vao = rlLoadVertexArray();
    rlEnableVertexArray(rlgCtx->skybox.vao);
    {
        // Load vertex buffer object (VBO) for positions and bind it
        rlgCtx->skybox.vbo = rlLoadVertexBuffer(skyboxPositions, sizeof(skyboxPositions), false);
        rlSetVertexAttribute(0, 3, RL_FLOAT, 0, 0, 0);
        rlEnableVertexAttribute(0);

        // Load element buffer object (EBO) for indices and bind it
        rlgCtx->skybox.ebo = rlLoadVertexBufferElement(skyboxIndices, sizeof(skyboxIndices), false);
    }
    rlDisableVertexArray();

    return (RLG_Context)rlgCtx;
}

void RLG_DestroyContext(RLG_Context ctx)
{
    struct RLG_Core *pCtx = (struct RLG_Core*)ctx;

    rlUnloadVertexBuffer(rlgCtx->skybox.ebo);
    rlUnloadVertexBuffer(rlgCtx->skybox.vbo);
    rlUnloadVertexArray(rlgCtx->skybox.vao);

    for (int i = 0; i < RLG_COUNT_SHADERS; i++)
    {
        if (IsShaderValid(pCtx->shaders[i]))
        {
            UnloadShader(pCtx->shaders[i]);
            pCtx->shaders[i] = INIT_STRUCT_ZERO(Shader);
        }
    }

    for (int i = 0; i < RLG_MAX_LIGHTS_PER_MATERIAL; i++)
    {
        struct RLG_Light *light = &pCtx->lights[i];

        if (light->data.shadowMap.id != 0)
        {
            rlUnloadTexture(light->data.shadowMap.depth.id);
            rlUnloadFramebuffer(light->data.shadowMap.id);
        }
    }
}

void RLG_SetContext(RLG_Context ctx)
{
    rlgCtx = (struct RLG_Core*)ctx;
}

RLG_Context RLG_GetContext(void)
{
    return (RLG_Context)rlgCtx;
}

void RLG_SetCustomShaderCode(RLG_Shader shader, const char *vsCode, const char *fsCode)
{
    switch (shader)
    {
        case RLG_SHADER_MODEL:
            G_FS_CACHE_Model = vsCode;
            G_VS_CACHE_Model = fsCode;
            break;

        case RLG_SHADER_DEPTH:
            G_VS_CACHE_Depth = vsCode;
            G_FS_CACHE_Depth = fsCode;
            break;

        case RLG_SHADER_DEPTH_CUBEMAP:
            G_VS_CACHE_DepthCubemap = vsCode;
            G_FS_CACHE_DepthCubemap = fsCode;
            break;

        case RLG_SHADER_EQUIRECTANGULAR_TO_CUBEMAP:
            G_VS_CACHE_EquirectangularToCubemap = vsCode;
            G_FS_CACHE_EquirectangularToCubemap = fsCode;
            break;

        case RLG_SHADER_IRRADIANCE_CONVOLUTION:
            G_VS_CACHE_IrradianceConvolution = vsCode;
            G_FS_CACHE_IrradianceConvolution = fsCode;
            break;

        case RLG_SHADER_SKYBOX:
            G_VS_CACHE_Skybox = vsCode;
            G_FS_CACHE_Skybox = fsCode;
            break;

        default:
            TraceLog(LOG_WARNING, "Unsupported 'shader' passed to 'RLG_SetCustomShader'");
            break;
    }
}

const Shader* RLG_GetShader(RLG_Shader shader)
{
    if (shader < 0 || shader >= RLG_COUNT_SHADERS)
    {
        return NULL;
    }

    return &rlgCtx->shaders[shader];
}

void RLG_SetViewPosition(float x, float y, float z)
{
    RLG_SetViewPositionV(INIT_STRUCT(Vector3, x, y, z));
}

void RLG_SetViewPositionV(Vector3 position)
{
    int loc = rlgCtx->shaders[RLG_SHADER_MODEL].locs[RLG_LOC_VECTOR_VIEW];

    if (loc != -1)
    {
        rlgCtx->viewPos = position;
        SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL],
            loc, &rlgCtx->viewPos, SHADER_UNIFORM_VEC3);
    }

}

Vector3 RLG_GetViewPosition(void)
{
    return rlgCtx->viewPos;
}

void RLG_SetAmbientColor(Color color)
{
    int loc = rlgCtx->shaders[RLG_SHADER_MODEL].locs[RLG_LOC_COLOR_AMBIENT];

    if (loc != -1)
    {
        rlgCtx->colAmbient.x = (float)color.r/255.0f;
        rlgCtx->colAmbient.y = (float)color.g/255.0f;
        rlgCtx->colAmbient.z = (float)color.b/255.0f;

        SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL],
            loc, &rlgCtx->colAmbient, SHADER_UNIFORM_VEC3);
    }
}
Color RLG_GetAmbientColor(void)
{
    Color color = { 0 };

    color.r = rlgCtx->colAmbient.x*255.0f;
    color.g = rlgCtx->colAmbient.y*255.0f;
    color.b = rlgCtx->colAmbient.z*255.0f;
    color.a = 255;

    return color;
}

void RLG_SetParallaxLayers(int min, int max)
{
    if (rlgCtx->material.locs.parallaxMinLayers != -1 &&
        min != rlgCtx->material.data.parallaxMinLayers)
    {
        rlgCtx->material.data.parallaxMinLayers = min;
        SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL],
            rlgCtx->material.locs.parallaxMinLayers,
            &min, RL_SHADER_UNIFORM_INT);
    }

    if (rlgCtx->material.locs.parallaxMaxLayers != -1 &&
        max != rlgCtx->material.data.parallaxMaxLayers)
    {
        rlgCtx->material.data.parallaxMaxLayers = max;
        SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL],
            rlgCtx->material.locs.parallaxMaxLayers,
            &max, RL_SHADER_UNIFORM_INT);
    }
}

void RLG_GetParallaxLayers(int* min, int* max)
{
    if (min != NULL) *min = rlgCtx->material.data.parallaxMinLayers;
    if (max != NULL) *max = rlgCtx->material.data.parallaxMaxLayers;
}

void RLG_UseMap(MaterialMapIndex mapIndex, bool active)
{
    if (mapIndex >= 0 && mapIndex < RLG_COUNT_MATERIAL_MAPS)
    {
        if (rlgCtx->material.locs.useMaps[mapIndex] != -1 &&
            active != rlgCtx->material.data.useMaps[mapIndex])
        {
            int v = (int)active;
            rlgCtx->material.data.useMaps[mapIndex] = active;
            SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], rlgCtx->material.locs.useMaps[mapIndex], &v, SHADER_UNIFORM_INT);
        }
    }
}

bool RLG_IsMapUsed(MaterialMapIndex mapIndex)
{
    bool result = false;

    if (mapIndex >= 0 && mapIndex < RLG_COUNT_MATERIAL_MAPS)
    {
        result = rlgCtx->material.data.useMaps[mapIndex];
    }

    return result;
}

void RLG_UseDefaultMap(MaterialMapIndex mapIndex, bool active)
{
    if (mapIndex >= 0 || mapIndex < RLG_COUNT_MATERIAL_MAPS)
    {
        rlgCtx->usedDefaultMaps[mapIndex] = active;
    }
}

void RLG_SetDefaultMap(MaterialMapIndex mapIndex, MaterialMap map)
{
    if (mapIndex >= 0 || mapIndex < RLG_COUNT_MATERIAL_MAPS)
    {
        rlgCtx->defaultMaps[mapIndex] = map;
    }
}

MaterialMap RLG_GetDefaultMap(MaterialMapIndex mapIndex)
{
    MaterialMap map = { 0 };

    if (mapIndex >= 0 || mapIndex < RLG_COUNT_MATERIAL_MAPS)
    {
        map = rlgCtx->defaultMaps[mapIndex];
    }

    return map;
}

bool RLG_IsDefaultMapUsed(MaterialMapIndex mapIndex)
{
    return rlgCtx->usedDefaultMaps[mapIndex];
}

void RLG_UseLight(unsigned int light, bool active)
{
    if (light >= RLG_MAX_LIGHTS_PER_MATERIAL)
    {
        TraceLog(LOG_ERROR, "Light [ID %i] specified to 'RLG_UseLight' exceeds allocated number [MAX %i]", light, RLG_MAX_LIGHTS_PER_MATERIAL);
        return;
    }

    struct RLG_Light *l = &rlgCtx->lights[light];

    if (active != l->data.enabled)
    {
        l->data.enabled = (int)active;
        SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL],
            l->locs.enabled, &l->data.enabled, SHADER_UNIFORM_INT);
    }
}

bool RLG_IsLightUsed(unsigned int light)
{
    if (light >= RLG_MAX_LIGHTS_PER_MATERIAL)
    {
        TraceLog(LOG_ERROR, "Light [ID %i] specified to 'RLG_IsLightUsed' exceeds allocated number [MAX %i]", light, RLG_MAX_LIGHTS_PER_MATERIAL);
        return false;
    }

    return (bool)rlgCtx->lights[light].data.enabled;
}

void RLG_ToggleLight(unsigned int light)
{
    if (light >= RLG_MAX_LIGHTS_PER_MATERIAL)
    {
        TraceLog(LOG_ERROR, "Light [ID %i] specified to 'RLG_ToggleLight' exceeds allocated number [MAX %i]", light, RLG_MAX_LIGHTS_PER_MATERIAL);
        return;
    }

    struct RLG_Light *l = &rlgCtx->lights[light];

    l->data.enabled = !l->data.enabled;
    SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.enabled,
        &l->data.enabled, SHADER_UNIFORM_INT);
}

void RLG_SetLightType(unsigned int light, RLG_LightType type)
{
    if (light >= RLG_MAX_LIGHTS_PER_MATERIAL)
    {
        TraceLog(LOG_ERROR, "Light [ID %i] specified to 'RLG_SetLightType' exceeds allocated number [MAX %i]", light, RLG_MAX_LIGHTS_PER_MATERIAL);
        return;
    }

    struct RLG_Light *l = &rlgCtx->lights[light];

    if (l->data.type != type)
    {
        if (l->data.shadow)
        {
            int shadowMapResolution = l->data.shadowMap.width;

            RLG_DisableShadow(light);
            RLG_EnableShadow(light, shadowMapResolution);
        }

        l->data.type = (int)type;
        SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.type,
            &l->data.type, SHADER_UNIFORM_INT);
    }
}

RLG_LightType RLG_GetLightType(unsigned int light)
{
    if (light >= RLG_MAX_LIGHTS_PER_MATERIAL)
    {
        TraceLog(LOG_ERROR, "Light [ID %i] specified to 'RLG_GetLightType' exceeds allocated number [MAX %i]", light, RLG_MAX_LIGHTS_PER_MATERIAL);
        return (RLG_LightType)0;
    }

    return (RLG_LightType)rlgCtx->lights[light].data.type;
}

void RLG_SetLightValue(unsigned int light, RLG_LightProperty property, float value)
{
    if (light >= RLG_MAX_LIGHTS_PER_MATERIAL)
    {
        TraceLog(LOG_ERROR, "Light [ID %i] specified to 'RLG_SetLightValue' exceeds allocated number [MAX %i]", light, RLG_MAX_LIGHTS_PER_MATERIAL);
        return;
    }

    struct RLG_Light *l = &rlgCtx->lights[light];

    switch (property)
    {
        case RLG_LIGHT_COLOR:
            l->data.color = INIT_STRUCT(Vector3, value, value, value);
            SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.color,
                &l->data.color, SHADER_UNIFORM_VEC3);
            break;

        case RLG_LIGHT_ENERGY:
            if (value != l->data.energy)
            {
                l->data.energy = value;
                SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.energy,
                    &l->data.energy, SHADER_UNIFORM_FLOAT);
            }
            break;

        case RLG_LIGHT_SPECULAR:
            if (value != l->data.specular)
            {
                l->data.specular = value;
                SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.specular,
                    &l->data.specular, SHADER_UNIFORM_FLOAT);
            }
            break;

        case RLG_LIGHT_SIZE:
            if (value != l->data.size)
            {
                l->data.size = value;
                SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.size,
                    &l->data.size, SHADER_UNIFORM_FLOAT);
            }
            break;

        case RLG_LIGHT_INNER_CUTOFF:
            if (value != l->data.innerCutOff)
            {
                l->data.innerCutOff = value;
                float c = cosf(value*DEG2RAD);
                SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.innerCutOff,
                    &c, SHADER_UNIFORM_FLOAT);
            }
            break;

        case RLG_LIGHT_OUTER_CUTOFF:
            if (value != l->data.outerCutOff)
            {
                l->data.outerCutOff = value;
                float c = cosf(value*DEG2RAD);
                SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.outerCutOff,
                    &c, SHADER_UNIFORM_FLOAT);
            }
            break;

        case RLG_LIGHT_DISTANCE:
            if (value != l->data.distance)
            {
                l->data.distance = value;
                SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.distance, &value, SHADER_UNIFORM_FLOAT);
            }
            break;

        case RLG_LIGHT_ATTENUATION:
            if (value != l->data.attenuation)
            {
                l->data.attenuation = value;
                SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.attenuation, &value, SHADER_UNIFORM_FLOAT);
            }
            break;

        default:
            break;
    }
}

void RLG_SetLightXYZ(unsigned int light, RLG_LightProperty property, float x, float y, float z)
{
    if (light >= RLG_MAX_LIGHTS_PER_MATERIAL)
    {
        TraceLog(LOG_ERROR, "Light [ID %i] specified to 'RLG_SetLightXYZ' exceeds allocated number [MAX %i]", light, RLG_MAX_LIGHTS_PER_MATERIAL);
        return;
    }

    struct RLG_Light *l = &rlgCtx->lights[light];
    Vector3 value = { x, y, z };

    switch (property)
    {
        case RLG_LIGHT_POSITION:
            l->data.position = value;
            SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.position,
                &value, SHADER_UNIFORM_VEC3);
            break;

        case RLG_LIGHT_DIRECTION:
            l->data.direction = value;
            SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.direction,
                &value, SHADER_UNIFORM_VEC3);
            break;

        case RLG_LIGHT_COLOR:
            l->data.color = value;
            SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.color,
                &value, SHADER_UNIFORM_VEC3);
            break;

        default:
            break;
    }
}

void RLG_SetLightVec3(unsigned int light, RLG_LightProperty property, Vector3 value)
{
    if (light >= RLG_MAX_LIGHTS_PER_MATERIAL)
    {
        TraceLog(LOG_ERROR, "Light [ID %i] specified to 'RLG_SetLightVec3' exceeds allocated number [MAX %i]", light, RLG_MAX_LIGHTS_PER_MATERIAL);
        return;
    }

    struct RLG_Light *l = &rlgCtx->lights[light];

    switch (property)
    {
        case RLG_LIGHT_POSITION:
            l->data.position = value;
            SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.position,
                &value, SHADER_UNIFORM_VEC3);
            break;

        case RLG_LIGHT_DIRECTION:
            l->data.direction = value;
            SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.direction,
                &value, SHADER_UNIFORM_VEC3);
            break;

        case RLG_LIGHT_COLOR:
            l->data.color = value;
            SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.color,
                &value, SHADER_UNIFORM_VEC3);
            break;

        default:
            break;
    }
}

void RLG_SetLightColor(unsigned int light, Color color)
{
    if (light >= RLG_MAX_LIGHTS_PER_MATERIAL)
    {
        TraceLog(LOG_ERROR, "Light [ID %i] specified to 'RLG_SetLightColor' exceeds allocated number [MAX %i]", light, RLG_MAX_LIGHTS_PER_MATERIAL);
        return;
    }

    Vector3 nCol = { 0 };
    nCol.x = color.r/255.0f;
    nCol.y = color.g/255.0f;
    nCol.z = color.b/255.0f;

    struct RLG_Light *l = &rlgCtx->lights[light];

    l->data.color = nCol;
    SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.color,
        &nCol, SHADER_UNIFORM_VEC3);
}

float RLG_GetLightValue(unsigned int light, RLG_LightProperty property)
{
    float result = 0;

    if (light >= RLG_MAX_LIGHTS_PER_MATERIAL)
    {
        TraceLog(LOG_ERROR, "Light [ID %i] specified to 'RLG_GetLightValue' exceeds allocated number [MAX %i]", light, RLG_MAX_LIGHTS_PER_MATERIAL);
        return result;
    }

    const struct RLG_Light *l = &rlgCtx->lights[light];

    switch (property)
    {
        case RLG_LIGHT_ENERGY:
            result = l->data.energy;
            break;

        case RLG_LIGHT_SPECULAR:
            result = l->data.specular;
            break;

        case RLG_LIGHT_SIZE:
            result = l->data.size;
            break;

        case RLG_LIGHT_INNER_CUTOFF:
            result = l->data.innerCutOff;
            break;

        case RLG_LIGHT_OUTER_CUTOFF:
            result = l->data.outerCutOff;
            break;

        case RLG_LIGHT_DISTANCE:
            result = l->data.distance;
            break;

        case RLG_LIGHT_ATTENUATION:
            result = l->data.attenuation;
            break;

        default:
            break;
    }

    return result;
}

Vector3 RLG_GetLightVec3(unsigned int light, RLG_LightProperty property)
{
    Vector3 result = { 0 };

    if (light >= RLG_MAX_LIGHTS_PER_MATERIAL)
    {
        TraceLog(LOG_ERROR, "Light [ID %i] specified to 'RLG_GetLightVec3' exceeds allocated number [MAX %i]", light, RLG_MAX_LIGHTS_PER_MATERIAL);
        return result;
    }

    const struct RLG_Light *l = &rlgCtx->lights[light];

    switch (property)
    {
        case RLG_LIGHT_POSITION:
            result = l->data.position;
            break;

        case RLG_LIGHT_DIRECTION:
            result = l->data.direction;
            break;

        case RLG_LIGHT_COLOR:
            result = l->data.color;
            break;

        default:
            break;
    }

    return result;
}

Color RLG_GetLightColor(unsigned int light)
{
    Color result = BLACK;

    if (light >= RLG_MAX_LIGHTS_PER_MATERIAL)
    {
        TraceLog(LOG_ERROR, "Light [ID %i] specified to 'RLG_GetLightColor' exceeds allocated number [MAX %i]", light, RLG_MAX_LIGHTS_PER_MATERIAL);
        return result;
    }

    const struct RLG_Light *l = &rlgCtx->lights[light];

    result.r = 255*l->data.color.x;
    result.g = 255*l->data.color.y;
    result.b = 255*l->data.color.z;

    return result;
}

void RLG_LightTranslate(unsigned int light, float x, float y, float z)
{
    if (light >= RLG_MAX_LIGHTS_PER_MATERIAL)
    {
        TraceLog(LOG_ERROR, "Light [ID %i] specified to 'RLG_LightTranslate' exceeds allocated number [MAX %i]", light, RLG_MAX_LIGHTS_PER_MATERIAL);
        return;
    }

    struct RLG_Light *l = &rlgCtx->lights[light];

    l->data.position.x += x;
    l->data.position.y += y;
    l->data.position.z += z;

    SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.position,
        &l->data.position, SHADER_UNIFORM_VEC3);
}

void RLG_LightTranslateV(unsigned int light, Vector3 v)
{
    if (light >= RLG_MAX_LIGHTS_PER_MATERIAL)
    {
        TraceLog(LOG_ERROR, "Light [ID %i] specified to 'RLG_LightTranslateV' exceeds allocated number [MAX %i]", light, RLG_MAX_LIGHTS_PER_MATERIAL);
        return;
    }

    struct RLG_Light *l = &rlgCtx->lights[light];
    l->data.position.x += v.x;
    l->data.position.y += v.y;
    l->data.position.z += v.z;

    SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.position,
        &l->data.position, SHADER_UNIFORM_VEC3);
}

void RLG_LightRotateX(unsigned int light, float degrees)
{
    if (light >= RLG_MAX_LIGHTS_PER_MATERIAL)
    {
        TraceLog(LOG_ERROR, "Light [ID %i] specified to 'RLG_LightRotateX' exceeds allocated number [MAX %i]", light, RLG_MAX_LIGHTS_PER_MATERIAL);
        return;
    }

    struct RLG_Light *l = &rlgCtx->lights[light];
    float radians = DEG2RAD*degrees;
    float c = cosf(radians);
    float s = sinf(radians);

    l->data.direction.y = l->data.direction.y*c + l->data.direction.z*s;
    l->data.direction.z = -l->data.direction.y*s + l->data.direction.z*c;

    SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.direction,
        &l->data.direction, SHADER_UNIFORM_VEC3);
}

void RLG_LightRotateY(unsigned int light, float degrees)
{
    if (light >= RLG_MAX_LIGHTS_PER_MATERIAL)
    {
        TraceLog(LOG_ERROR, "Light [ID %i] specified to 'RLG_LightRotateY' exceeds allocated number [MAX %i]", light, RLG_MAX_LIGHTS_PER_MATERIAL);
        return;
    }

    struct RLG_Light *l = &rlgCtx->lights[light];
    float radians = DEG2RAD*degrees;
    float c = cosf(radians);
    float s = sinf(radians);

    l->data.direction.x = l->data.direction.x*c - l->data.direction.z*s;
    l->data.direction.z = l->data.direction.x*s + l->data.direction.z*c;

    SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.direction,
        &l->data.direction, SHADER_UNIFORM_VEC3);
}

void RLG_LightRotateZ(unsigned int light, float degrees)
{
    if (light >= RLG_MAX_LIGHTS_PER_MATERIAL)
    {
        TraceLog(LOG_ERROR, "Light [ID %i] specified to 'RLG_LightRotateZ' exceeds allocated number [MAX %i]", light, RLG_MAX_LIGHTS_PER_MATERIAL);
        return;
    }

    struct RLG_Light *l = &rlgCtx->lights[light];
    float radians = DEG2RAD*degrees;
    float c = cosf(radians);
    float s = sinf(radians);

    l->data.direction.x = l->data.direction.x*c + l->data.direction.y*s;
    l->data.direction.y = -l->data.direction.x*s + l->data.direction.y*c;

    SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.direction,
        &l->data.direction, SHADER_UNIFORM_VEC3);
}

void RLG_LightRotate(unsigned int light, Vector3 axis, float degrees)
{
    if (light >= RLG_MAX_LIGHTS_PER_MATERIAL)
    {
        TraceLog(LOG_ERROR, "Light [ID %i] specified to 'RLG_LightRotate' exceeds allocated number [MAX %i]", light, RLG_MAX_LIGHTS_PER_MATERIAL);
        return;
    }

    struct RLG_Light *l = &rlgCtx->lights[light];
    float radians = -DEG2RAD*degrees;
    float halfTheta = radians*0.5f;

    float sinHalfTheta = sinf(halfTheta);
    Quaternion rotationQuat = {
        axis.x * sinHalfTheta,
        axis.y * sinHalfTheta,
        axis.z * sinHalfTheta,
        cosf(halfTheta)
    };

    // Convert the current direction vector to a quaternion
    Quaternion directionQuat = {
        l->data.direction.x,
        l->data.direction.y,
        l->data.direction.z,
        0.0f
    };

    // Calculate the rotated direction quaternion
    Quaternion rotatedQuat = QuaternionMultiply(
        QuaternionMultiply(rotationQuat, directionQuat),
        QuaternionInvert(rotationQuat));

    // Update the light direction with the rotated direction
    l->data.direction = Vector3Normalize(INIT_STRUCT(Vector3,
        rotatedQuat.x, rotatedQuat.y, rotatedQuat.z
    ));

    SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.direction,
        &l->data.direction, SHADER_UNIFORM_VEC3);
}

void RLG_SetLightTarget(unsigned int light, float x, float y, float z)
{
    RLG_SetLightTargetV(light, INIT_STRUCT(Vector3, x, y, z));
}

void RLG_SetLightTargetV(unsigned int light, Vector3 targetPosition)
{
    if (light >= RLG_MAX_LIGHTS_PER_MATERIAL)
    {
        TraceLog(LOG_ERROR, "Light [ID %i] specified to 'RLG_SetLightTarget' exceeds allocated number [MAX %i]", light, RLG_MAX_LIGHTS_PER_MATERIAL);
        return;
    }

    struct RLG_Light *l = &rlgCtx->lights[light];

    l->data.direction = Vector3Normalize(Vector3Subtract(
        targetPosition, l->data.position));

    SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.direction,
        &l->data.direction, SHADER_UNIFORM_VEC3);
}

Vector3 RLG_GetLightTarget(unsigned int light)
{
    Vector3 result = INIT_STRUCT_ZERO(Vector3);

    if (light >= RLG_MAX_LIGHTS_PER_MATERIAL)
    {
        TraceLog(LOG_ERROR, "Light [ID %i] specified to 'RLG_GetLightTarget' exceeds allocated number [MAX %i]", light, RLG_MAX_LIGHTS_PER_MATERIAL);
        return result;
    }

    struct RLG_Light *l = &rlgCtx->lights[light];
    result = Vector3Add(l->data.position, l->data.direction);

    return result;
}

void RLG_EnableShadow(unsigned int light, int shadowMapResolution)
{
    // Check if the specified light ID is within the valid range
    if (light >= RLG_MAX_LIGHTS_PER_MATERIAL)
    {
        TraceLog(LOG_ERROR, "Light [ID %i] specified to 'RLG_EnableShadow' exceeds allocated number [MAX %i]", light, RLG_MAX_LIGHTS_PER_MATERIAL);
        return;
    }

    // Get a pointer to the specified light structure
    struct RLG_Light *l = &rlgCtx->lights[light];

    // Check if the current shadow map resolution is different from the desired resolution
    if (l->data.shadowMap.width != shadowMapResolution)
    {
        // If the shadow map is already initialized, unload the existing texture and framebuffer
        if (l->data.shadowMap.id != 0)
        {
            rlUnloadTexture(l->data.shadowMap.depth.id);
            rlUnloadFramebuffer(l->data.shadowMap.id);
        }

        // Get a pointer to the shadow map structure of the light
        struct RLG_ShadowMap *sm = &l->data.shadowMap;

        // If the light is an omnidirectional light, set up a cube map for shadows
        if (l->data.type == RLG_OMNILIGHT)
        {
            glGenTextures(1, &sm->depth.id);
            glBindTexture(GL_TEXTURE_CUBE_MAP, sm->depth.id);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
            for (int i = 0; i < 6; ++i)
            {
                glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_DEPTH_COMPONENT,
                    shadowMapResolution, shadowMapResolution, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
            }
            glBindTexture(GL_TEXTURE_CUBE_MAP, 0);

            glGenFramebuffers(1, &sm->id);
            glBindFramebuffer(GL_FRAMEBUFFER, sm->id);
            glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, sm->depth.id, 0);
            glDrawBuffer(GL_NONE);
            glReadBuffer(GL_NONE);

            // Check if the framebuffer is complete
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            {
                // Log an error if the framebuffer is not complete
                TraceLog(LOG_ERROR, "Framebuffer is not complete for omnidirectional shadow map");
            }

            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            // Configure the shadow map parameters
            sm->depth.width = sm->depth.height = shadowMapResolution;
            sm->width = sm->height = shadowMapResolution;
            sm->depth.format = 19, sm->depth.mipmaps = 1;
        }
        else
        {
            // Set up a 2D texture for shadow map for other light types
            sm->id = rlLoadFramebuffer();
            sm->width = sm->height = shadowMapResolution;
            rlEnableFramebuffer(sm->id);

            sm->depth.id = rlLoadTextureDepth(shadowMapResolution, shadowMapResolution, false);
            sm->depth.width = sm->depth.height = shadowMapResolution;
            sm->depth.format = 19, sm->depth.mipmaps = 1;

            rlTextureParameters(sm->depth.id, RL_TEXTURE_WRAP_S, RL_TEXTURE_WRAP_CLAMP);
            rlTextureParameters(sm->depth.id, RL_TEXTURE_WRAP_T, RL_TEXTURE_WRAP_CLAMP);
            rlFramebufferAttach(sm->id, sm->depth.id, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_TEXTURE2D, 0);
        }

        // REVIEW: Should this value be modifiable by the user?
        float texelSize = 1.0f/shadowMapResolution;
        SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.shadowMapTxlSz,
            &texelSize, SHADER_UNIFORM_FLOAT);

        // Set the depth bias value based on the light type
        l->data.depthBias = (l->data.type == RLG_OMNILIGHT) ? 0.05f : 0.0002f;
        SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.depthBias,
            &l->data.depthBias, SHADER_UNIFORM_FLOAT);
    }

    // Enable shadows for the light and send the information to the shader
    l->data.shadow = true;
    SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.shadow,
        &l->data.shadow, SHADER_UNIFORM_INT);
}

void RLG_DisableShadow(unsigned int light)
{
    if (light >= RLG_MAX_LIGHTS_PER_MATERIAL)
    {
        TraceLog(LOG_ERROR, "Light [ID %i] specified to 'RLG_DisableShadow' exceeds allocated number [MAX %i]", light, RLG_MAX_LIGHTS_PER_MATERIAL);
        return;
    }

    struct RLG_Light *l = &rlgCtx->lights[light];

    if (l->data.shadow)
    {
        // Unload depth texture and framebuffer
        rlUnloadTexture(l->data.shadowMap.depth.id);
        rlUnloadFramebuffer(l->data.shadowMap.id);

        // Fill shadow map struct with zeroes
        l->data.shadowMap = INIT_STRUCT_ZERO(struct RLG_ShadowMap);

        // Send info to the shader
        l->data.shadow = false;
        SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.shadow,
            &l->data.shadow, SHADER_UNIFORM_INT);
    }
}

bool RLG_IsShadowEnabled(unsigned int light)
{
    if (light >= RLG_MAX_LIGHTS_PER_MATERIAL)
    {
        TraceLog(LOG_ERROR, "Light [ID %i] specified to 'RLG_IsShadowEnabled' exceeds allocated number [MAX %i]", light, RLG_MAX_LIGHTS_PER_MATERIAL);
        return false;
    }

    return rlgCtx->lights[light].data.shadow;
}

void RLG_SetShadowBias(unsigned int light, float value)
{
    if (light >= RLG_MAX_LIGHTS_PER_MATERIAL)
    {
        TraceLog(LOG_ERROR, "Light [ID %i] specified to 'RLG_SetShadowBias' exceeds allocated number [MAX %i]", light, RLG_MAX_LIGHTS_PER_MATERIAL);
        return;
    }

    struct RLG_Light *l = &rlgCtx->lights[light];

    l->data.depthBias = value;
    SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.depthBias,
        &value, SHADER_UNIFORM_FLOAT);
}

float RLG_GetShadowBias(unsigned int light)
{
    if (light >= RLG_MAX_LIGHTS_PER_MATERIAL)
    {
        TraceLog(LOG_ERROR, "Light [ID %i] specified to 'RLG_GetShadowBias' exceeds allocated number [MAX %i]", light, RLG_MAX_LIGHTS_PER_MATERIAL);
        return 0;
    }

    return rlgCtx->lights[light].data.depthBias;
}

void RLG_UpdateShadowMap(unsigned int light, RLG_DrawFunc drawFunc)
{
    // Directions and up vectors for the 6 faces of the cubemap
    static const Vector3 dirs[6] = {
        {  1.0,  0.0,  0.0 }, // +X
        { -1.0,  0.0,  0.0 }, // -X
        {  0.0,  1.0,  0.0 }, // +Y
        {  0.0, -1.0,  0.0 }, // -Y
        {  0.0,  0.0,  1.0 }, // +Z
        {  0.0,  0.0, -1.0 }  // -Z
    };

    static const Vector3 ups[6] = {
        {  0.0, -1.0,  0.0 }, // +X
        {  0.0, -1.0,  0.0 }, // -X
        {  0.0,  0.0,  1.0 }, // +Y
        {  0.0,  0.0, -1.0 }, // -Y
        {  0.0, -1.0,  0.0 }, // +Z
        {  0.0, -1.0,  0.0 }  // -Z
    };

    // Safety checks
    if (!drawFunc)
    {
        // Log an error if the draw function pointer is NULL
        TraceLog(LOG_ERROR, "The drawing function pointer specified to 'RLG_UpdateShadowMap' is NULL");
        return;  
    }

    if (light >= RLG_MAX_LIGHTS_PER_MATERIAL)
    {
        // Log an error if the light ID exceeds the number of allocated lights
        TraceLog(LOG_ERROR, "Light [ID %i] specified to 'RLG_UpdateShadowMap' exceeds allocated number [MAX %i]", light, RLG_MAX_LIGHTS_PER_MATERIAL);
        return;
    }

    // Get a pointer to the specified light structure
    struct RLG_Light *l = &rlgCtx->lights[light];
    if (!l->data.shadow)
    {
        TraceLog(LOG_ERROR, "Light [ID %i] does not support shadow casting", light);
        return;
    }

    // Flush the rendering batch and enable the shadow map framebuffer
    rlDrawRenderBatchActive();
    rlEnableFramebuffer(l->data.shadowMap.id);

    // Configure the projection for the shadow map
    rlViewport(0, 0, l->data.shadowMap.width, l->data.shadowMap.height);
    rlMatrixMode(RL_PROJECTION);
    rlPushMatrix();
    rlLoadIdentity();

    // Near and far clipping planes for shadow map rendering
    rlgCtx->zNear = 0.01f;      // TODO: replace with rlGetCullDistanceNear()
    rlgCtx->zFar = 1000.0f;     // TODO: replace with rlGetCullDistanceFar()

    // Set up projection matrix based on the light type
    switch (l->data.type)
    {
        case RLG_DIRLIGHT:
            // Orthographic projection for directional
            rlOrtho(-10.0, 10.0, -10.0, 10.0, rlgCtx->zNear, rlgCtx->zFar);
            break;
        case RLG_SPOTLIGHT:
            // Perspective projection for spotlight
            rlMultMatrixf(MatrixToFloat(MatrixPerspective(90*DEG2RAD, 1.0, 0.01, l->data.distance)));
            break;
        case RLG_OMNILIGHT:
            // Perspective projection for omnidirectional light
            rlMultMatrixf(MatrixToFloat(MatrixPerspective(90*DEG2RAD, 1.0, 0.01, l->data.distance)));
            break;
    }

    // Switch to modelview matrix mode
    rlMatrixMode(RL_MODELVIEW);

    // Enable depth test and disable color blending
    rlEnableDepthTest();
    rlDisableColorBlend();

    // Select the appropriate depth shader
    Shader shader = { 0 };
    if (l->data.type == RLG_OMNILIGHT)
    {
        shader = rlgCtx->shaders[RLG_SHADER_DEPTH_CUBEMAP];

        // Send the light position to the depth shader
        SetShaderValue(shader, rlgCtx->locDepthCubemapLightPos,
            &l->data.position, SHADER_UNIFORM_VEC3);

        // Send zFar to the depth shader to scale depth from [0..zFar] to [0..1]
        SetShaderValue(shader, rlgCtx->locDepthCubemapFar,
            &rlgCtx->zFar, SHADER_UNIFORM_FLOAT);

        // Send zFar to the lighting shader to scale depth from [0..1] to [0..zFar]
        SetShaderValue(rlgCtx->shaders[RLG_SHADER_MODEL],
            rlgCtx->locLightingFar, &rlgCtx->zFar,
            SHADER_UNIFORM_FLOAT);
    }
    else
    {
        shader = rlgCtx->shaders[RLG_SHADER_DEPTH];
    }

    // Determine the number of iterations for omnidirectional light
    int iterationCount = (l->data.type == RLG_OMNILIGHT) ? 6 : 1;
    for (int i = 0; i < iterationCount; i++)
    {
        // Configure the ModelView matrix
        Matrix matView = { 0 };
        if (l->data.type == RLG_OMNILIGHT)
        {
            // Attach the depth texture of the i-th face
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                GL_TEXTURE_CUBE_MAP_POSITIVE_X + i,
                l->data.shadowMap.depth.id, 0);

            // Calculate the view matrix
            matView = MatrixLookAt(l->data.position, Vector3Add(l->data.position, dirs[i]), ups[i]);
        }
        else
        {
            // Calculate the view matrix for directional and spotlight
            matView = MatrixLookAt(l->data.position, Vector3Add(l->data.position, l->data.direction), INIT_STRUCT(Vector3, 0, 1, 0));

            // Calculate and send the view-projection matrix to the lighting shader for later rendering
            Matrix viewProj = MatrixMultiply(matView, rlGetMatrixProjection());
            SetShaderValueMatrix(rlgCtx->shaders[RLG_SHADER_MODEL], l->locs.vpMatrix, viewProj);
        }

        // Apply the view matrix for rendering into the depth texture
        rlLoadIdentity();
        rlMultMatrixf(MatrixToFloat(matView));

        // Clear the previous state of the depth texture
        rlClearScreenBuffers();

        // Render objects in the light's context
        drawFunc(shader);

        // Flush the rendering batch
        rlDrawRenderBatchActive();
    }

    // End rendering
    rlEnableColorBlend();
    rlDisableFramebuffer();

    // Reset the viewport and projection matrix to the screen settings
    rlViewport(0, 0, GetScreenWidth(), GetScreenHeight());
    rlMatrixMode(RL_PROJECTION);
    rlPopMatrix();
    rlMatrixMode(RL_MODELVIEW);
    rlLoadIdentity();
}

Texture RLG_GetShadowMap(unsigned int light)
{
    if (light >= RLG_MAX_LIGHTS_PER_MATERIAL)
    {
        // Log an error if the light ID exceeds the number of allocated lights
        TraceLog(LOG_ERROR, "Light [ID %i] specified to 'RLG_GetShadowMap' exceeds allocated number [MAX %i]", light, RLG_MAX_LIGHTS_PER_MATERIAL);
        return INIT_STRUCT_ZERO(Texture);
    }

    return rlgCtx->lights[light].data.shadowMap.depth;
}

void RLG_CastMesh(Shader shader, Mesh mesh, Matrix transform)
{
    // Bind shader program
    rlEnableShader(shader.id);

    // Get a copy of current matrices to work with,
    // just in case stereo render is required, and we need to modify them
    // NOTE: At this point the modelview matrix just contains the view matrix (camera)
    // That's because BeginMode3D() sets it and there is no model-drawing function
    // that modifies it, all use rlPushMatrix() and rlPopMatrix()
    Matrix matModel = MatrixIdentity();
    Matrix matView = rlGetMatrixModelview();
    Matrix matModelView = MatrixIdentity();
    Matrix matProjection = rlGetMatrixProjection();

    // Model transformation matrix is sent to shader uniform location: RLG_LOC_MATRIX_MODEL
    if (shader.locs[RLG_LOC_MATRIX_MODEL] != -1)
        rlSetUniformMatrix(shader.locs[RLG_LOC_MATRIX_MODEL], transform);

    // Accumulate several model transformations:
    //    transform: model transformation provided (includes DrawModel() params combined with model.transform)
    //    rlGetMatrixTransform(): rlgl internal transform matrix due to push/pop matrix stack
    matModel = MatrixMultiply(transform, rlGetMatrixTransform());

    // Get model-view matrix
    matModelView = MatrixMultiply(matModel, matView);

    // Try binding vertex array objects (VAO) or use VBOs if not possible
    if (!rlEnableVertexArray(mesh.vaoId))
    {
        // Bind mesh VBO data: vertex position (shader-location = 0)
        rlEnableVertexBuffer(mesh.vboId[0]);
        rlSetVertexAttribute(shader.locs[RLG_LOC_VERTEX_POSITION], 3, RL_FLOAT, 0, 0, 0);
        rlEnableVertexAttribute(shader.locs[RLG_LOC_VERTEX_POSITION]);

        // If vertex indices exist, bine the VBO containing the indices
        if (mesh.indices != NULL) rlEnableVertexBufferElement(mesh.vboId[6]);
    }

    int eyeCount = rlIsStereoRenderEnabled() ? 2 : 1;

    for (int eye = 0; eye < eyeCount; eye++)
    {
        // Calculate model-view-projection matrix (MVP)
        Matrix matModelViewProjection = MatrixIdentity();
        if (eyeCount == 1) matModelViewProjection = MatrixMultiply(matModelView, matProjection);
        else
        {
            // Setup current eye viewport (half screen width)
            rlViewport(eye*rlGetFramebufferWidth()/2, 0, rlGetFramebufferWidth()/2, rlGetFramebufferHeight());
            matModelViewProjection = MatrixMultiply(MatrixMultiply(matModelView, rlGetMatrixViewOffsetStereo(eye)), rlGetMatrixProjectionStereo(eye));
        }

        // Send combined model-view-projection matrix to shader
        rlSetUniformMatrix(shader.locs[RLG_LOC_MATRIX_MVP], matModelViewProjection);

        // Draw mesh
        if (mesh.indices != NULL) rlDrawVertexArrayElements(0, mesh.triangleCount*3, 0);
        else rlDrawVertexArray(0, mesh.vertexCount);
    }

    // Disable all possible vertex array objects (or VBOs)
    rlDisableVertexArray();
    rlDisableVertexBuffer();
    rlDisableVertexBufferElement();

    // Disable shader program
    rlDisableShader();

    // Restore rlgl internal modelview and projection matrices
    rlSetMatrixModelview(matView);
    rlSetMatrixProjection(matProjection);
}

void RLG_CastModel(Shader shader, Model model, Vector3 position, float scale)
{
    Vector3 vScale = { scale, scale, scale };
    Vector3 rotationAxis = { 0.0f, 1.0f, 0.0f };

    RLG_CastModelEx(shader, model, position, rotationAxis, 0.0f, vScale);
}

void RLG_CastModelEx(Shader shader, Model model, Vector3 position, Vector3 rotationAxis, float rotationAngle, Vector3 scale)
{
    Matrix matScale = MatrixScale(scale.x, scale.y, scale.z);
    Matrix matRotation = MatrixRotate(rotationAxis, rotationAngle*DEG2RAD);
    Matrix matTranslation = MatrixTranslate(position.x, position.y, position.z);

    Matrix matTransform = MatrixMultiply(MatrixMultiply(matScale, matRotation), matTranslation);
    model.transform = MatrixMultiply(model.transform, matTransform);

    for (int i = 0; i < model.meshCount; i++)
    {
        RLG_CastMesh(shader, model.meshes[i], model.transform);
    }
}

void RLG_DrawMesh(Mesh mesh, Material material, Matrix transform)
{
    const Shader *shader = &rlgCtx->shaders[RLG_SHADER_MODEL];

    // Bind shader program
    rlEnableShader(shader->id);

    // Send required data to shader (matrices, values)
    //-----------------------------------------------------
    // Upload to shader material.data.colDiffuse
    if (shader->locs[RLG_LOC_COLOR_DIFFUSE] != -1)
    {
        float values[4] = { };

        if (rlgCtx->usedDefaultMaps[MATERIAL_MAP_ALBEDO])
        {
            values[0] = (float)rlgCtx->defaultMaps[MATERIAL_MAP_ALBEDO].color.r/255.0f;
            values[1] = (float)rlgCtx->defaultMaps[MATERIAL_MAP_ALBEDO].color.g/255.0f;
            values[2] = (float)rlgCtx->defaultMaps[MATERIAL_MAP_ALBEDO].color.b/255.0f;
            values[3] = (float)rlgCtx->defaultMaps[MATERIAL_MAP_ALBEDO].color.a/255.0f;
        }
        else
        {
            values[0] = (float)material.maps[MATERIAL_MAP_ALBEDO].color.r/255.0f;
            values[1] = (float)material.maps[MATERIAL_MAP_ALBEDO].color.g/255.0f;
            values[2] = (float)material.maps[MATERIAL_MAP_ALBEDO].color.b/255.0f;
            values[3] = (float)material.maps[MATERIAL_MAP_ALBEDO].color.a/255.0f;
        }

        rlSetUniform(shader->locs[RLG_LOC_COLOR_DIFFUSE], values, SHADER_UNIFORM_VEC4, 1);
    }

    // Upload to shader material.data.colSpecular (if location available)
    if (shader->locs[RLG_LOC_COLOR_SPECULAR] != -1)
    {
        float values[4] = { };

        if (rlgCtx->usedDefaultMaps[MATERIAL_MAP_METALNESS])
        {
            values[0] = (float)rlgCtx->defaultMaps[MATERIAL_MAP_METALNESS].color.r/255.0f;
            values[1] = (float)rlgCtx->defaultMaps[MATERIAL_MAP_METALNESS].color.g/255.0f;
            values[2] = (float)rlgCtx->defaultMaps[MATERIAL_MAP_METALNESS].color.b/255.0f;
            values[3] = (float)rlgCtx->defaultMaps[MATERIAL_MAP_METALNESS].color.a/255.0f;
        }
        else
        {
            values[0] = (float)material.maps[MATERIAL_MAP_METALNESS].color.r/255.0f;
            values[1] = (float)material.maps[MATERIAL_MAP_METALNESS].color.g/255.0f;
            values[2] = (float)material.maps[MATERIAL_MAP_METALNESS].color.b/255.0f;
            values[3] = (float)material.maps[MATERIAL_MAP_METALNESS].color.a/255.0f;
        }

        rlSetUniform(shader->locs[RLG_LOC_COLOR_SPECULAR], values, SHADER_UNIFORM_VEC4, 1);
    }

    // Upload to shader material.data.colEmission (if location available)
    if (shader->locs[RLG_LOC_COLOR_EMISSION] != -1)
    {
        float values[4] = { };

        if (rlgCtx->usedDefaultMaps[MATERIAL_MAP_EMISSION])
        {
            values[0] = (float)rlgCtx->defaultMaps[MATERIAL_MAP_EMISSION].color.r/255.0f;
            values[1] = (float)rlgCtx->defaultMaps[MATERIAL_MAP_EMISSION].color.g/255.0f;
            values[2] = (float)rlgCtx->defaultMaps[MATERIAL_MAP_EMISSION].color.b/255.0f;
            values[3] = (float)rlgCtx->defaultMaps[MATERIAL_MAP_EMISSION].color.a/255.0f;
        }
        else
        {
            values[0] = (float)material.maps[MATERIAL_MAP_EMISSION].color.r/255.0f;
            values[1] = (float)material.maps[MATERIAL_MAP_EMISSION].color.g/255.0f;
            values[2] = (float)material.maps[MATERIAL_MAP_EMISSION].color.b/255.0f;
            values[3] = (float)material.maps[MATERIAL_MAP_EMISSION].color.a/255.0f;
        }

        rlSetUniform(shader->locs[RLG_LOC_COLOR_EMISSION], values, SHADER_UNIFORM_VEC4, 1);
    }

    // Upload to shader material.data.metalness (if location available)
    if (shader->locs[RLG_LOC_METALNESS_SCALE] != -1)
    {
        if (rlgCtx->usedDefaultMaps[MATERIAL_MAP_METALNESS])
        {
            rlSetUniform(shader->locs[RLG_LOC_METALNESS_SCALE],
                &rlgCtx->defaultMaps[MATERIAL_MAP_METALNESS].value, SHADER_UNIFORM_FLOAT, 1);
        }
        else
        {
            rlSetUniform(shader->locs[RLG_LOC_METALNESS_SCALE],
                &material.maps[MATERIAL_MAP_METALNESS].value, SHADER_UNIFORM_FLOAT, 1);
        }
    }

    // Upload to shader material.data.roughness (if location available)
    if (shader->locs[RLG_LOC_ROUGHNESS_SCALE] != -1)
    {
        if (rlgCtx->usedDefaultMaps[MATERIAL_MAP_ROUGHNESS])
        {
            rlSetUniform(shader->locs[RLG_LOC_ROUGHNESS_SCALE],
                &rlgCtx->defaultMaps[MATERIAL_MAP_ROUGHNESS].value, SHADER_UNIFORM_FLOAT, 1);
        }
        else
        {
            rlSetUniform(shader->locs[RLG_LOC_ROUGHNESS_SCALE],
                &material.maps[MATERIAL_MAP_ROUGHNESS].value, SHADER_UNIFORM_FLOAT, 1);
        }
    }

    // Upload to shader material.data.aoLightAffect (if location available)
    if (shader->locs[RLG_LOC_AO_LIGHT_AFFECT] != -1)
    {
        if (rlgCtx->usedDefaultMaps[MATERIAL_MAP_OCCLUSION])
        {
            rlSetUniform(shader->locs[RLG_LOC_AO_LIGHT_AFFECT],
                &rlgCtx->defaultMaps[MATERIAL_MAP_OCCLUSION].value, SHADER_UNIFORM_FLOAT, 1);
        }
        else
        {
            rlSetUniform(shader->locs[RLG_LOC_AO_LIGHT_AFFECT],
                &material.maps[MATERIAL_MAP_OCCLUSION].value, SHADER_UNIFORM_FLOAT, 1);
        }
    }

    // Upload to shader material.data.heightScale (if location available)
    if (shader->locs[RLG_LOC_HEIGHT_SCALE] != -1)
    {
        if (rlgCtx->usedDefaultMaps[MATERIAL_MAP_HEIGHT])
        {
            rlSetUniform(shader->locs[RLG_LOC_HEIGHT_SCALE],
                &rlgCtx->defaultMaps[MATERIAL_MAP_HEIGHT].value, SHADER_UNIFORM_FLOAT, 1);
        }
        else
        {
            rlSetUniform(shader->locs[RLG_LOC_HEIGHT_SCALE],
                &material.maps[MATERIAL_MAP_HEIGHT].value, SHADER_UNIFORM_FLOAT, 1);
        }
    }

    // Get a copy of current matrices to work with,
    // just in case stereo render is required, and we need to modify them
    // NOTE: At this point the modelview matrix just contains the view matrix (camera)
    // That's because BeginMode3D() sets it and there is no model-drawing function
    // that modifies it, all use rlPushMatrix() and rlPopMatrix()
    Matrix matModel = MatrixIdentity();
    Matrix matView = rlGetMatrixModelview();
    Matrix matModelView = MatrixIdentity();
    Matrix matProjection = rlGetMatrixProjection();

    // Upload view matrix (if location available)
    if (shader->locs[RLG_LOC_MATRIX_VIEW] != -1)
        rlSetUniformMatrix(shader->locs[RLG_LOC_MATRIX_VIEW], matView);

    // Upload projection matrix (if location available)
    if (shader->locs[RLG_LOC_MATRIX_PROJECTION] != -1)
        rlSetUniformMatrix(shader->locs[RLG_LOC_MATRIX_PROJECTION], matProjection);

    // Model transformation matrix is sent to shader uniform location: RLG_LOC_MATRIX_MODEL
    if (shader->locs[RLG_LOC_MATRIX_MODEL] != -1)
        rlSetUniformMatrix(shader->locs[RLG_LOC_MATRIX_MODEL], transform);

    // Accumulate several model transformations:
    //    transform: model transformation provided (includes DrawModel() params combined with model.transform)
    //    rlGetMatrixTransform(): rlgl internal transform matrix due to push/pop matrix stack
    matModel = MatrixMultiply(transform, rlGetMatrixTransform());

    // Get model-view matrix
    matModelView = MatrixMultiply(matModel, matView);

    // Upload model normal matrix (if locations available)
    if (shader->locs[RLG_LOC_MATRIX_NORMAL] != -1)
        rlSetUniformMatrix(shader->locs[RLG_LOC_MATRIX_NORMAL], MatrixTranspose(MatrixInvert(matModel)));
    //-----------------------------------------------------

    // Bind active texture maps (if available)
    for (int i = 0; i < 11; i++)
    {
        if (rlgCtx->material.data.useMaps[i])
        {
            int textureID = (rlgCtx->usedDefaultMaps[i])
                ? rlgCtx->defaultMaps[i].texture.id
                : material.maps[i].texture.id;

            if (textureID > 0)
            {
                // Select current shader texture slot
                rlActiveTextureSlot(i);

                // Enable texture for active slot
                if (i == MATERIAL_MAP_IRRADIANCE ||
                    i == MATERIAL_MAP_PREFILTER ||
                    i == MATERIAL_MAP_CUBEMAP)
                {
                    rlEnableTextureCubemap(textureID);
                }
                else
                {
                    rlEnableTexture(textureID);
                }

                rlSetUniform(shader->locs[RLG_LOC_MAP_ALBEDO + i], &i, SHADER_UNIFORM_INT, 1);
            }
        }
    }

    // Bind depth textures for shadow mapping
    for (int i = 0; i < RLG_MAX_LIGHTS_PER_MATERIAL; i++)
    {
        const struct RLG_Light *l = &rlgCtx->lights[i];

        if (l->data.enabled && l->data.shadow)
        {
            int j = 11 + i;
            rlActiveTextureSlot(j);

            if (l->data.type == RLG_OMNILIGHT)
            {
                rlEnableTextureCubemap(l->data.shadowMap.depth.id);
                rlSetUniform(l->locs.shadowCubemap, &j, SHADER_UNIFORM_INT, 1);
            }
            else
            {
                rlEnableTexture(l->data.shadowMap.depth.id);
                rlSetUniform(l->locs.shadowMap, &j, SHADER_UNIFORM_INT, 1);
            }
        }
    }

    // Try binding vertex array objects (VAO) or use VBOs if not possible
    // WARNING: UploadMesh() enables all vertex attributes available in mesh and sets default attribute values
    // for shader expected vertex attributes that are not provided by the mesh (i.e. colors)
    // This could be a dangerous approach because different meshes with different shaders can enable/disable some attributes
    if (!rlEnableVertexArray(mesh.vaoId))
    {
        // Bind mesh VBO data: vertex position (shader-location = 0)
        rlEnableVertexBuffer(mesh.vboId[0]);
        rlSetVertexAttribute(shader->locs[RLG_LOC_VERTEX_POSITION], 3, RL_FLOAT, 0, 0, 0);
        rlEnableVertexAttribute(shader->locs[RLG_LOC_VERTEX_POSITION]);

        // Bind mesh VBO data: vertex texcoords (shader-location = 1)
        rlEnableVertexBuffer(mesh.vboId[1]);
        rlSetVertexAttribute(shader->locs[RLG_LOC_VERTEX_TEXCOORD01], 2, RL_FLOAT, 0, 0, 0);
        rlEnableVertexAttribute(shader->locs[RLG_LOC_VERTEX_TEXCOORD01]);

        if (shader->locs[RLG_LOC_VERTEX_NORMAL] != -1)
        {
            // Bind mesh VBO data: vertex normals (shader-location = 2)
            rlEnableVertexBuffer(mesh.vboId[2]);
            rlSetVertexAttribute(shader->locs[RLG_LOC_VERTEX_NORMAL], 3, RL_FLOAT, 0, 0, 0);
            rlEnableVertexAttribute(shader->locs[RLG_LOC_VERTEX_NORMAL]);
        }

        // Bind mesh VBO data: vertex colors (shader-location = 3, if available)
        if (shader->locs[RLG_LOC_VERTEX_COLOR] != -1)
        {
            if (mesh.vboId[3] != 0)
            {
                rlEnableVertexBuffer(mesh.vboId[3]);
                rlSetVertexAttribute(shader->locs[RLG_LOC_VERTEX_COLOR], 4, RL_UNSIGNED_BYTE, 1, 0, 0);
                rlEnableVertexAttribute(shader->locs[RLG_LOC_VERTEX_COLOR]);
            }
            else
            {
                // Set default value for defined vertex attribute in shader but not provided by mesh
                // WARNING: It could result in GPU undefined behaviour
                float value[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
                rlSetVertexAttributeDefault(shader->locs[RLG_LOC_VERTEX_COLOR], value, SHADER_ATTRIB_VEC4, 4);
                rlDisableVertexAttribute(shader->locs[RLG_LOC_VERTEX_COLOR]);
            }
        }

        // Bind mesh VBO data: vertex tangents (shader-location = 4, if available)
        if (shader->locs[RLG_LOC_VERTEX_TANGENT] != -1)
        {
            rlEnableVertexBuffer(mesh.vboId[4]);
            rlSetVertexAttribute(shader->locs[RLG_LOC_VERTEX_TANGENT], 4, RL_FLOAT, 0, 0, 0);
            rlEnableVertexAttribute(shader->locs[RLG_LOC_VERTEX_TANGENT]);
        }

        // Bind mesh VBO data: vertex texcoords2 (shader-location = 5, if available)
        if (shader->locs[RLG_LOC_VERTEX_TEXCOORD02] != -1)
        {
            rlEnableVertexBuffer(mesh.vboId[5]);
            rlSetVertexAttribute(shader->locs[RLG_LOC_VERTEX_TEXCOORD02], 2, RL_FLOAT, 0, 0, 0);
            rlEnableVertexAttribute(shader->locs[RLG_LOC_VERTEX_TEXCOORD02]);
        }

        if (mesh.indices != NULL) rlEnableVertexBufferElement(mesh.vboId[6]);
    }

    int eyeCount = 1;
    if (rlIsStereoRenderEnabled()) eyeCount = 2;

    for (int eye = 0; eye < eyeCount; eye++)
    {
        // Calculate model-view-projection matrix (MVP)
        Matrix matModelViewProjection = MatrixIdentity();
        if (eyeCount == 1) matModelViewProjection = MatrixMultiply(matModelView, matProjection);
        else
        {
            // Setup current eye viewport (half screen width)
            rlViewport(eye*rlGetFramebufferWidth()/2, 0, rlGetFramebufferWidth()/2, rlGetFramebufferHeight());
            matModelViewProjection = MatrixMultiply(MatrixMultiply(matModelView, rlGetMatrixViewOffsetStereo(eye)), rlGetMatrixProjectionStereo(eye));
        }

        // Send combined model-view-projection matrix to shader
        rlSetUniformMatrix(shader->locs[RLG_LOC_MATRIX_MVP], matModelViewProjection);

        // Draw mesh
        if (mesh.indices != NULL) rlDrawVertexArrayElements(0, mesh.triangleCount*3, 0);
        else rlDrawVertexArray(0, mesh.vertexCount);
    }

    // Unbind all bound texture maps
    for (int i = 0; i < 11; i++)
    {
        if (rlgCtx->material.data.useMaps[i])
        {
            // Select current shader texture slot
            rlActiveTextureSlot(i);

            // Disable texture for active slot
            if (i == MATERIAL_MAP_IRRADIANCE ||
                i == MATERIAL_MAP_PREFILTER ||
                i == MATERIAL_MAP_CUBEMAP)
            {
                rlDisableTextureCubemap();
            }
            else
            {
                rlDisableTexture();
            }
        }
    }

    // Unbind depth textures
    for (int i = 0; i < RLG_MAX_LIGHTS_PER_MATERIAL; i++)
    {
        const struct RLG_Light *l = &rlgCtx->lights[i];

        if (l->data.enabled && l->data.shadow)
        {
            rlActiveTextureSlot(11 + i);

            if (l->data.type == RLG_OMNILIGHT)
            {
                rlDisableTextureCubemap();
            }
            else
            {
                rlDisableTexture();
            }
        }
    }

    // Disable all possible vertex array objects (or VBOs)
    rlDisableVertexArray();
    rlDisableVertexBuffer();
    rlDisableVertexBufferElement();

    // Disable shader program
    rlDisableShader();

    // Restore rlgl internal modelview and projection matrices
    rlSetMatrixModelview(matView);
    rlSetMatrixProjection(matProjection);
}

void RLG_DrawModel(Model model, Vector3 position, float scale, Color tint)
{
    Vector3 vScale = { scale, scale, scale };
    Vector3 rotationAxis = { 0.0f, 1.0f, 0.0f };

    RLG_DrawModelEx(model, position, rotationAxis, 0.0f, vScale, tint);
}

void RLG_DrawModelEx(Model model, Vector3 position, Vector3 rotationAxis, float rotationAngle, Vector3 scale, Color tint)
{
    Matrix matScale = MatrixScale(scale.x, scale.y, scale.z);
    Matrix matRotation = MatrixRotate(rotationAxis, rotationAngle*DEG2RAD);
    Matrix matTranslation = MatrixTranslate(position.x, position.y, position.z);

    Matrix matTransform = MatrixMultiply(MatrixMultiply(matScale, matRotation), matTranslation);
    model.transform = MatrixMultiply(model.transform, matTransform);

    for (int i = 0; i < model.meshCount; i++)
    {
        Color color = model.materials[model.meshMaterial[i]].maps[MATERIAL_MAP_DIFFUSE].color;

        Color colorTint = WHITE;
        colorTint.r = (unsigned char)((color.r*tint.r)/255);
        colorTint.g = (unsigned char)((color.g*tint.g)/255);
        colorTint.b = (unsigned char)((color.b*tint.b)/255);
        colorTint.a = (unsigned char)((color.a*tint.a)/255);

        model.materials[model.meshMaterial[i]].maps[MATERIAL_MAP_DIFFUSE].color = colorTint;
        RLG_DrawMesh(model.meshes[i], model.materials[model.meshMaterial[i]], model.transform);
        model.materials[model.meshMaterial[i]].maps[MATERIAL_MAP_DIFFUSE].color = color;
    }
}

RLG_Skybox RLG_LoadSkybox(const char* skyboxFileName)
{
    RLG_Skybox skybox = { 0 };

    // Load the cubemap texture from the image file
    Image img = LoadImage(skyboxFileName);
    skybox.cubemap = LoadTextureCubemap(img, CUBEMAP_LAYOUT_AUTO_DETECT);
    UnloadImage(img);

    // Generate Irradiance Cubemap
    {
        int size = skybox.cubemap.width / 16;
        size = (size < 8) ? 8 : size;

        // Create a renderbuffer for depth attachment
        unsigned int rbo = rlLoadTextureDepth(size, size, true);

        // Create a cubemap texture to hold the HDR data
        skybox.irradiance.id = rlLoadTextureCubemap(NULL, size, skybox.cubemap.format, _RLIGHTS_RL55_MIPMAP_COUNT);
        rlCubemapParameters(skybox.irradiance.id, RL_TEXTURE_MIN_FILTER, RL_TEXTURE_FILTER_LINEAR);
        rlCubemapParameters(skybox.irradiance.id, GL_TEXTURE_MAG_FILTER, RL_TEXTURE_FILTER_LINEAR);

        // Create and configure the framebuffer
        unsigned int fbo = rlLoadFramebuffer();
        rlFramebufferAttach(fbo, rbo, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_RENDERBUFFER, 0);
        rlFramebufferAttach(fbo, skybox.irradiance.id, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_CUBEMAP_POSITIVE_X, 0);

        // Validate the framebuffer configuration
        if (rlFramebufferComplete(fbo))
        {
            TraceLog(LOG_INFO, "FBO: [ID %i] Framebuffer object created successfully", fbo);
        }  

        // Enable the shader for converting HDR equirectangular environment map to cubemap faces
        rlEnableShader(rlgCtx->shaders[RLG_SHADER_IRRADIANCE_CONVOLUTION].id);

        // Set the projection matrix for the shader
        Matrix matFboProjection = MatrixPerspective(90.0 * DEG2RAD, 1.0, 0.1, 10.0);
        rlSetUniformMatrix(rlgCtx->shaders[RLG_SHADER_IRRADIANCE_CONVOLUTION].locs[SHADER_LOC_MATRIX_PROJECTION], matFboProjection);

        // Define view matrices for each cubemap face
        Matrix fboViews[6] = {
            MatrixLookAt(INIT_STRUCT_ZERO(Vector3), INIT_STRUCT(Vector3,  1.0f,  0.0f,  0.0f), INIT_STRUCT(Vector3, 0.0f, -1.0f,  0.0f)),
            MatrixLookAt(INIT_STRUCT_ZERO(Vector3), INIT_STRUCT(Vector3, -1.0f,  0.0f,  0.0f), INIT_STRUCT(Vector3, 0.0f, -1.0f,  0.0f)),
            MatrixLookAt(INIT_STRUCT_ZERO(Vector3), INIT_STRUCT(Vector3,  0.0f,  1.0f,  0.0f), INIT_STRUCT(Vector3, 0.0f,  0.0f,  1.0f)),
            MatrixLookAt(INIT_STRUCT_ZERO(Vector3), INIT_STRUCT(Vector3,  0.0f, -1.0f,  0.0f), INIT_STRUCT(Vector3, 0.0f,  0.0f, -1.0f)),
            MatrixLookAt(INIT_STRUCT_ZERO(Vector3), INIT_STRUCT(Vector3,  0.0f,  0.0f,  1.0f), INIT_STRUCT(Vector3, 0.0f, -1.0f,  0.0f)),
            MatrixLookAt(INIT_STRUCT_ZERO(Vector3), INIT_STRUCT(Vector3,  0.0f,  0.0f, -1.0f), INIT_STRUCT(Vector3, 0.0f, -1.0f,  0.0f))
        };

        // Set the viewport to match the framebuffer dimensions
        rlViewport(0, 0, size, size);
        rlDisableBackfaceCulling();

        // Activate the panorama texture
        rlActiveTextureSlot(0);
        rlEnableTextureCubemap(skybox.cubemap.id);

        for (int i = 0; i < 6; i++)
        {
            // Set the view matrix for the current cubemap face
            rlSetUniformMatrix(rlgCtx->shaders[RLG_SHADER_IRRADIANCE_CONVOLUTION].locs[SHADER_LOC_MATRIX_VIEW], fboViews[i]);
            
            // Attach the current cubemap face to the framebuffer
            rlFramebufferAttach(fbo, skybox.irradiance.id, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_CUBEMAP_POSITIVE_X + i, 0);
            rlEnableFramebuffer(fbo);

            // Clear the framebuffer and draw the cube face
            rlClearScreenBuffers();
            rlLoadDrawCube();
        }

        // Disable the shader and textures
        rlDisableShader();
        rlDisableTextureCubemap();
        rlDisableFramebuffer();

        // Unload the framebuffer and its attachments
        rlUnloadFramebuffer(fbo);

        // Reset the viewport to default dimensions
        rlViewport(0, 0, rlGetFramebufferWidth(), rlGetFramebufferHeight());
        rlEnableBackfaceCulling();

        // Set the cubemap properties
        skybox.cubemap.width = size;
        skybox.cubemap.height = size;
        skybox.cubemap.mipmaps = 1;
        skybox.cubemap.format = skybox.cubemap.format;
    }

    return skybox;
}

RLG_Skybox RLG_LoadSkyboxHDR(const char* skyboxFileName, int size, int format)
{
    RLG_Skybox skybox = { 0 };

    // Create a framebuffer object (FBO) to generate the skybox and irradiance map
    unsigned int fbo = rlLoadFramebuffer();

    // Generate the cubemap for the skybox
    {
        // Load the HDR panorama texture
        Texture2D panorama = LoadTexture(skyboxFileName);

        // Create a renderbuffer for depth attachment
        unsigned int rbo = rlLoadTextureDepth(size, size, true);

        // Create a cubemap texture to hold the HDR data
        skybox.cubemap.id = rlLoadTextureCubemap(NULL, size, format, _RLIGHTS_RL55_MIPMAP_COUNT);

        // Configure the framebuffer with the renderbuffer and cubemap texture
        rlFramebufferAttach(fbo, rbo, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_RENDERBUFFER, 0);
        rlFramebufferAttach(fbo, skybox.cubemap.id, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_CUBEMAP_POSITIVE_X, 0);

        // Validate the framebuffer configuration
        if (rlFramebufferComplete(fbo))
        {
            TraceLog(LOG_INFO, "FBO: [ID %i] Framebuffer object created successfully", fbo);
        }

        // Enable the shader to convert the HDR equirectangular map to cubemap faces
        rlEnableShader(rlgCtx->shaders[RLG_SHADER_EQUIRECTANGULAR_TO_CUBEMAP].id);

        // Set the projection matrix for the shader
        Matrix matFboProjection = MatrixPerspective(90.0 * DEG2RAD, 1.0, 0.1, 10.0);
        rlSetUniformMatrix(rlgCtx->shaders[RLG_SHADER_EQUIRECTANGULAR_TO_CUBEMAP].locs[SHADER_LOC_MATRIX_PROJECTION], matFboProjection);

        // Define view matrices for each cubemap face
        Matrix fboViews[6] = {
            MatrixLookAt(INIT_STRUCT_ZERO(Vector3), INIT_STRUCT(Vector3,  1.0f,  0.0f,  0.0f), INIT_STRUCT(Vector3, 0.0f, -1.0f,  0.0f)),
            MatrixLookAt(INIT_STRUCT_ZERO(Vector3), INIT_STRUCT(Vector3, -1.0f,  0.0f,  0.0f), INIT_STRUCT(Vector3, 0.0f, -1.0f,  0.0f)),
            MatrixLookAt(INIT_STRUCT_ZERO(Vector3), INIT_STRUCT(Vector3,  0.0f,  1.0f,  0.0f), INIT_STRUCT(Vector3, 0.0f,  0.0f,  1.0f)),
            MatrixLookAt(INIT_STRUCT_ZERO(Vector3), INIT_STRUCT(Vector3,  0.0f, -1.0f,  0.0f), INIT_STRUCT(Vector3, 0.0f,  0.0f, -1.0f)),
            MatrixLookAt(INIT_STRUCT_ZERO(Vector3), INIT_STRUCT(Vector3,  0.0f,  0.0f,  1.0f), INIT_STRUCT(Vector3, 0.0f, -1.0f,  0.0f)),
            MatrixLookAt(INIT_STRUCT_ZERO(Vector3), INIT_STRUCT(Vector3,  0.0f,  0.0f, -1.0f), INIT_STRUCT(Vector3, 0.0f, -1.0f,  0.0f))
        };

        // Set the viewport to match the framebuffer dimensions
        rlViewport(0, 0, size, size);
        rlDisableBackfaceCulling();

        // Activate the panorama texture
        rlActiveTextureSlot(0);
        rlEnableTexture(panorama.id);

        for (int i = 0; i < 6; i++)
        {
            // Set the view matrix for the current cubemap face
            rlSetUniformMatrix(rlgCtx->shaders[RLG_SHADER_EQUIRECTANGULAR_TO_CUBEMAP].locs[SHADER_LOC_MATRIX_VIEW], fboViews[i]);
            
            // Attach the current cubemap face to the framebuffer
            rlFramebufferAttach(fbo, skybox.cubemap.id, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_CUBEMAP_POSITIVE_X + i, 0);
            rlEnableFramebuffer(fbo);

            // Clear the framebuffer and draw the cube
            rlClearScreenBuffers();
            rlLoadDrawCube();
        }

        // Disable the shader and textures
        rlDisableShader();
        rlDisableTexture();
        rlDisableFramebuffer();

        // Reset the viewport to default dimensions
        rlViewport(0, 0, rlGetFramebufferWidth(), rlGetFramebufferHeight());
        rlEnableBackfaceCulling();

        // Set the cubemap properties
        skybox.cubemap.width = size;
        skybox.cubemap.height = size;
        skybox.cubemap.mipmaps = 1;
        skybox.cubemap.format = format;

        // Unload the panorama texture as it's no longer needed
        UnloadTexture(panorama);
    }

    // Generate the irradiance cubemap
    {
        int irrSize = skybox.cubemap.width / 16;
        irrSize = (irrSize < 8) ? 8 : irrSize;

        // Create a renderbuffer for depth attachment
        unsigned int rbo = rlLoadTextureDepth(irrSize, irrSize, true);

        // Create a cubemap texture to hold the irradiance data
        skybox.irradiance.id = rlLoadTextureCubemap(NULL, irrSize, skybox.cubemap.format, _RLIGHTS_RL55_MIPMAP_COUNT);
        rlCubemapParameters(skybox.irradiance.id, RL_TEXTURE_MIN_FILTER, RL_TEXTURE_FILTER_LINEAR);
        rlCubemapParameters(skybox.irradiance.id, GL_TEXTURE_MAG_FILTER, RL_TEXTURE_FILTER_LINEAR);

        // Create and configure the framebuffer
        unsigned int fbo = rlLoadFramebuffer();
        rlFramebufferAttach(fbo, rbo, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_RENDERBUFFER, 0);
        rlFramebufferAttach(fbo, skybox.irradiance.id, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_CUBEMAP_POSITIVE_X, 0);

        // Validate the framebuffer configuration
        if (rlFramebufferComplete(fbo))
        {
            TraceLog(LOG_INFO, "FBO: [ID %i] Framebuffer object created successfully", fbo);
        }

        // Enable the shader for irradiance convolution
        rlEnableShader(rlgCtx->shaders[RLG_SHADER_IRRADIANCE_CONVOLUTION].id);

        // Set the projection matrix for the shader
        Matrix matFboProjection = MatrixPerspective(90.0 * DEG2RAD, 1.0, 0.1, 10.0);
        rlSetUniformMatrix(rlgCtx->shaders[RLG_SHADER_IRRADIANCE_CONVOLUTION].locs[SHADER_LOC_MATRIX_PROJECTION], matFboProjection);

        // Define view matrices for each cubemap face
        Matrix fboViews[6] = {
            MatrixLookAt(INIT_STRUCT_ZERO(Vector3), INIT_STRUCT(Vector3,  1.0f,  0.0f,  0.0f), INIT_STRUCT(Vector3, 0.0f, -1.0f,  0.0f)),
            MatrixLookAt(INIT_STRUCT_ZERO(Vector3), INIT_STRUCT(Vector3, -1.0f,  0.0f,  0.0f), INIT_STRUCT(Vector3, 0.0f, -1.0f,  0.0f)),
            MatrixLookAt(INIT_STRUCT_ZERO(Vector3), INIT_STRUCT(Vector3,  0.0f,  1.0f,  0.0f), INIT_STRUCT(Vector3, 0.0f,  0.0f,  1.0f)),
            MatrixLookAt(INIT_STRUCT_ZERO(Vector3), INIT_STRUCT(Vector3,  0.0f, -1.0f,  0.0f), INIT_STRUCT(Vector3, 0.0f,  0.0f, -1.0f)),
            MatrixLookAt(INIT_STRUCT_ZERO(Vector3), INIT_STRUCT(Vector3,  0.0f,  0.0f,  1.0f), INIT_STRUCT(Vector3, 0.0f, -1.0f,  0.0f)),
            MatrixLookAt(INIT_STRUCT_ZERO(Vector3), INIT_STRUCT(Vector3,  0.0f,  0.0f, -1.0f), INIT_STRUCT(Vector3, 0.0f, -1.0f,  0.0f))
        };

        // Set the viewport to match the framebuffer dimensions
        rlViewport(0, 0, irrSize, irrSize);
        rlDisableBackfaceCulling();

        // Activate the cubemap texture
        rlActiveTextureSlot(0);
        rlEnableTextureCubemap(skybox.cubemap.id);

        for (int i = 0; i < 6; i++)
        {
            // Set the view matrix for the current cubemap face
            rlSetUniformMatrix(rlgCtx->shaders[RLG_SHADER_IRRADIANCE_CONVOLUTION].locs[SHADER_LOC_MATRIX_VIEW], fboViews[i]);
            
            // Attach the current cubemap face to the framebuffer
            rlFramebufferAttach(fbo, skybox.irradiance.id, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_CUBEMAP_POSITIVE_X + i, 0);
            rlEnableFramebuffer(fbo);

            // Clear the framebuffer and draw the cube
            rlClearScreenBuffers();
            rlLoadDrawCube();
        }

        // Disable the shader and textures
        rlDisableShader();
        rlDisableTextureCubemap();
        rlDisableFramebuffer();

        // Reset the viewport to default dimensions
        rlViewport(0, 0, rlGetFramebufferWidth(), rlGetFramebufferHeight());
        rlEnableBackfaceCulling();

        // Set the cubemap properties
        skybox.cubemap.width = irrSize;
        skybox.cubemap.height = irrSize;
        skybox.cubemap.mipmaps = 1;
        skybox.cubemap.format = skybox.cubemap.format;
    }

    // Unload the framebuffer
    rlUnloadFramebuffer(fbo);

    // Indicate that the texture used is HDR
    skybox.isHDR = true;

    return skybox;
}

void RLG_UnloadSkybox(RLG_Skybox skybox)
{
    UnloadTexture(skybox.cubemap);
    UnloadTexture(skybox.irradiance);
}

void RLG_DrawSkybox(RLG_Skybox skybox)
{
    Shader *shader = &rlgCtx->shaders[RLG_SHADER_SKYBOX];

    // Bind shader program
    rlEnableShader(shader->id);

    if (rlgCtx->skybox.previousCubemapID != skybox.cubemap.id)
    {
        int isHDR = (int)skybox.isHDR;
        rlSetUniform(rlgCtx->skybox.locDoGamma, &isHDR, SHADER_UNIFORM_INT, 1);
        rlgCtx->skybox.previousCubemapID = skybox.cubemap.id;
    }

    rlDisableBackfaceCulling();
    rlDisableDepthMask();

    // Get current view/projection matrices
    Matrix matView = rlGetMatrixModelview();
    Matrix matProjection = rlGetMatrixProjection();

    // Upload view and projection matrices (if locations available)
    if (shader->locs[SHADER_LOC_MATRIX_VIEW] != -1) rlSetUniformMatrix(shader->locs[SHADER_LOC_MATRIX_VIEW], matView);
    if (shader->locs[SHADER_LOC_MATRIX_PROJECTION] != -1) rlSetUniformMatrix(shader->locs[SHADER_LOC_MATRIX_PROJECTION], matProjection);

    // Bind cubemap texture (if available)
    if (skybox.cubemap.id > 0)
    {
        rlActiveTextureSlot(0);
        rlEnableTextureCubemap(skybox.cubemap.id);
    }

    // Try binding vertex array objects (VAO) or use VBOs if not possible
    if (!rlEnableVertexArray(rlgCtx->skybox.vao))
    {
        // Bind mesh VBO data: vertex position (shader-location = 0)
        rlEnableVertexBuffer(rlgCtx->skybox.vbo);
        rlSetVertexAttribute(shader->locs[SHADER_LOC_VERTEX_POSITION], 3, RL_FLOAT, 0, 0, 0);
        rlEnableVertexAttribute(shader->locs[SHADER_LOC_VERTEX_POSITION]);

        if (rlgCtx->skybox.ebo != 0)
        {
            rlEnableVertexBufferElement(rlgCtx->skybox.ebo);
        }
    }

    int eyeCount = 1;
    if (rlIsStereoRenderEnabled()) eyeCount = 2;

    for (int eye = 0; eye < eyeCount; eye++)
    {
        // Calculate model-view-projection matrix (MVP)
        Matrix matModelViewProjection = MatrixIdentity();
        if (eyeCount == 1)
        {
            matModelViewProjection = MatrixMultiply(matView, matProjection);
        }
        else
        {
            // Setup current eye viewport (half screen width)
            rlViewport(eye*rlGetFramebufferWidth()/2, 0, rlGetFramebufferWidth()/2, rlGetFramebufferHeight());
            matModelViewProjection = MatrixMultiply(MatrixMultiply(matView, rlGetMatrixViewOffsetStereo(eye)), rlGetMatrixProjectionStereo(eye));
        }

        // Send combined model-view-projection matrix to shader
        rlSetUniformMatrix(shader->locs[SHADER_LOC_MATRIX_MVP], matModelViewProjection);

        // Draw mesh
        if (rlgCtx->skybox.ebo != 0)
        {
            rlDrawVertexArrayElements(0, 36, 0);
        }
        else
        {
            rlDrawVertexArray(0, 36);
        }
    }

    // Unbind bound cubemap texture
    if (skybox.cubemap.id > 0)
    {
        rlActiveTextureSlot(0);
        rlDisableTextureCubemap();
    }

    // Disable all possible vertex array objects (or VBOs)
    rlDisableVertexArray();
    rlDisableVertexBuffer();
    rlDisableVertexBufferElement();

    // Disable shader program
    rlDisableShader();

    // Restore rlgl internal modelview and projection matrices
    rlSetMatrixModelview(matView);
    rlSetMatrixProjection(matProjection);

    rlEnableBackfaceCulling();
    rlEnableDepthMask();
}

/* Helper Function Declarations */

unsigned int EXT_LoadShaderEx(const char** vsCodes, const char** fsCodes, int vsCount, int fsCount)
{
    GLint success;
    GLchar infoLog[512];

    /* Compile Vertex Shader */

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, vsCount, vsCodes, 0);
    glCompileShader(vs);

    glGetShaderiv(vs, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(vs, sizeof(infoLog), 0, infoLog);
        TraceLog(LOG_ERROR, "Failed to compile vertex shader: %s\n", infoLog);
        glDeleteShader(vs);
        return 0;
    }

    /* Compile Fragment Shader */

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, fsCount, fsCodes, 0);
    glCompileShader(fs);

    glGetShaderiv(fs, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(fs, sizeof(infoLog), 0, infoLog);
        TraceLog(LOG_ERROR, "Failed to compile fragment shader: %s\n", infoLog);
        glDeleteShader(fs);
        return 0;
    }

    /* Link Shaders */

    GLuint program = glCreateProgram();

    if (program == 0) {
        TraceLog(LOG_ERROR, "Failed to create shader program\n");
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0;
    }

    glAttachShader(program, vs);
    glAttachShader(program, fs);

    glBindAttribLocation(program, 0, RLG_SHADER_ATTRIB_POSITION);
    glBindAttribLocation(program, 1, RLG_SHADER_ATTRIB_TEXCOORD);
    glBindAttribLocation(program, 2, RLG_SHADER_ATTRIB_NORMAL);
    glBindAttribLocation(program, 3, RLG_SHADER_ATTRIB_COLOR);
    glBindAttribLocation(program, 4, RLG_SHADER_ATTRIB_TANGENT);
    glBindAttribLocation(program, 5, RLG_SHADER_ATTRIB_TEXCOORD2);

    glLinkProgram(program);

    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        glGetProgramInfoLog(program, sizeof(infoLog), 0, infoLog);
        TraceLog(LOG_ERROR, "Failed to link program shader: %s\n", infoLog);
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    return program;
}

#undef TOSTRING
#undef STRINGIFY

#undef INIT_STRUCT
#undef INIT_STRUCT_ZERO

#endif //RLIGHTS_IMPLEMENTATION
#endif //RLIGHTS_H
