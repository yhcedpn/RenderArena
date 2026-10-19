#pragma once

#include <filesystem>

namespace arena {

class Application {
public:
    explicit Application(std::filesystem::path executableDirectory);
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    int run();

private:
    struct Impl;
    Impl* implementation_{};
};

} // 命名空间 arena
