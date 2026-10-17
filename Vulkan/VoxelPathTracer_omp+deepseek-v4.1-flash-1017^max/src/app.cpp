#include "app.hpp"

#include "math.hpp"
#include "renderer.hpp"
#include "scene.hpp"
#include "vk_context.hpp"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

namespace vpt {

namespace {

constexpr float kPitchLimit = 89.0f * kPi / 180.0f;
constexpr float kMouseSensitivity = 0.0025f;
constexpr float kMoveSpeed = 3.0f;

Vec3 forwardFromAngles(float yaw, float pitch) {
    return Vec3(std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                -std::cos(pitch) * std::cos(yaw));
}

std::filesystem::path executableDirectory() {
    const char* base = SDL_GetBasePath();
    if (base == nullptr) return std::filesystem::current_path();
    std::filesystem::path path(base);
    SDL_free(const_cast<char*>(base));
    return path;
}

void setPlaceholderTitle(SDL_Window* window) {
    SDL_SetWindowTitle(window, "VoxelPathTracer - -- FPS / -- ms/frame");
}

void setStatsTitle(SDL_Window* window, double fps, double ms) {
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "VoxelPathTracer - %.1f FPS / %.2f ms/frame", fps, ms);
    SDL_SetWindowTitle(window, buffer);
}

} // namespace

int runApp() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL 错误: 初始化失败: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow(
        "VoxelPathTracer", 1280, 720,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window == nullptr) {
        std::fprintf(stderr, "SDL 错误: 创建窗口失败: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    setPlaceholderTitle(window);

    const std::filesystem::path exeDir = executableDirectory();
    VulkanContext context;
#ifdef NDEBUG
    const bool debugBuild = false;
#else
    const bool debugBuild = true;
#endif
    if (!context.init(window, debugBuild)) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SceneData scene;
    try {
        scene = loadScene(exeDir / "materials.json");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "场景错误: %s\n", e.what());
        context.shutdown();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    Renderer renderer;
    if (!renderer.init(context, std::move(scene), window, exeDir)) {
        std::fprintf(stderr, "渲染器初始化失败\n");
        renderer.shutdown();
        context.shutdown();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    CameraState camera;
    bool resetRequested = false;
    bool running = true;
    bool mouseCaptured = false;
    std::uint64_t lastCounter = SDL_GetPerformanceCounter();
    const std::uint64_t frequency = SDL_GetPerformanceFrequency();

    std::uint64_t statsStart = SDL_GetPerformanceCounter();
    std::uint64_t statsFrames = 0;
    bool statsFresh = true;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.key == SDLK_ESCAPE) {
                    if (mouseCaptured) {
                        SDL_SetWindowRelativeMouseMode(window, false);
                        mouseCaptured = false;
                    } else {
                        running = false;
                    }
                } else if (event.key.key == SDLK_R && !event.key.repeat) {
                    camera = CameraState{};
                    resetRequested = true;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (event.button.button == SDL_BUTTON_RIGHT) {
                    SDL_SetWindowRelativeMouseMode(window, true);
                    mouseCaptured = true;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (event.button.button == SDL_BUTTON_RIGHT) {
                    SDL_SetWindowRelativeMouseMode(window, false);
                    mouseCaptured = false;
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (mouseCaptured) {
                    camera.yaw += event.motion.xrel * kMouseSensitivity;
                    camera.pitch -= event.motion.yrel * kMouseSensitivity;
                    camera.pitch = std::clamp(camera.pitch, -kPitchLimit, kPitchLimit);
                }
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                if (mouseCaptured) {
                    SDL_SetWindowRelativeMouseMode(window, false);
                    mouseCaptured = false;
                }
                lastCounter = SDL_GetPerformanceCounter();
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                lastCounter = SDL_GetPerformanceCounter();
                break;
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                renderer.requestResize();
                break;
            case SDL_EVENT_WINDOW_MINIMIZED:
                lastCounter = SDL_GetPerformanceCounter();
                break;
            default:
                break;
            }
        }

        const std::uint64_t now = SDL_GetPerformanceCounter();
        float deltaSeconds = static_cast<float>(now - lastCounter) /
                             static_cast<float>(frequency);
        lastCounter = now;
        if (!(deltaSeconds > 0.0f)) deltaSeconds = 0.0f;
        deltaSeconds = std::min(deltaSeconds, 0.1f);

        const bool* keys = SDL_GetKeyboardState(nullptr);
        const Vec3 forward = forwardFromAngles(camera.yaw, camera.pitch);
        const Vec3 flatForward = normalize(Vec3(forward.x, 0.0f, forward.z));
        const Vec3 right = normalize(cross(flatForward, Vec3(0.0f, 1.0f, 0.0f)));
        Vec3 move(0.0f, 0.0f, 0.0f);
        if (keys[SDL_SCANCODE_W]) move += flatForward;
        if (keys[SDL_SCANCODE_S]) move -= flatForward;
        if (keys[SDL_SCANCODE_D]) move += right;
        if (keys[SDL_SCANCODE_A]) move -= right;
        if (keys[SDL_SCANCODE_E]) move += Vec3(0.0f, 1.0f, 0.0f);
        if (keys[SDL_SCANCODE_Q]) move -= Vec3(0.0f, 1.0f, 0.0f);
        if (length(move) > 0.0f) {
            const float speed = kMoveSpeed * ((keys[SDL_SCANCODE_LSHIFT] ||
                                               keys[SDL_SCANCODE_RSHIFT]) ? 3.0f : 1.0f);
            camera.position += normalize(move) * (speed * deltaSeconds);
        }

        const FrameStatus status = renderer.renderFrame(camera, resetRequested);
        resetRequested = false;
        if (renderer.takeStatsReset()) {
            statsStart = SDL_GetPerformanceCounter();
            statsFrames = 0;
            statsFresh = true;
            setPlaceholderTitle(window);
        }
        if (status == FrameStatus::Fatal) break;
        if (status == FrameStatus::Presented) {
            ++statsFrames;
            const double elapsed = static_cast<double>(SDL_GetPerformanceCounter() - statsStart) /
                                   static_cast<double>(frequency);
            if (elapsed >= 0.5) {
                const double fps = static_cast<double>(statsFrames) / elapsed;
                setStatsTitle(window, fps, fps > 0.0 ? 1000.0 / fps : 0.0);
                statsStart = SDL_GetPerformanceCounter();
                statsFrames = 0;
                statsFresh = false;
            }
        }
        if (statsFresh && statsFrames == 0) setPlaceholderTitle(window);

        if (g_validationFailure.load(std::memory_order_relaxed)) {
            std::fprintf(stderr, "Vulkan 验证层报告 warning/error，按失败退出\n");
            running = false;
        }
    }

    renderer.shutdown();
    context.shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return g_validationFailure.load(std::memory_order_relaxed) ? 1 : 0;
}

} // namespace vpt
