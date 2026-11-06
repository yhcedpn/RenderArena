#include "src/vulkan_app.hpp"

#include <exception>
#include <iostream>

int main() {
    try {
        vpt::VulkanApp app;
        return app.run();
    } catch (const std::exception& exception) {
        std::cerr << "VoxelPathTracer: " << exception.what() << '\n';
        return 1;
    }
}
