#include "VulkanFrameCapture.h"

#include <cmath>

namespace CryVR
{
namespace
{
struct Vec3 { float x, y, z; };
struct Quat { float x, y, z, w; };

Vec3 Rotate(Quat q, Vec3 v)
{
    const Vec3 u{q.x, q.y, q.z};
    const float dotUV = u.x*v.x + u.y*v.y + u.z*v.z;
    const float dotUU = u.x*u.x + u.y*u.y + u.z*u.z;
    const Vec3 cross{u.y*v.z - u.z*v.y, u.z*v.x - u.x*v.z, u.x*v.y - u.y*v.x};
    return {2.0f*dotUV*u.x + (q.w*q.w-dotUU)*v.x + 2.0f*q.w*cross.x,
            2.0f*dotUV*u.y + (q.w*q.w-dotUU)*v.y + 2.0f*q.w*cross.y,
            2.0f*dotUV*u.z + (q.w*q.w-dotUU)*v.z + 2.0f*q.w*cross.z};
}

void MultiplyColumnMajor(const float a[16], const float b[16], float out[16])
{
    float result[16]{};
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 4; ++k)
                result[column*4 + row] += a[k*4 + row] * b[column*4 + k];
    for (int i = 0; i < 16; ++i)
        out[i] = result[i];
}

bool BuildProjection(const XrFovf& fov, float nearPlane, float farPlane, float projection[16],
                     float fovScale = 1.0f)
{
    const float left = std::tan(fov.angleLeft * fovScale);
    const float right = std::tan(fov.angleRight * fovScale);
    const float down = std::tan(fov.angleDown * fovScale);
    const float up = std::tan(fov.angleUp * fovScale);
    if (!projection || !std::isfinite(left) || !std::isfinite(right) ||
        !std::isfinite(down) || !std::isfinite(up) || !(right > left) || !(up > down) ||
        !(nearPlane > 0.0f) || !(farPlane > nearPlane) ||
        !(fovScale > 0.0f) || !std::isfinite(fovScale))
        return false;
    const float depth = nearPlane - farPlane;
    const float values[16] = {
        2.0f/(right-left), 0.0f, 0.0f, 0.0f,
        0.0f, -2.0f/(up-down), 0.0f, 0.0f,
        (right+left)/(right-left), -(up+down)/(up-down), farPlane/depth, -1.0f,
        0.0f, 0.0f, farPlane*nearPlane/depth, 0.0f
    };
    for (int i = 0; i < 16; ++i) projection[i] = values[i];
    return true;
}

bool BuildEyeView(const XrPosef& pose, float view[16])
{
    Quat inverse{-pose.orientation.x, -pose.orientation.y,
                 -pose.orientation.z, pose.orientation.w};
    const float length = std::sqrt(inverse.x*inverse.x + inverse.y*inverse.y +
                                   inverse.z*inverse.z + inverse.w*inverse.w);
    if (!(length > 0.0f) || !std::isfinite(length) ||
        !std::isfinite(pose.position.x) || !std::isfinite(pose.position.y) ||
        !std::isfinite(pose.position.z)) return false;
    inverse.x /= length; inverse.y /= length; inverse.z /= length; inverse.w /= length;
    const Vec3 x = Rotate(inverse, {1.0f, 0.0f, 0.0f});
    const Vec3 y = Rotate(inverse, {0.0f, 1.0f, 0.0f});
    const Vec3 z = Rotate(inverse, {0.0f, 0.0f, 1.0f});
    const Vec3 t = Rotate(inverse, {-pose.position.x, -pose.position.y, -pose.position.z});
    const float values[16] = { x.x,x.y,x.z,0, y.x,y.y,y.z,0, z.x,z.y,z.z,0, t.x,t.y,t.z,1 };
    for (int i = 0; i < 16; ++i) view[i] = values[i];
    return true;
}

bool BuildPoseMatrix(const XrPosef& pose, float matrix[16])
{
    Quat q{pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w};
    const float length = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    if (!(length > 0.0f) || !std::isfinite(length) ||
        !std::isfinite(pose.position.x) || !std::isfinite(pose.position.y) ||
        !std::isfinite(pose.position.z)) return false;
    q.x /= length; q.y /= length; q.z /= length; q.w /= length;
    const Vec3 x = Rotate(q, {1.0f, 0.0f, 0.0f});
    const Vec3 y = Rotate(q, {0.0f, 1.0f, 0.0f});
    const Vec3 z = Rotate(q, {0.0f, 0.0f, 1.0f});
    const float values[16] = { x.x,x.y,x.z,0, y.x,y.y,y.z,0, z.x,z.y,z.z,0,
                               pose.position.x,pose.position.y,pose.position.z,1 };
    for (int i = 0; i < 16; ++i) matrix[i] = values[i];
    return true;
}
}

