#pragma once
#include "scene.hpp"
#include <memory>
struct SDL_Window;
namespace arena {
struct Camera {V3 eye{8,2.5f,20};float yaw{},pitch{};Camera();V3 forward()const;V3 right()const;V3 up()const;};
class Renderer {
 struct Impl;std::unique_ptr<Impl> impl;
public:
 Renderer(SDL_Window* window,const std::filesystem::path& base,const Scene& scene);
 ~Renderer();bool frame(const Camera& camera,bool cut);void rebuild();bool diagnostics()const;
};
}
