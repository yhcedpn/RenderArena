#pragma once

#include <memory>

namespace vpt {

class VulkanApp {
public:
    VulkanApp();
    ~VulkanApp();
    VulkanApp(const VulkanApp&) = delete;
    VulkanApp& operator=(const VulkanApp&) = delete;

    int run();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vpt
