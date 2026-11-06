#include "camera.hpp"

#include <algorithm>
#include <cmath>

namespace voxelpt
{
namespace
{
constexpr float pi = 3.14159265358979323846F;
constexpr float nearPlane = 0.05F;

[[nodiscard]] std::array<float, 3> add(const std::array<float, 3>& a, const std::array<float, 3>& b)
{
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

[[nodiscard]] std::array<float, 3> multiply(const std::array<float, 3>& value, float scalar)
{
    return {value[0] * scalar, value[1] * scalar, value[2] * scalar};
}

[[nodiscard]] float dot(const std::array<float, 3>& a, const std::array<float, 3>& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

[[nodiscard]] std::array<float, 3> normalize(const std::array<float, 3>& value)
{
    const float length = std::sqrt(dot(value, value));
    return multiply(value, 1.0F / length);
}

}

Camera::Camera()
{
    reset();
}

void Camera::reset()
{
    position_ = {8.0F, 2.5F, 20.0F};
    yaw_ = pi;
    pitch_ = std::asin(-0.5F / std::sqrt(14.0F * 14.0F + 0.5F * 0.5F));
}

void Camera::rotate(float mouseDeltaX, float mouseDeltaY)
{
    constexpr float sensitivity = 0.0025F;
    yaw_ += mouseDeltaX * sensitivity;
    pitch_ = std::clamp(pitch_ - mouseDeltaY * sensitivity, -89.0F * pi / 180.0F, 89.0F * pi / 180.0F);
}

void Camera::move(float forwardAxis, float rightAxis, float verticalAxis, float elapsedSeconds, bool fast)
{
    const float horizontalLength = std::sqrt(forwardAxis * forwardAxis + rightAxis * rightAxis);
    if (horizontalLength > 1.0F)
    {
        forwardAxis /= horizontalLength;
        rightAxis /= horizontalLength;
    }
    const auto state = snapshot();
    const std::array<float, 3> horizontalForward{state.forward[0], 0.0F, state.forward[2]};
    const float forwardLength = std::sqrt(dot(horizontalForward, horizontalForward));
    const auto forward = multiply(horizontalForward, 1.0F / forwardLength);
    const std::array<float, 3> horizontalRight{state.right[0], 0.0F, state.right[2]};
    const float speed = 3.0F * (fast ? 3.0F : 1.0F) * std::max(elapsedSeconds, 0.0F);
    auto displacement = multiply(forward, forwardAxis * speed);
    displacement = add(displacement, multiply(horizontalRight, rightAxis * speed));
    displacement[1] += verticalAxis * speed;
    position_ = add(position_, displacement);
}

CameraSnapshot Camera::snapshot() const
{
    const float cosPitch = std::cos(pitch_);
    const auto forward = normalize({cosPitch * std::sin(yaw_), std::sin(pitch_), cosPitch * std::cos(yaw_)});
    const auto right = normalize({-std::cos(yaw_), 0.0F, std::sin(yaw_)});
    const std::array<float, 3> up{
        right[1] * forward[2] - right[2] * forward[1],
        right[2] * forward[0] - right[0] * forward[2],
        right[0] * forward[1] - right[1] * forward[0]
    };
    return {position_, forward, right, up, 60.0F * pi / 180.0F};
}

Ray Camera::primaryRay(float ndcX, float ndcY, float framebufferAspect) const
{
    const auto state = snapshot();
    const float halfHeight = std::tan(state.verticalFovRadians * 0.5F);
    const float halfWidth = halfHeight * framebufferAspect;
    const auto direction = normalize(add(state.forward, add(multiply(state.right, ndcX * halfWidth),
                                                                multiply(state.up, ndcY * halfHeight))));
    return {position_, direction, nearPlane / dot(direction, state.forward)};
}

bool Camera::sameProjectionPose(const CameraSnapshot& other) const
{
    const auto current = snapshot();
    return current.position == other.position && current.forward == other.forward &&
           current.verticalFovRadians == other.verticalFovRadians;
}
}
