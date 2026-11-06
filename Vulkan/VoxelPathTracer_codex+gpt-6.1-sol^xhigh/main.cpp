#include "src/renderer.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <stdexcept>

int main(){
 SDL_Window* window=nullptr;int result=0;
 try{
  if(!SDL_Init(SDL_INIT_VIDEO))throw std::runtime_error(SDL_GetError());
  const char* path=SDL_GetBasePath();if(!path)throw std::runtime_error(SDL_GetError());arena::Scene scene{std::filesystem::path(path)/"materials.json"};
  window=SDL_CreateWindow("VoxelPathTracer | FPS -- | -- ms/frame",1280,720,SDL_WINDOW_VULKAN|SDL_WINDOW_RESIZABLE|SDL_WINDOW_HIGH_PIXEL_DENSITY);if(!window)throw std::runtime_error(SDL_GetError());
  {
   arena::Renderer renderer(window,std::filesystem::path(path),scene);arena::Camera camera;bool running=true,captured=false,cut=true,paused=false,focused=true;
   using Clock=std::chrono::steady_clock;auto previous=Clock::now(),statisticsStart=previous;uint32_t presented=0;
   auto resetStatistics=[&](){statisticsStart=Clock::now();presented=0;SDL_SetWindowTitle(window,"VoxelPathTracer | FPS -- | -- ms/frame");};
   auto capture=[&](bool enabled){if(!SDL_SetWindowRelativeMouseMode(window,enabled))throw std::runtime_error(SDL_GetError());captured=enabled;};
   while(running){
    SDL_Event e;while(SDL_PollEvent(&e)){
     if(e.type==SDL_EVENT_QUIT||e.type==SDL_EVENT_WINDOW_CLOSE_REQUESTED)running=false;
     if(e.type==SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED){renderer.rebuild();resetStatistics();previous=Clock::now();}
     if(e.type==SDL_EVENT_WINDOW_MINIMIZED){paused=true;capture(false);resetStatistics();}
     if(e.type==SDL_EVENT_WINDOW_RESTORED){paused=false;renderer.rebuild();previous=Clock::now();resetStatistics();}
     if(e.type==SDL_EVENT_WINDOW_FOCUS_LOST){focused=false;capture(false);previous=Clock::now();}
     if(e.type==SDL_EVENT_WINDOW_FOCUS_GAINED){focused=true;previous=Clock::now();}
     if(e.type==SDL_EVENT_MOUSE_BUTTON_DOWN&&e.button.button==SDL_BUTTON_RIGHT)capture(true);
     if(e.type==SDL_EVENT_MOUSE_BUTTON_UP&&e.button.button==SDL_BUTTON_RIGHT)capture(false);
     if(e.type==SDL_EVENT_MOUSE_MOTION&&captured){camera.yaw+=e.motion.xrel*0.0025f;camera.pitch=std::clamp(camera.pitch-e.motion.yrel*0.0025f,-1.553343034f,1.553343034f);}
     if(e.type==SDL_EVENT_KEY_DOWN&&!e.key.repeat){if(e.key.key==SDLK_ESCAPE){if(captured)capture(false);else running=false;}if(e.key.key==SDLK_R){camera=arena::Camera();cut=true;}}
    }
    if(!running)break;int w=0,h=0;if(!SDL_GetWindowSizeInPixels(window,&w,&h))throw std::runtime_error(SDL_GetError());if(paused||w<=0||h<=0){SDL_Delay(20);previous=Clock::now();resetStatistics();continue;}
    auto now=Clock::now();float dt=std::chrono::duration<float>(now-previous).count();previous=now;
    if(focused){const bool* keys=SDL_GetKeyboardState(nullptr);float speed=3*dt*((keys[SDL_SCANCODE_LSHIFT]||keys[SDL_SCANCODE_RSHIFT])?3.0f:1.0f);arena::V3 right=camera.right(),forward={right.z,0,-right.x};arena::V3 move{};if(keys[SDL_SCANCODE_W])move=move+forward;if(keys[SDL_SCANCODE_S])move=move-forward;if(keys[SDL_SCANCODE_D])move=move+right;if(keys[SDL_SCANCODE_A])move=move-right;if(keys[SDL_SCANCODE_E])move.y+=1;if(keys[SDL_SCANCODE_Q])move.y-=1;if(arena::dot(move,move)>0)move=arena::normalize(move);camera.eye=camera.eye+move*speed;}
    bool successful=renderer.frame(camera,cut);if(successful){if(cut)std::cout<<"First/reset present completed\n"<<std::flush;cut=false;++presented;double elapsed=std::chrono::duration<double>(Clock::now()-statisticsStart).count();if(elapsed>=0.5){char title[128];std::snprintf(title,sizeof(title),"VoxelPathTracer | %.2f FPS | %.2f ms/frame",static_cast<double>(presented)/elapsed,elapsed*1000/static_cast<double>(presented));SDL_SetWindowTitle(window,title);statisticsStart=Clock::now();presented=0;}}
    else {previous=Clock::now();resetStatistics();}
   }
   capture(false);if(renderer.diagnostics())result=1;
  }
 }catch(const std::exception& e){std::cerr<<"VoxelPathTracer failure: "<<e.what()<<'\n';result=1;}
 if(window)SDL_DestroyWindow(window);SDL_Quit();return result;
}