bool GetOpenXrRelativeOrientation(const XrQuaternionf& origin,
                                  const XrQuaternionf& current,
                                  XrQuaternionf& relative)
{
    const float originLength = std::sqrt(origin.x*origin.x + origin.y*origin.y +
        origin.z*origin.z + origin.w*origin.w);
    const float currentLength = std::sqrt(current.x*current.x + current.y*current.y +
        current.z*current.z + current.w*current.w);
    if (!(originLength > 0.0f) || !(currentLength > 0.0f) ||
        !std::isfinite(originLength) || !std::isfinite(currentLength))
        return false;

    const Quat inverseOrigin{-origin.x / originLength, -origin.y / originLength,
                             -origin.z / originLength, origin.w / originLength};
    const Quat normalizedCurrent{current.x / currentLength, current.y / currentLength,
                                 current.z / currentLength, current.w / currentLength};
    Quat result{
        inverseOrigin.w*normalizedCurrent.x + normalizedCurrent.w*inverseOrigin.x +
            inverseOrigin.y*normalizedCurrent.z - inverseOrigin.z*normalizedCurrent.y,
        inverseOrigin.w*normalizedCurrent.y + normalizedCurrent.w*inverseOrigin.y +
            inverseOrigin.z*normalizedCurrent.x - inverseOrigin.x*normalizedCurrent.z,
        inverseOrigin.w*normalizedCurrent.z + normalizedCurrent.w*inverseOrigin.z +
            inverseOrigin.x*normalizedCurrent.y - inverseOrigin.y*normalizedCurrent.x,
        inverseOrigin.w*normalizedCurrent.w - inverseOrigin.x*normalizedCurrent.x -
            inverseOrigin.y*normalizedCurrent.y - inverseOrigin.z*normalizedCurrent.z};
    const float resultLength = std::sqrt(result.x*result.x + result.y*result.y +
        result.z*result.z + result.w*result.w);
    if (!(resultLength > 0.0f) || !std::isfinite(resultLength))
        return false;
    relative = XrQuaternionf{result.x / resultLength, result.y / resultLength,
                             result.z / resultLength, result.w / resultLength};
    return true;
}

bool BuildOpenXrViewProjection(const XrView& view, float nearPlane,
                               float farPlane, float viewProjection[16])
{
    if (!viewProjection || !(nearPlane > 0.0f) || !(farPlane > nearPlane))
        return false;

    float eyeView[16], projection[16];
    if (!BuildEyeView(view.pose, eyeView) ||
        !BuildProjection(view.fov, nearPlane, farPlane, projection)) return false;
    MultiplyColumnMajor(projection, eyeView, viewProjection);
    return true;
}

bool BuildOpenXrEyeMvp(const XrView& eye, const XrPosef& referenceHeadPose,
                       const float referenceModelView[16],
                       float nearPlane, float farPlane, float mvp[16], float eyeModelView[16],
                       bool nearestObject)
{
    if (!referenceModelView || !mvp) return false;
    XrPosef relativeEye{};
    if (!GetOpenXrRelativeOrientation(referenceHeadPose.orientation,
                                      eye.pose.orientation,
                                      relativeEye.orientation))
        return false;

    Quat inverseCurrent{-eye.pose.orientation.x, -eye.pose.orientation.y,
                        -eye.pose.orientation.z, eye.pose.orientation.w};
    const float currentLength = std::sqrt(inverseCurrent.x*inverseCurrent.x +
        inverseCurrent.y*inverseCurrent.y + inverseCurrent.z*inverseCurrent.z +
        inverseCurrent.w*inverseCurrent.w);
    if (!(currentLength > 0.0f) || !std::isfinite(currentLength) ||
        !std::isfinite(referenceHeadPose.position.x) ||
        !std::isfinite(referenceHeadPose.position.y) ||
        !std::isfinite(referenceHeadPose.position.z) ||
        !std::isfinite(eye.pose.position.x) || !std::isfinite(eye.pose.position.y) ||
        !std::isfinite(eye.pose.position.z))
        return false;
    inverseCurrent.x /= currentLength;
    inverseCurrent.y /= currentLength;
    inverseCurrent.z /= currentLength;
    inverseCurrent.w /= currentLength;
    const Vec3 worldDelta{eye.pose.position.x - referenceHeadPose.position.x,
                          eye.pose.position.y - referenceHeadPose.position.y,
                          eye.pose.position.z - referenceHeadPose.position.z};
    const Vec3 eyeLocal = Rotate(inverseCurrent, worldDelta);
    const Quat relativeQ{relativeEye.orientation.x, relativeEye.orientation.y,
                         relativeEye.orientation.z, relativeEye.orientation.w};
    const Vec3 eyeInOrigin = Rotate(relativeQ, eyeLocal);
    relativeEye.position = XrVector3f{eyeInOrigin.x, eyeInOrigin.y, eyeInOrigin.z};

    float eyeView[16], projection[16], viewModel[16];
    if (!BuildEyeView(relativeEye, eyeView) ||
        !BuildProjection(eye.fov, nearPlane, farPlane, projection,
                         1.0f)) return false;
    MultiplyColumnMajor(eyeView, referenceModelView, viewModel);
    MultiplyColumnMajor(projection, viewModel, mvp);
    if (eyeModelView)
        for (int i = 0; i < 16; ++i) eyeModelView[i] = viewModel[i];
    return true;
}

