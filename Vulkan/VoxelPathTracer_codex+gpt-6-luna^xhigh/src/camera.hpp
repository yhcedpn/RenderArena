#pragma once

#include <array>

namespace voxelpt
{
struct Ray
{
    std::array<float, 3> origin{};
    std::array<float, 3> direction{};
    float minimumDistance{};
};

struct CameraSnapshot
{
    std::array<float, 3> position{};
    std::array<float, 3> forward{};
    std::array<float, 3> right{};
    std::array<float, 3> up{};
    float verticalFovRadians{};
};

class Camera
{
public:
    Camera();

    void reset();
    void rotate(float mouseDeltaX, float mouseDeltaY);
    void move(float forwardAxis, float rightAxis, float verticalAxis, float elapsedSeconds, bool fast);

    [[nodiscard]] CameraSnapshot snapshot() const;
    [[nodiscard]] Ray primaryRay(float ndcX, float ndcY, float framebufferAspect) const;
    [[nodiscard]] bool sameProjectionPose(const CameraSnapshot& other) const;

private:
    std::array<float, 3> position_{};
    float yaw_{};
    float pitch_{};
};
}
