#include "src/renderer.hpp"
#include <exception>
#include <iostream>
int main(){try{Renderer renderer;renderer.run();return 0;}catch(const std::exception& e){std::cerr<<"VoxelPathTracer: "<<e.what()<<'\n';return 1;}}