void ApplyOpenXrEyeMatrices(const float eyeView[16], const float eyeViewProjection[16],
                           const float modelView[16], float mvp[16], float viewModel[16])
{
    MultiplyColumnMajor(eyeViewProjection, modelView, mvp);
    MultiplyColumnMajor(eyeView, modelView, viewModel);
}

bool BuildFlatPanelMvp(const XrView& leftView, const XrView& rightView,
                       uint32_t eyeIndex, float aspectRatio, float distanceMeters,
                       float mvp[16])
{
    if (!mvp || eyeIndex > 1 || !(aspectRatio > 0.0f) || !(distanceMeters > 0.0f))
        return false;
    const XrView& eye = eyeIndex == 0 ? leftView : rightView;
    Quat head{leftView.pose.orientation.x, leftView.pose.orientation.y,
              leftView.pose.orientation.z, leftView.pose.orientation.w};
    const float qLength = std::sqrt(head.x*head.x + head.y*head.y + head.z*head.z + head.w*head.w);
    if (!(qLength > 0.0f))
        return false;
    head.x /= qLength; head.y /= qLength; head.z /= qLength; head.w /= qLength;
    const Quat inverseEye{-eye.pose.orientation.x, -eye.pose.orientation.y,
                          -eye.pose.orientation.z, eye.pose.orientation.w};
    const Vec3 headPosition{
        0.5f*(leftView.pose.position.x + rightView.pose.position.x),
        0.5f*(leftView.pose.position.y + rightView.pose.position.y),
        0.5f*(leftView.pose.position.z + rightView.pose.position.z)};
    const Vec3 forward = Rotate(head, {0.0f, 0.0f, -1.0f});
    const Vec3 right = Rotate(head, {1.0f, 0.0f, 0.0f});
    const Vec3 up = Rotate(head, {0.0f, 1.0f, 0.0f});
    const Vec3 panelCenter{headPosition.x + distanceMeters*forward.x,
                           headPosition.y + distanceMeters*forward.y,
                           headPosition.z + distanceMeters*forward.z};
    const Vec3 eyePosition{eye.pose.position.x, eye.pose.position.y, eye.pose.position.z};
    const Vec3 relative{panelCenter.x-eyePosition.x, panelCenter.y-eyePosition.y,
                        panelCenter.z-eyePosition.z};
    const Vec3 translation = Rotate(inverseEye, relative);

    // Keep the menu comfortably readable in VR instead of a small desktop
    // window floating at a distance.
    const float halfHeight = 0.95f;
    const float halfWidth = halfHeight * aspectRatio;
    const Vec3 cameraRight = Rotate(inverseEye, {right.x*halfWidth, right.y*halfWidth, right.z*halfWidth});
    const Vec3 cameraUp = Rotate(inverseEye, {up.x*halfHeight, up.y*halfHeight, up.z*halfHeight});

    const float left = std::tan(eye.fov.angleLeft);
    const float rightFov = std::tan(eye.fov.angleRight);
    const float down = std::tan(eye.fov.angleDown);
    const float upFov = std::tan(eye.fov.angleUp);
    if (!(rightFov > left) || !(upFov > down))
        return false;
    const float nearZ = 0.05f;
    const float farZ = 100.0f;
    const float sx = 2.0f/(rightFov-left);
    const float sy = -2.0f/(upFov-down); // Vulkan framebuffer Y points down.
    const float ox = (rightFov+left)/(rightFov-left);
    const float oy = (upFov+down)/(upFov-down);
    const float sz = farZ/(nearZ-farZ);
    const float tz = farZ*nearZ/(nearZ-farZ);

    // Column-major clip = projection * eyeView * panelModel * vec4(x,y,0,1).
    const Vec3 columns[2] = {cameraRight, cameraUp};
    for (int column = 0; column < 2; ++column)
    {
        const Vec3 v = columns[column];
        const float z = v.z;
        mvp[column*4+0] = sx*v.x + ox*z;
        mvp[column*4+1] = sy*v.y - oy*z;
        mvp[column*4+2] = sz*z;
        mvp[column*4+3] = -z;
    }
    mvp[8] = mvp[9] = mvp[10] = mvp[11] = 0.0f;
    mvp[12] = sx*translation.x + ox*translation.z;
    mvp[13] = sy*translation.y - oy*translation.z;
    mvp[14] = sz*translation.z + tz;
    mvp[15] = -translation.z;
    return true;
}
}
