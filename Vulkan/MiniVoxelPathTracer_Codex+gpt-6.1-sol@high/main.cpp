#define VK_NO_PROTOTYPES
#include <volk.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <shaderc/shaderc.hpp>
#include "scene.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <chrono>
#include <atomic>
#include <functional>
#include <memory>
#include <cstdlib>
template<class T> T vkinit(VkStructureType type){T t{};
t.sType=type;
return t;
}
void check(VkResult r,const char* what){if(r!=VK_SUCCESS)throw std::runtime_error(std::string(what)+": VkResult="+std::to_string(r));
}
uint64_t alignUp(uint64_t n,uint64_t a){return (n+a-1)/a*a;
}
struct ScopeExit {
 std::function<void()> action;
 ~ScopeExit(){if(action)action();
 }
 void dismiss(){action={};
 }
};
struct Buffer {VkBuffer handle{};
VkDeviceMemory memory{};
VkDeviceAddress address{};
VkDeviceSize size{};
void* mapped{};
};
struct Image {VkImage handle{};
VkDeviceMemory memory{};
VkImageView view{};
};
struct AS {VkAccelerationStructureKHR handle{};
Buffer buffer;
VkDeviceAddress address{};
};
struct FrameGPU {F4 cam[8]{};
U4 extent;
F4 jitter;
U4 settings;
};
static_assert(sizeof(FrameGPU)==176 && sizeof(MaterialGPU)==64 && sizeof(Triangle)==80 && sizeof(Light)==16);
static_assert(sizeof(VkTraceRaysIndirectCommandKHR)==12);
struct Push {uint32_t q{},bounce{},pass{},step{};
};
struct Slot {VkCommandPool pool{};
VkCommandBuffer command{};
VkSemaphore acquire{};
uint64_t done{};
Buffer frame,descriptor;
};
struct Camera {
 V3 position{8,2.5f,20};
 float yaw=0,pitch=-0.0356991f;
 V3 forward()const{return {std::sin(yaw)*std::cos(pitch),std::sin(pitch),-std::cos(yaw)*std::cos(pitch)};
 }
 V3 right()const{return {std::cos(yaw),0,std::sin(yaw)};
 }
 V3 up()const{return cross(right(),forward());
 }
};
class Includes final:public shaderc::CompileOptions::IncluderInterface{
 std::filesystem::path dir;
 struct Data{std::string name,text;
 shaderc_include_result result{};
 };
 public:explicit Includes(std::filesystem::path d):dir(std::move(d)){}
 shaderc_include_result* GetInclude(const char* requested,shaderc_include_type,const char*,size_t)override{
  auto* d=new Data;
  d->name=requested;
  std::ifstream in(dir/d->name);
  std::ostringstream s;
  s<<in.rdbuf();
  d->text=in?s.str():"";
  d->result.source_name=d->name.c_str();
  d->result.source_name_length=d->name.size();
  d->result.content=d->text.c_str();
  d->result.content_length=d->text.size();
  d->result.user_data=d;
  return &d->result;
 }
 void ReleaseInclude(shaderc_include_result* r)override{delete static_cast<Data*>(r->user_data);
 }
};
class App {
 public:
 std::filesystem::path root;
 Scene scene;
 GLFWwindow* window{};
 VkInstance instance{};
 VkDebugUtilsMessengerEXT messenger{};
 VkSurfaceKHR surface{};
 VkPhysicalDevice physical{};
 VkDevice device{};
 VkQueue queue{};
 uint32_t family{};
 VkPhysicalDeviceProperties properties{};
 VkPhysicalDeviceMemoryProperties memoryProperties{};
 VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptorProps=vkinit<VkPhysicalDeviceDescriptorBufferPropertiesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT);
 VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps=vkinit<VkPhysicalDeviceRayTracingPipelinePropertiesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR);
 VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps=vkinit<VkPhysicalDeviceAccelerationStructurePropertiesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR);
 VkCommandPool initPool{};
 VkCommandBuffer initCmd{};
 VkSemaphore timeline{};
 uint64_t submitted{};
 VkDescriptorSetLayout setLayout{};
 VkPipelineLayout layout{};
 VkDeviceSize descriptorSize{},bindingOffsets[26]{};
 VkPipeline rtPipeline{};
 Buffer sbt;
 VkStridedDeviceAddressRegionKHR raygenRegion{},missRegion{},hitRegion{},callRegion{};
 std::map<std::string,std::vector<uint32_t>> binaries;
 std::map<std::string,VkShaderEXT> shaders;
 Buffer geometry,materials,lightTable,vertices;
 AS blas,glassBlas,tlas;
 Image colorTexture,roughTexture;
 VkSampler sampler{};
 Slot slots[2];
 uint32_t slotIndex{};
 VkSwapchainKHR swapchain{};
 VkExtent2D extent{};
 VkFormat format{};
 std::vector<VkImage> swapImages;
 std::vector<VkImageView> swapViews;
 std::vector<VkSemaphore> presentSemaphores;
 std::vector<bool> presented;
 Buffer frameBuffers[19];
 Camera camera,previousCamera;
 F4 previousJitter{};
 uint32_t frameNumber{};
 bool reset=true,captured=false,resize=true,rHeld=false;
 double mouseX{},mouseY{};
 bool mouseInit=false,wasFocused=true,wasRenderable=true,escapeHeld=false,captureSuppressed=false;
 std::atomic<bool> failed{false};
 explicit App(std::filesystem::path r):root(std::move(r)){}
 ~App(){destroy();
 }
#ifndef NDEBUG
 static VKAPI_ATTR VkBool32 VKAPI_CALL debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* d,void* user){
  auto& app=*static_cast<App*>(user);
  std::cerr<<"Vulkan: "<<d->pMessage<<"\n";
  if(severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)app.failed=true;
  return VK_FALSE;
 }
#endif
 uint32_t memoryType(uint32_t bits,VkMemoryPropertyFlags wanted){
  for(uint32_t i=0;i<memoryProperties.memoryTypeCount;i++)if((bits&(1u<<i))&&(memoryProperties.memoryTypes[i].propertyFlags&wanted)==wanted)return i;
  throw std::runtime_error("没有所需的 Vulkan 内存类型");
 }
 Buffer buffer(VkDeviceSize size,VkBufferUsageFlags usage,bool host=false){
  Buffer b{};
  ScopeExit cleanup{[&]{release(b);
  }};
  b.size=size;
  auto info=vkinit<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
  info.size=size;
  info.usage=usage|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
  check(vkCreateBuffer(device,&info,nullptr,&b.handle),"vkCreateBuffer");
  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(device,b.handle,&req);
  auto flags=vkinit<VkMemoryAllocateFlagsInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO);
  flags.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
  auto alloc=vkinit<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
  alloc.pNext=&flags;
  alloc.allocationSize=req.size;
  alloc.memoryTypeIndex=memoryType(req.memoryTypeBits,host?VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT:VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  check(vkAllocateMemory(device,&alloc,nullptr,&b.memory),"vkAllocateMemory");
  check(vkBindBufferMemory(device,b.handle,b.memory,0),"vkBindBufferMemory");
  auto address=vkinit<VkBufferDeviceAddressInfo>(VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO);
  address.buffer=b.handle;
  b.address=vkGetBufferDeviceAddress(device,&address);
  if(host)check(vkMapMemory(device,b.memory,0,VK_WHOLE_SIZE,0,&b.mapped),"vkMapMemory");
  cleanup.dismiss();
  return b;
 }
 void release(Buffer& b){if(b.mapped)vkUnmapMemory(device,b.memory);
 if(b.handle)vkDestroyBuffer(device,b.handle,nullptr);
 if(b.memory)vkFreeMemory(device,b.memory,nullptr);
 b={};
 }
 void release(Image& i){if(i.view)vkDestroyImageView(device,i.view,nullptr);
 if(i.handle)vkDestroyImage(device,i.handle,nullptr);
 if(i.memory)vkFreeMemory(device,i.memory,nullptr);
 i={};
 }
 void release(AS& a){if(a.handle)vkDestroyAccelerationStructureKHR(device,a.handle,nullptr);
 release(a.buffer);
 a={};
 }
 void wait(uint64_t value){if(value==0)return;
 auto w=vkinit<VkSemaphoreWaitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO);
 w.semaphoreCount=1;
 w.pSemaphores=&timeline;
 w.pValues=&value;
 check(vkWaitSemaphores(device,&w,UINT64_MAX),"vkWaitSemaphores");
 }
 void beginInit(){check(vkResetCommandPool(device,initPool,0),"vkResetCommandPool");
 auto b=vkinit<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
 b.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
 check(vkBeginCommandBuffer(initCmd,&b),"vkBeginCommandBuffer");
 }
 void endInit(){
  check(vkEndCommandBuffer(initCmd),"vkEndCommandBuffer");
  auto c=vkinit<VkCommandBufferSubmitInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO);
  c.commandBuffer=initCmd;
  auto s=vkinit<VkSemaphoreSubmitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO);
  s.semaphore=timeline;
  s.value=++submitted;
  s.stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  auto sub=vkinit<VkSubmitInfo2>(VK_STRUCTURE_TYPE_SUBMIT_INFO_2);
  sub.commandBufferInfoCount=1;
  sub.pCommandBufferInfos=&c;
  sub.signalSemaphoreInfoCount=1;
  sub.pSignalSemaphoreInfos=&s;
  check(vkQueueSubmit2(queue,1,&sub,VK_NULL_HANDLE),"vkQueueSubmit2 初始化");
  wait(submitted);
 }
 static void barrier(VkCommandBuffer c,VkPipelineStageFlags2 src=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VkAccessFlags2 sa=VK_ACCESS_2_MEMORY_WRITE_BIT,VkPipelineStageFlags2 dst=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VkAccessFlags2 da=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT){
  auto m=vkinit<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
  m.srcStageMask=src;
  m.srcAccessMask=sa;
  m.dstStageMask=dst;
  m.dstAccessMask=da;
  auto d=vkinit<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
  d.memoryBarrierCount=1;
  d.pMemoryBarriers=&m;
  vkCmdPipelineBarrier2(c,&d);
 }
 static void imageBarrier(VkCommandBuffer c,VkImage image,VkImageLayout old,VkImageLayout next,VkPipelineStageFlags2 src,VkAccessFlags2 sa,VkPipelineStageFlags2 dst,VkAccessFlags2 da,uint32_t levels=1,uint32_t layers=1){
  auto b=vkinit<VkImageMemoryBarrier2>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2);
  b.srcStageMask=src;
  b.srcAccessMask=sa;
  b.dstStageMask=dst;
  b.dstAccessMask=da;
  b.oldLayout=old;
  b.newLayout=next;
  b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
  b.image=image;
  b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,levels,0,layers};
  auto dep=vkinit<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
  dep.imageMemoryBarrierCount=1;
  dep.pImageMemoryBarriers=&b;
  vkCmdPipelineBarrier2(c,&dep);
 }
 template<class T> Buffer upload(const std::vector<T>& data,VkBufferUsageFlags usage){
  VkDeviceSize size=data.size()*sizeof(T);
  Buffer staging=buffer(size,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,true);
  ScopeExit stagingCleanup{[&]{release(staging);
  }};
  Buffer out=buffer(size,usage|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  ScopeExit outputCleanup{[&]{release(out);
  }};
  std::memcpy(staging.mapped,data.data(),size);
  beginInit();
  barrier(initCmd,VK_PIPELINE_STAGE_2_HOST_BIT,VK_ACCESS_2_HOST_WRITE_BIT,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
  VkBufferCopy cp{0,0,size};
  vkCmdCopyBuffer(initCmd,staging.handle,out.handle,1,&cp);
  barrier(initCmd,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT);
  endInit();
  release(staging);
  outputCleanup.dismiss();
  return out;
 }
 void compile(){
  shaderc::Compiler compiler;
  shaderc::CompileOptions options;
  options.SetTargetEnvironment(shaderc_target_env_vulkan,shaderc_env_version_vulkan_1_4);
  options.SetTargetSpirv(shaderc_spirv_version_1_6);
  options.SetWarningsAsErrors();
  options.SetOptimizationLevel(shaderc_optimization_level_performance);
  options.SetIncluder(std::make_unique<Includes>(root/"shaders"));
  const std::array<std::pair<const char*,shaderc_shader_kind>,9> list={{{"trace.rgen",shaderc_raygen_shader},{"hit.rchit",shaderc_closesthit_shader},{"miss.rmiss",shaderc_miss_shader},{"wave.comp",shaderc_compute_shader},{"restir.comp",shaderc_compute_shader},{"shadow_resolve.comp",shaderc_compute_shader},{"svgf.comp",shaderc_compute_shader},{"display.vert",shaderc_vertex_shader},{"display.frag",shaderc_fragment_shader}}};
  for(auto [file,kind]:list){std::ifstream in(root/"shaders"/file);
  if(!in)throw std::runtime_error(std::string("shader 无法读取: ")+file);
  std::ostringstream text;
  text<<in.rdbuf();
  auto result=compiler.CompileGlslToSpv(text.str(),kind,file,options);
  if(result.GetCompilationStatus()!=shaderc_compilation_status_success||!result.GetErrorMessage().empty())throw std::runtime_error(std::string(file)+": "+result.GetErrorMessage());
  binaries[file]={result.cbegin(),result.cend()};
  std::cout<<"shader 编译通过: "<<file<<"\n";
  }
 }
 void initialize(){
  scene.load(root/"materials.json");
  scene.textures();
  scene.mesh();
  compile();
  if(!glfwInit())throw std::runtime_error("GLFW 初始化失败");
  glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API);
  window=glfwCreateWindow(1280,720,"MiniVoxelPathTracer | -- FPS | -- ms/frame",nullptr,nullptr);
  if(!window)throw std::runtime_error("窗口创建失败");
  glfwSetWindowUserPointer(window,this);
  glfwSetFramebufferSizeCallback(window,[](GLFWwindow* w,int,int){static_cast<App*>(glfwGetWindowUserPointer(w))->resize=true;});
  check(volkInitialize(),"volkInitialize");
  uint32_t version{};
  check(vkEnumerateInstanceVersion(&version),"vkEnumerateInstanceVersion");
  if(version<VK_API_VERSION_1_4)throw std::runtime_error("loader 缺少 Vulkan 1.4");
  uint32_t extensionCount{};
  const char** glfwExtensions=glfwGetRequiredInstanceExtensions(&extensionCount);
  std::vector<const char*> extensions(glfwExtensions,glfwExtensions+extensionCount);
  auto app=vkinit<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
  app.pApplicationName="MiniVoxelPathTracer";
  app.apiVersion=VK_API_VERSION_1_4;
  auto ci=vkinit<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
#ifndef NDEBUG
  extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  uint32_t layerCount{};
  check(vkEnumerateInstanceLayerProperties(&layerCount,nullptr),"instance layers");
  std::vector<VkLayerProperties> layers(layerCount);
  check(vkEnumerateInstanceLayerProperties(&layerCount,layers.data()),"instance layers");
  bool found=std::any_of(layers.begin(),layers.end(),[](const auto& l){return std::strcmp(l.layerName,"VK_LAYER_KHRONOS_validation")==0;});
  if(!found)throw std::runtime_error("缺少 VK_LAYER_KHRONOS_validation");
  const char* validationLayer="VK_LAYER_KHRONOS_validation";
  auto debugInfo=vkinit<VkDebugUtilsMessengerCreateInfoEXT>(VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT);
  debugInfo.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  debugInfo.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
  debugInfo.pfnUserCallback=debug;
  debugInfo.pUserData=this;
  VkValidationFeatureEnableEXT sync=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
  auto validation=vkinit<VkValidationFeaturesEXT>(VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT);
  validation.enabledValidationFeatureCount=1;
  validation.pEnabledValidationFeatures=&sync;
  validation.pNext=&debugInfo;
  ci.pNext=&validation;
  ci.enabledLayerCount=1;
  ci.ppEnabledLayerNames=&validationLayer;
#endif
  ci.pApplicationInfo=&app;
  ci.enabledExtensionCount=uint32_t(extensions.size());
  ci.ppEnabledExtensionNames=extensions.data();
  check(vkCreateInstance(&ci,nullptr,&instance),"vkCreateInstance");
  volkLoadInstance(instance);
#ifndef NDEBUG
  check(vkCreateDebugUtilsMessengerEXT(instance,&debugInfo,nullptr,&messenger),"debug messenger");
#endif
  check(glfwCreateWindowSurface(instance,window,nullptr,&surface),"GLFW surface");
  selectDevice();
  auto type=vkinit<VkSemaphoreTypeCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);
  type.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;
  auto sem=vkinit<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
  sem.pNext=&type;
  check(vkCreateSemaphore(device,&sem,nullptr,&timeline),"timeline");
  auto pool=vkinit<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
  pool.queueFamilyIndex=family;
  pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  check(vkCreateCommandPool(device,&pool,nullptr,&initPool),"init command pool");
  auto ca=vkinit<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
  ca.commandPool=initPool;
  ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ca.commandBufferCount=1;
  check(vkAllocateCommandBuffers(device,&ca,&initCmd),"init command");
  createScene();
  createLayouts();
  createPrograms();
  if(failed)throw std::runtime_error("初始化 validation 失败");
  for(auto& slot:slots){check(vkCreateCommandPool(device,&pool,nullptr,&slot.pool),"frame pool");
  ca.commandPool=slot.pool;
  check(vkAllocateCommandBuffers(device,&ca,&slot.command),"frame command");
  sem.pNext=nullptr;
  check(vkCreateSemaphore(device,&sem,nullptr,&slot.acquire),"acquire");
  slot.frame=buffer(sizeof(FrameGPU),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,true);
  slot.descriptor=buffer(descriptorSize,VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT|VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT,true);
  }
  rebuild();
 }
 void selectDevice();
 void createScene();
 void createLayouts();
 void createPrograms();
 void rebuild();
 void writeDescriptors(Slot& slot);
 void record(Slot& slot,uint32_t image);
 void run();
 void destroy();
};
void App::selectDevice(){
 uint32_t count{};
 check(vkEnumeratePhysicalDevices(instance,&count,nullptr),"physical devices");
 std::vector<VkPhysicalDevice> devices(count);
 check(vkEnumeratePhysicalDevices(instance,&count,devices.data()),"physical devices");
 const std::vector<const char*> required={VK_KHR_SWAPCHAIN_EXTENSION_NAME,VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,VK_EXT_SHADER_OBJECT_EXTENSION_NAME,VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME};
 for(VkPhysicalDevice candidate:devices){
  VkPhysicalDeviceProperties prop{};
  vkGetPhysicalDeviceProperties(candidate,&prop);
  std::vector<std::string> missing;
  if(prop.apiVersion<VK_API_VERSION_1_4)missing.emplace_back("Vulkan 1.4");
  uint32_t ec{};
  check(vkEnumerateDeviceExtensionProperties(candidate,nullptr,&ec,nullptr),"device extensions");
  std::vector<VkExtensionProperties> ext(ec);
  check(vkEnumerateDeviceExtensionProperties(candidate,nullptr,&ec,ext.data()),"device extensions");
  for(const char* e:required)if(!std::any_of(ext.begin(),ext.end(),[e](const auto& x){return std::strcmp(x.extensionName,e)==0;}))missing.emplace_back(e);
  auto f12=vkinit<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
  auto f13=vkinit<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
  auto fa=vkinit<VkPhysicalDeviceAccelerationStructureFeaturesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR);
  auto fr=vkinit<VkPhysicalDeviceRayTracingPipelineFeaturesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR);
  auto fd=vkinit<VkPhysicalDeviceDescriptorBufferFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT);
  auto fs=vkinit<VkPhysicalDeviceShaderObjectFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT);
  auto fe=vkinit<VkPhysicalDeviceExtendedDynamicState3FeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT);
  auto fv=vkinit<VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT);
  auto feature=vkinit<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
  feature.pNext=&f12;
  f12.pNext=&f13;
  f13.pNext=&fa;
  fa.pNext=&fr;
  fr.pNext=&fd;
  fd.pNext=&fs;
  fs.pNext=&fe;
  fe.pNext=&fv;
  vkGetPhysicalDeviceFeatures2(candidate,&feature);
  auto need=[&](VkBool32 value,const char* name){if(!value)missing.emplace_back(name);
  };
  need(f12.bufferDeviceAddress,"bufferDeviceAddress");
  need(f12.scalarBlockLayout,"scalarBlockLayout");
  need(f12.timelineSemaphore,"timelineSemaphore");
  need(f13.dynamicRendering,"dynamicRendering");
  need(f13.synchronization2,"synchronization2");
  need(f13.maintenance4,"maintenance4");
  need(fa.accelerationStructure,"accelerationStructure");
  need(fr.rayTracingPipeline,"rayTracingPipeline");
  need(fr.rayTracingPipelineTraceRaysIndirect,"rayTracingPipelineTraceRaysIndirect");
  need(fd.descriptorBuffer,"descriptorBuffer");
  need(fs.shaderObject,"shaderObject");
  need(fv.vertexInputDynamicState,"vertexInputDynamicState");
  need(fe.extendedDynamicState3PolygonMode,"extendedDynamicState3PolygonMode");
  need(fe.extendedDynamicState3RasterizationSamples,"extendedDynamicState3RasterizationSamples");
  need(fe.extendedDynamicState3SampleMask,"extendedDynamicState3SampleMask");
  need(fe.extendedDynamicState3AlphaToCoverageEnable,"extendedDynamicState3AlphaToCoverageEnable");
  need(fe.extendedDynamicState3ColorBlendEnable,"extendedDynamicState3ColorBlendEnable");
  need(fe.extendedDynamicState3ColorBlendEquation,"extendedDynamicState3ColorBlendEquation");
  need(fe.extendedDynamicState3ColorWriteMask,"extendedDynamicState3ColorWriteMask");
  uint32_t qc{};
  vkGetPhysicalDeviceQueueFamilyProperties(candidate,&qc,nullptr);
  std::vector<VkQueueFamilyProperties> qs(qc);
  vkGetPhysicalDeviceQueueFamilyProperties(candidate,&qc,qs.data());
  uint32_t chosen=UINT32_MAX;
  for(uint32_t i=0;i<qc;i++){VkBool32 present{};
  check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate,i,surface,&present),"surface support");
  if(present&&(qs[i].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)){chosen=i;
  break;
  }}
  if(chosen==UINT32_MAX)missing.emplace_back("graphics/compute/present 队列");
  for(VkFormat fmt:{VK_FORMAT_R8G8B8A8_SRGB,VK_FORMAT_R8_UNORM}){VkFormatProperties fp{};
  vkGetPhysicalDeviceFormatProperties(candidate,fmt,&fp);
  if((fp.optimalTilingFeatures&(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT))!=(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT))missing.emplace_back("纹理格式/linear filtering "+std::to_string(fmt));
  }
  std::cout<<"设备: "<<prop.deviceName<<"\n";
  if(!missing.empty()){for(const auto& m:missing)std::cout<<"  缺少: "<<m<<"\n";
  continue;
  }
  physical=candidate;
  properties=prop;
  family=chosen;
  vkGetPhysicalDeviceMemoryProperties(physical,&memoryProperties);
  descriptorProps.pNext=&rtProps;
  rtProps.pNext=&asProps;
  auto p2=vkinit<VkPhysicalDeviceProperties2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
  p2.pNext=&descriptorProps;
  vkGetPhysicalDeviceProperties2(physical,&p2);
  if(rtProps.maxRayRecursionDepth<1||descriptorProps.maxDescriptorBufferBindings<1||descriptorProps.maxResourceDescriptorBufferBindings<1||descriptorProps.maxSamplerDescriptorBufferBindings<1)throw std::runtime_error("RT/Descriptor Buffer limits 不满足");

  feature.features={};
  f12=vkinit<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
  f12.pNext=&f13;
  f12.bufferDeviceAddress=VK_TRUE;
  f12.timelineSemaphore=VK_TRUE;
  f12.scalarBlockLayout=VK_TRUE;
  f13=vkinit<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
  f13.pNext=&fa;
  f13.dynamicRendering=VK_TRUE;
  f13.synchronization2=VK_TRUE;
  f13.maintenance4=VK_TRUE;
  fa.accelerationStructureCaptureReplay=VK_FALSE;
  fa.accelerationStructureIndirectBuild=VK_FALSE;
  fa.accelerationStructureHostCommands=VK_FALSE;
  fa.descriptorBindingAccelerationStructureUpdateAfterBind=VK_FALSE;
  fr.rayTracingPipelineShaderGroupHandleCaptureReplay=VK_FALSE;
  fr.rayTracingPipelineShaderGroupHandleCaptureReplayMixed=VK_FALSE;
  fr.rayTraversalPrimitiveCulling=VK_FALSE;
  fd.descriptorBufferCaptureReplay=VK_FALSE;
  fd.descriptorBufferImageLayoutIgnored=VK_FALSE;
  fd.descriptorBufferPushDescriptors=VK_FALSE;
  fe=vkinit<VkPhysicalDeviceExtendedDynamicState3FeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT);
  fe.pNext=&fv;
  fe.extendedDynamicState3PolygonMode=VK_TRUE;
  fe.extendedDynamicState3RasterizationSamples=VK_TRUE;
  fe.extendedDynamicState3SampleMask=VK_TRUE;
  fe.extendedDynamicState3AlphaToCoverageEnable=VK_TRUE;
  fe.extendedDynamicState3ColorBlendEnable=VK_TRUE;
  fe.extendedDynamicState3ColorBlendEquation=VK_TRUE;
  fe.extendedDynamicState3ColorWriteMask=VK_TRUE;
  float priority=1;
  auto qi=vkinit<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
  qi.queueFamilyIndex=family;
  qi.queueCount=1;
  qi.pQueuePriorities=&priority;
  auto dc=vkinit<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
  dc.pNext=&feature;
  dc.queueCreateInfoCount=1;
  dc.pQueueCreateInfos=&qi;
  dc.enabledExtensionCount=uint32_t(required.size());
  dc.ppEnabledExtensionNames=required.data();
  check(vkCreateDevice(physical,&dc,nullptr,&device),"vkCreateDevice");
  volkLoadDevice(device);
  vkGetDeviceQueue(device,family,0,&queue);
  std::cout<<"必需 features/properties 检查通过，scratch alignment="<<asProps.minAccelerationStructureScratchOffsetAlignment<<", descriptor alignment="<<descriptorProps.descriptorBufferOffsetAlignment<<"\n";
  return;
 }
 throw std::runtime_error("没有满足全部必需能力的设备；未启用降级渲染");
}
void App::createScene(){
 if(scene.materials.size()>properties.limits.maxImageArrayLayers||scene.materials.size()>65535||scene.textureSize>properties.limits.maxImageDimension2D)throw std::runtime_error("材质纹理容量超过设备限制");
 if(asProps.maxInstanceCount<2||asProps.maxGeometryCount<1||properties.limits.maxPerStageDescriptorStorageBuffers<23||properties.limits.maxPerStageDescriptorSampledImages<2||properties.limits.maxPerStageDescriptorSamplers<2)throw std::runtime_error("AS/descriptor 容量不满足实际场景");
 VkFormatProperties vertexFormat{};
 vkGetPhysicalDeviceFormatProperties(physical,VK_FORMAT_R32G32B32_SFLOAT,&vertexFormat);
 if(!(vertexFormat.bufferFeatures&VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR))throw std::runtime_error("缺少 Triangle AS 的 float3 vertex format");
 geometry=upload(scene.triangles,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
 std::vector<MaterialGPU> ms;
 for(const auto& m:scene.materials)ms.push_back(m.gpu);
 materials=upload(ms,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
 lightTable=scene.lights.empty()?buffer(sizeof(Light),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT):upload(scene.lights,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
 vertices=upload(scene.vertices,VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
 auto createAS=[&](VkDeviceSize size,VkAccelerationStructureTypeKHR type){AS a;
 ScopeExit cleanup{[&]{release(a);
 }};
 a.buffer=buffer(size,VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR);
 auto c=vkinit<VkAccelerationStructureCreateInfoKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR);
 c.buffer=a.buffer.handle;
 c.size=size;
 c.type=type;
 check(vkCreateAccelerationStructureKHR(device,&c,nullptr,&a.handle) ,"create AS");
 cleanup.dismiss();
 return a;
 };
 auto addressAS=[&](AS& a){auto ai=vkinit<VkAccelerationStructureDeviceAddressInfoKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR);
 ai.accelerationStructure=a.handle;
 a.address=vkGetAccelerationStructureDeviceAddressKHR(device,&ai);
 };
 // 展厅与玻璃按空间职责分组，压缩后的每组地址才允许写入 TLAS。
 auto buildBLAS=[&](uint32_t first,uint32_t primitives){
  if(primitives>asProps.maxPrimitiveCount)throw std::runtime_error("BLAS primitive count 超过设备限制");
  auto tri=vkinit<VkAccelerationStructureGeometryTrianglesDataKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR);
  tri.vertexFormat=VK_FORMAT_R32G32B32_SFLOAT;
  tri.vertexData.deviceAddress=vertices.address+uint64_t(first)*3*sizeof(V3);
  tri.vertexStride=sizeof(V3);
  tri.maxVertex=primitives*3-1;
  tri.indexType=VK_INDEX_TYPE_NONE_KHR;
  auto geom=vkinit<VkAccelerationStructureGeometryKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR);
  geom.geometryType=VK_GEOMETRY_TYPE_TRIANGLES_KHR;
  geom.flags=VK_GEOMETRY_OPAQUE_BIT_KHR;
  geom.geometry.triangles=tri;
  auto bi=vkinit<VkAccelerationStructureBuildGeometryInfoKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR);
  bi.type=VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  bi.flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR|VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
  bi.mode=VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  bi.geometryCount=1;
  bi.pGeometries=&geom;
  auto sizes=vkinit<VkAccelerationStructureBuildSizesInfoKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR);
  vkGetAccelerationStructureBuildSizesKHR(device,VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,&bi,&primitives,&sizes);
  AS original=createAS(sizes.accelerationStructureSize,bi.type);
  ScopeExit originalCleanup{[&]{release(original);
  }};
  Buffer scratch=buffer(sizes.buildScratchSize+asProps.minAccelerationStructureScratchOffsetAlignment,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  ScopeExit scratchCleanup{[&]{release(scratch);
  }};
  bi.dstAccelerationStructure=original.handle;
  bi.scratchData.deviceAddress=alignUp(scratch.address,asProps.minAccelerationStructureScratchOffsetAlignment);
  auto query=vkinit<VkQueryPoolCreateInfo>(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
  query.queryType=VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
  query.queryCount=1;
  VkQueryPool qp{};
  check(vkCreateQueryPool(device,&query,nullptr,&qp) ,"compact query");
  ScopeExit queryCleanup{[&]{if(qp)vkDestroyQueryPool(device,qp,nullptr);
  }};
  VkAccelerationStructureBuildRangeInfoKHR range{};
  range.primitiveCount=primitives;
  const auto* rangePtr=&range;
  beginInit();
  vkCmdResetQueryPool(initCmd,qp,0,1);
  vkCmdBuildAccelerationStructuresKHR(initCmd,1,&bi,&rangePtr);
  barrier(initCmd,VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
  vkCmdWriteAccelerationStructuresPropertiesKHR(initCmd,1,&original.handle,VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,qp,0);
  endInit();
  VkDeviceSize compactSize{};
  check(vkGetQueryPoolResults(device,qp,0,1,sizeof(compactSize),&compactSize,sizeof(compactSize),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT),"compaction size");
  AS compact=createAS(compactSize,VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
  ScopeExit compactCleanup{[&]{release(compact);
  }};
  auto copy=vkinit<VkCopyAccelerationStructureInfoKHR>(VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR);
  copy.src=original.handle;
  copy.dst=compact.handle;
  copy.mode=VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
  beginInit();
  vkCmdCopyAccelerationStructureKHR(initCmd,&copy);
  barrier(initCmd,VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
  endInit();
  addressAS(compact);
  std::cout<<"静态 BLAS: "<<primitives<<" triangles, "<<sizes.accelerationStructureSize<<" -> "<<compactSize<<" bytes\n";
  release(original);
  release(scratch);
  vkDestroyQueryPool(device,qp,nullptr);
  qp=VK_NULL_HANDLE;
  compactCleanup.dismiss();
  return compact;
 };
 uint32_t glassFirst=uint32_t(scene.triangles.size())-12;
 blas=buildBLAS(0,glassFirst);
 glassBlas=buildBLAS(glassFirst,12);
 std::vector<VkAccelerationStructureInstanceKHR> instanceData(2);
 for(uint32_t i=0;i<2;i++){auto& inst=instanceData[i];
 inst.transform.matrix[0][0]=1;
 inst.transform.matrix[1][1]=1;
 inst.transform.matrix[2][2]=1;
 inst.mask=255;
 inst.instanceCustomIndex=i==0?0:glassFirst;
 inst.flags=VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
 inst.accelerationStructureReference=i==0?blas.address:glassBlas.address;
 }
 Buffer instances=upload(instanceData,VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
 ScopeExit instanceCleanup{[&]{release(instances);
 }};
 auto id=vkinit<VkAccelerationStructureGeometryInstancesDataKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR);
 id.data.deviceAddress=instances.address;
 auto geom=vkinit<VkAccelerationStructureGeometryKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR);
 geom.geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR;
 geom.geometry.instances=id;
 auto bi=vkinit<VkAccelerationStructureBuildGeometryInfoKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR);
 bi.type=VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
 bi.flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
 bi.mode=VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
 bi.geometryCount=1;
 bi.pGeometries=&geom;
 uint32_t primitives=2;
 auto sizes=vkinit<VkAccelerationStructureBuildSizesInfoKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR);
 vkGetAccelerationStructureBuildSizesKHR(device,VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,&bi,&primitives,&sizes);
 tlas=createAS(sizes.accelerationStructureSize,bi.type);
 Buffer scratch=buffer(sizes.buildScratchSize+asProps.minAccelerationStructureScratchOffsetAlignment,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
 ScopeExit tlasScratchCleanup{[&]{release(scratch);
 }};
 bi.dstAccelerationStructure=tlas.handle;
 bi.scratchData.deviceAddress=alignUp(scratch.address,asProps.minAccelerationStructureScratchOffsetAlignment);
 VkAccelerationStructureBuildRangeInfoKHR range{};
 range.primitiveCount=2;
 const auto* rangePtr=&range;
 beginInit();
 vkCmdBuildAccelerationStructuresKHR(initCmd,1,&bi,&rangePtr);
 barrier(initCmd,VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
 endInit();
 addressAS(tlas);
 release(scratch);
 release(instances);
 release(vertices);
 std::cout<<"greedy mesh: "<<scene.triangles.size()<<" triangles, "<<scene.lights.size()<<" emissive triangles\n";
 auto texture=[&](const std::vector<std::vector<uint8_t>>& data,VkFormat fmt,uint32_t bpp){
  Image image;
  ScopeExit imageCleanup{[&]{release(image);
  }};
  uint32_t levels=uint32_t(std::log2(scene.textureSize))+1,layers=uint32_t(data.size());
  VkImageFormatProperties textureProperties{};
  check(vkGetPhysicalDeviceImageFormatProperties(physical,fmt,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,0,&textureProperties),"texture image format properties");
  if(scene.textureSize>textureProperties.maxExtent.width||scene.textureSize>textureProperties.maxExtent.height||levels>textureProperties.maxMipLevels||layers>textureProperties.maxArrayLayers||!(textureProperties.sampleCounts&VK_SAMPLE_COUNT_1_BIT))throw std::runtime_error("纹理 mip/layer/extent 超过格式能力");
  auto ic=vkinit<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
  ic.imageType=VK_IMAGE_TYPE_2D;
  ic.format=fmt;
  ic.extent={scene.textureSize,scene.textureSize,1};
  ic.mipLevels=levels;
  ic.arrayLayers=layers;
  ic.samples=VK_SAMPLE_COUNT_1_BIT;
  ic.tiling=VK_IMAGE_TILING_OPTIMAL;
  ic.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
  ic.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
  check(vkCreateImage(device,&ic,nullptr,&image.handle),"texture image");
  VkMemoryRequirements mr{};
  vkGetImageMemoryRequirements(device,image.handle,&mr);
  auto ma=vkinit<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
  ma.allocationSize=mr.size;
  ma.memoryTypeIndex=memoryType(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  check(vkAllocateMemory(device,&ma,nullptr,&image.memory),"texture memory");
  check(vkBindImageMemory(device,image.handle,image.memory,0),"texture bind");
    VkDeviceSize layerBytes=data[0].size();
  Buffer staging=buffer(layerBytes*layers,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,true);
  ScopeExit stagingCleanup{[&]{release(staging);
  }};
  std::vector<VkBufferImageCopy> regions;
  for(uint32_t layer=0;layer<layers;layer++){std::memcpy(static_cast<uint8_t*>(staging.mapped)+layerBytes*layer,data[layer].data(),layerBytes);
  VkDeviceSize offset=layerBytes*layer;
  uint32_t sz=scene.textureSize;
  for(uint32_t level=0;level<levels;level++){VkBufferImageCopy cp{};
  cp.bufferOffset=offset;
  cp.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,layer,1};
  cp.imageExtent={sz,sz,1};
  regions.push_back(cp);
  offset+=VkDeviceSize(sz)*sz*bpp;
  sz=std::max(1u,sz/2);
  }}
  beginInit();
  barrier(initCmd,VK_PIPELINE_STAGE_2_HOST_BIT,VK_ACCESS_2_HOST_WRITE_BIT,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
  imageBarrier(initCmd,image.handle,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_PIPELINE_STAGE_2_NONE,0,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,levels,layers);
  vkCmdCopyBufferToImage(initCmd,staging.handle,image.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,uint32_t(regions.size()),regions.data());
  imageBarrier(initCmd,image.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,levels,layers);
  endInit();
  release(staging);
  auto vc=vkinit<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
  vc.image=image.handle;
  vc.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  vc.format=fmt;
  vc.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,levels,0,layers};
  check(vkCreateImageView(device,&vc,nullptr,&image.view) ,"texture view");
  imageCleanup.dismiss();
  return image;
 };
 colorTexture=texture(scene.colors,VK_FORMAT_R8G8B8A8_SRGB,4);
 roughTexture=texture(scene.roughness,VK_FORMAT_R8_UNORM,1);
 auto sc=vkinit<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
 sc.magFilter=VK_FILTER_LINEAR;
 sc.minFilter=VK_FILTER_LINEAR;
 sc.mipmapMode=VK_SAMPLER_MIPMAP_MODE_LINEAR;
 sc.addressModeU=VK_SAMPLER_ADDRESS_MODE_REPEAT;
 sc.addressModeV=VK_SAMPLER_ADDRESS_MODE_REPEAT;
 sc.addressModeW=VK_SAMPLER_ADDRESS_MODE_REPEAT;
 sc.maxLod=float(std::log2(scene.textureSize));
 check(vkCreateSampler(device,&sc,nullptr,&sampler),"sampler");
}
void App::createLayouts(){
 VkDescriptorSetLayoutBinding bindings[26]{};
 for(uint32_t i=0;i<26;i++){bindings[i].binding=i;
 bindings[i].descriptorCount=1;
 bindings[i].stageFlags=VK_SHADER_STAGE_ALL;
 bindings[i].descriptorType=i==0?VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:(i==4||i==5?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
 }
 auto ci=vkinit<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
 ci.flags=VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
 ci.bindingCount=26;
 ci.pBindings=bindings;
 check(vkCreateDescriptorSetLayout(device,&ci,nullptr,&setLayout),"descriptor layout");
 vkGetDescriptorSetLayoutSizeEXT(device,setLayout,&descriptorSize);
 descriptorSize=alignUp(descriptorSize,descriptorProps.descriptorBufferOffsetAlignment);
 for(uint32_t i=0;i<26;i++)vkGetDescriptorSetLayoutBindingOffsetEXT(device,setLayout,i,&bindingOffsets[i]);
 if(descriptorSize>descriptorProps.maxResourceDescriptorBufferRange||descriptorSize>descriptorProps.maxSamplerDescriptorBufferRange)throw std::runtime_error("descriptor layout 超过设备 range");
 VkPushConstantRange push{VK_SHADER_STAGE_ALL,0,sizeof(Push)};
 auto li=vkinit<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
 li.setLayoutCount=1;
 li.pSetLayouts=&setLayout;
 li.pushConstantRangeCount=1;
 li.pPushConstantRanges=&push;
 check(vkCreatePipelineLayout(device,&li,nullptr,&layout),"pipeline layout");
}
void App::writeDescriptors(Slot& slot){
 std::memset(slot.descriptor.mapped,0,size_t(descriptorSize));
 for(uint32_t i=0;i<26;i++){
  auto get=vkinit<VkDescriptorGetInfoEXT>(VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT);
  size_t bytes{};
  auto addr=vkinit<VkDescriptorAddressInfoEXT>(VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT);
  VkDescriptorImageInfo image{};
  if(i==0){get.type=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
  get.data.accelerationStructure=tlas.address;
  bytes=descriptorProps.accelerationStructureDescriptorSize;
  }
  else if(i==4||i==5){get.type=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  image.sampler=sampler;
  image.imageView=i==4?colorTexture.view:roughTexture.view;
  image.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  get.data.pCombinedImageSampler=&image;
  bytes=descriptorProps.combinedImageSamplerDescriptorSize;
  }
  else {get.type=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  Buffer* b=i>=7?&frameBuffers[i-7]:(i==1?&geometry:(i==2?&materials:(i==3?&lightTable:&slot.frame)));
  addr.address=b->address;
  addr.range=b->size;
  addr.format=VK_FORMAT_UNDEFINED;
  get.data.pStorageBuffer=&addr;
  bytes=descriptorProps.storageBufferDescriptorSize;
  }
  vkGetDescriptorEXT(device,&get,bytes,static_cast<uint8_t*>(slot.descriptor.mapped)+bindingOffsets[i]);
 }
}
void App::createPrograms(){
 VkPushConstantRange push{VK_SHADER_STAGE_ALL,0,sizeof(Push)};
 for(auto [file,stage]:std::array<std::pair<const char*,VkShaderStageFlagBits>,6>{{{"wave.comp",VK_SHADER_STAGE_COMPUTE_BIT},{"restir.comp",VK_SHADER_STAGE_COMPUTE_BIT},{"shadow_resolve.comp",VK_SHADER_STAGE_COMPUTE_BIT},{"svgf.comp",VK_SHADER_STAGE_COMPUTE_BIT},{"display.vert",VK_SHADER_STAGE_VERTEX_BIT},{"display.frag",VK_SHADER_STAGE_FRAGMENT_BIT}}}){
  auto ci=vkinit<VkShaderCreateInfoEXT>(VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT);
  ci.stage=stage;
  ci.nextStage=stage==VK_SHADER_STAGE_VERTEX_BIT?VK_SHADER_STAGE_FRAGMENT_BIT:0;
  ci.codeType=VK_SHADER_CODE_TYPE_SPIRV_EXT;
  ci.codeSize=binaries.at(file).size()*4;
  ci.pCode=binaries.at(file).data();
  ci.pName="main";
  ci.setLayoutCount=1;
  ci.pSetLayouts=&setLayout;
  ci.pushConstantRangeCount=1;
  ci.pPushConstantRanges=&push;
  VkShaderEXT object{};
  check(vkCreateShadersEXT(device,1,&ci,nullptr,&object),"vkCreateShadersEXT");
  shaders[file]=object;
 }
 VkShaderModule modules[3]{};
 ScopeExit moduleCleanup{[&]{for(auto m:modules)if(m)vkDestroyShaderModule(device,m,nullptr);
 }};
 VkPipelineShaderStageCreateInfo stages[3]{};
 const std::array<const char*,3> names={"trace.rgen","miss.rmiss","hit.rchit"};
 for(uint32_t i=0;i<3;i++){auto ci=vkinit<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
 ci.codeSize=binaries.at(names[i]).size()*4;
 ci.pCode=binaries.at(names[i]).data();
 check(vkCreateShaderModule(device,&ci,nullptr,&modules[i]),"RT shader module");
 stages[i]=vkinit<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
 stages[i].module=modules[i];
 stages[i].pName="main";
 stages[i].stage=i==0?VK_SHADER_STAGE_RAYGEN_BIT_KHR:(i==1?VK_SHADER_STAGE_MISS_BIT_KHR:VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);
 }
 VkRayTracingShaderGroupCreateInfoKHR groups[3]{};
 for(uint32_t i=0;i<3;i++){groups[i]=vkinit<VkRayTracingShaderGroupCreateInfoKHR>(VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR);
 groups[i].type=i==2?VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR:VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
 groups[i].generalShader=i==2?VK_SHADER_UNUSED_KHR:i;
 groups[i].closestHitShader=i==2?2:VK_SHADER_UNUSED_KHR;
 groups[i].anyHitShader=VK_SHADER_UNUSED_KHR;
 groups[i].intersectionShader=VK_SHADER_UNUSED_KHR;
 }
 auto ci=vkinit<VkRayTracingPipelineCreateInfoKHR>(VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR);
 ci.flags=VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
 ci.stageCount=3;
 ci.pStages=stages;
 ci.groupCount=3;
 ci.pGroups=groups;
 ci.maxPipelineRayRecursionDepth=1;
 ci.layout=layout;
 check(vkCreateRayTracingPipelinesKHR(device,VK_NULL_HANDLE,VK_NULL_HANDLE,1,&ci,nullptr,&rtPipeline),"RT pipeline");
 for(auto& m:modules){vkDestroyShaderModule(device,m,nullptr);
 m=VK_NULL_HANDLE;
 }
 uint64_t stride=alignUp(rtProps.shaderGroupHandleSize,std::max(rtProps.shaderGroupHandleAlignment,rtProps.shaderGroupBaseAlignment));
 if(stride>rtProps.maxShaderGroupStride)throw std::runtime_error("SBT stride 超过限制");
 sbt=buffer(stride*3+rtProps.shaderGroupBaseAlignment,VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR,true);
 uint64_t base=alignUp(sbt.address,rtProps.shaderGroupBaseAlignment),offset=base-sbt.address;
 std::vector<uint8_t> handles(size_t(rtProps.shaderGroupHandleSize)*3);
 check(vkGetRayTracingShaderGroupHandlesKHR(device,rtPipeline,0,3,handles.size(),handles.data()),"SBT handles");
 for(uint32_t i=0;i<3;i++)std::memcpy(static_cast<uint8_t*>(sbt.mapped)+offset+stride*i,handles.data()+rtProps.shaderGroupHandleSize*i,rtProps.shaderGroupHandleSize);
 raygenRegion={base,stride,stride};
 missRegion={base+stride,stride,stride};
 hitRegion={base+stride*2,stride,stride};
}
void App::rebuild(){
 int w{},h{};
 glfwGetFramebufferSize(window,&w,&h);
 if(w==0||h==0)return;
 check(vkDeviceWaitIdle(device),"resize 安全点");
 for(auto view:swapViews)vkDestroyImageView(device,view,nullptr);
 swapViews.clear();
 for(auto sem:presentSemaphores)vkDestroySemaphore(device,sem,nullptr);
 presentSemaphores.clear();
 VkSurfaceCapabilitiesKHR caps{};
 check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&caps),"surface capabilities");
 uint32_t fc{};
 check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&fc,nullptr),"surface formats");
 std::vector<VkSurfaceFormatKHR> formats(fc);
 check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&fc,formats.data()),"surface formats");
 format=VK_FORMAT_UNDEFINED;
 for(VkFormat preferred:{VK_FORMAT_B8G8R8A8_SRGB,VK_FORMAT_R8G8B8A8_SRGB}){
  bool offered=std::any_of(formats.begin(),formats.end(),[preferred](const auto& entry){return entry.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR&&(entry.format==preferred||entry.format==VK_FORMAT_UNDEFINED);});
  VkFormatProperties fp{};vkGetPhysicalDeviceFormatProperties(physical,preferred,&fp);
  if(offered&&(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)){format=preferred;break;}
 }
 if(format==VK_FORMAT_UNDEFINED)throw std::runtime_error("交换链没有 sRGB 格式");
 if(!(caps.supportedUsageFlags&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))throw std::runtime_error("交换链缺少 color attachment usage");
 extent=caps.currentExtent.width==UINT32_MAX?VkExtent2D{std::clamp(uint32_t(w),caps.minImageExtent.width,caps.maxImageExtent.width),std::clamp(uint32_t(h),caps.minImageExtent.height,caps.maxImageExtent.height)}:caps.currentExtent;
 uint64_t N=uint64_t(extent.width)*extent.height;
 uint64_t traceWidth=std::min(uint64_t(1024),uint64_t(properties.limits.maxComputeWorkGroupCount[0])*properties.limits.maxComputeWorkGroupSize[0]);
 if(((N*2+traceWidth-1)/traceWidth)*traceWidth>rtProps.maxRayDispatchInvocationCount||N*3>UINT32_MAX)throw std::runtime_error("framebuffer 的双分支/padding 超过 RT dispatch 容量");
 if((N*2+traceWidth-1)/traceWidth>uint64_t(properties.limits.maxComputeWorkGroupCount[1])*properties.limits.maxComputeWorkGroupSize[1])throw std::runtime_error("RT dispatch height 超过设备限制");
 uint32_t imageCount=std::max(caps.minImageCount,3u);
 if(caps.maxImageCount)imageCount=std::min(imageCount,caps.maxImageCount);
 auto ci=vkinit<VkSwapchainCreateInfoKHR>(VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);
 ci.surface=surface;
 ci.minImageCount=imageCount;
 ci.imageFormat=format;
 ci.imageColorSpace=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
 ci.imageExtent=extent;
 ci.imageArrayLayers=1;
 ci.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
 ci.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;
 ci.preTransform=caps.currentTransform;
 ci.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
 if(!(caps.supportedCompositeAlpha&ci.compositeAlpha))ci.compositeAlpha=VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha&(~caps.supportedCompositeAlpha+1));
 ci.presentMode=VK_PRESENT_MODE_FIFO_KHR;
 ci.clipped=VK_TRUE;
 ci.oldSwapchain=swapchain;
 VkSwapchainKHR next{};
 check(vkCreateSwapchainKHR(device,&ci,nullptr,&next),"swapchain");
 if(swapchain)vkDestroySwapchainKHR(device,swapchain,nullptr);
 swapchain=next;
 check(vkGetSwapchainImagesKHR(device,swapchain,&imageCount,nullptr),"swap images");
 swapImages.resize(imageCount);
 check(vkGetSwapchainImagesKHR(device,swapchain,&imageCount,swapImages.data()),"swap images");
 presented.assign(imageCount,false);
 for(auto image:swapImages){auto vi=vkinit<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
 vi.image=image;
 vi.viewType=VK_IMAGE_VIEW_TYPE_2D;
 vi.format=format;
 vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
 VkImageView view{};
 check(vkCreateImageView(device,&vi,nullptr,&view),"swap view");
 swapViews.push_back(view);
 auto si=vkinit<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
 VkSemaphore semaphore{};
 check(vkCreateSemaphore(device,&si,nullptr,&semaphore),"present semaphore");
 presentSemaphores.push_back(semaphore);
 }
 for(auto& b:frameBuffers)release(b);
 const std::array<uint64_t,19> sizes={N*2*192,N*2*192,N*2*8,16,N*2*64,N*3*16,N*3*144,N*3*144,N*32,N*32,N*32,N*32,N*3*32,N*3*32,N*3*16,N*3*16,N*16,N*2*16,24};
 for(size_t i=0;i<sizes.size();i++){if(sizes[i]>properties.limits.maxStorageBufferRange)throw std::runtime_error("framebuffer buffer 超过 maxStorageBufferRange");
 frameBuffers[i]=buffer(sizes[i],VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|(i==18?VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT:0));
 }
 beginInit();
 for(auto& b:frameBuffers)vkCmdFillBuffer(initCmd,b.handle,0,VK_WHOLE_SIZE,0);
 barrier(initCmd);
 endInit();
 for(auto& slot:slots)writeDescriptors(slot);
 reset=true;
 resize=false;
 std::cout<<"framebuffer: "<<extent.width<<" x "<<extent.height<<", 队列容量 "<<N*2<<", 光学贡献 slots "<<N*3<<"\n";
}
void App::record(Slot& slot,uint32_t image){
 VkCommandBuffer c=slot.command;
 auto begin=vkinit<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
 begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
 check(vkBeginCommandBuffer(c,&begin),"frame begin");
 barrier(c);
 barrier(c,VK_PIPELINE_STAGE_2_HOST_BIT,VK_ACCESS_2_HOST_WRITE_BIT,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_DESCRIPTOR_BUFFER_READ_BIT_EXT);
 auto binding=vkinit<VkDescriptorBufferBindingInfoEXT>(VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT);
 binding.address=slot.descriptor.address;
 binding.usage=VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT|VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT;
 vkCmdBindDescriptorBuffersEXT(c,1,&binding);
 uint32_t index=0;
 VkDeviceSize offset=0;
 for(auto point:{VK_PIPELINE_BIND_POINT_COMPUTE,VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,VK_PIPELINE_BIND_POINT_GRAPHICS})vkCmdSetDescriptorBufferOffsetsEXT(c,point,layout,0,1,&index,&offset);
 uint64_t N=uint64_t(extent.width)*extent.height;
 auto compute=[&](const char* name,Push p,uint64_t items){
  auto stage=VK_SHADER_STAGE_COMPUTE_BIT;
  VkShaderEXT shader=shaders.at(name);
  vkCmdBindShadersEXT(c,1,&stage,&shader);
  vkCmdPushConstants(c,layout,VK_SHADER_STAGE_ALL,0,sizeof(p),&p);
  uint64_t groups=(items+63)/64;
  uint32_t x=uint32_t(std::min(groups,uint64_t(properties.limits.maxComputeWorkGroupCount[0]))),y=uint32_t((groups+x-1)/x);
  if(y>properties.limits.maxComputeWorkGroupCount[1])throw std::runtime_error("compute dispatch 超过限制");
  vkCmdDispatch(c,x,y,1);
 };
 auto trace=[&](Push p,bool direct){
  vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,rtPipeline);
  vkCmdPushConstants(c,layout,VK_SHADER_STAGE_ALL,0,sizeof(p),&p);
  if(direct)vkCmdTraceRaysKHR(c,&raygenRegion,&missRegion,&hitRegion,&callRegion,extent.width,extent.height,1);
  else vkCmdTraceRaysIndirectKHR(c,&raygenRegion,&missRegion,&hitRegion,&callRegion,frameBuffers[18].address+(p.pass==1?12:0));
 };
 compute("wave.comp",{0,0,0,0},N*3);
 barrier(c);
 for(uint32_t bounce=0;bounce<=8;bounce++){
  uint32_t q=bounce%2;
  compute("wave.comp",{q,bounce,1,0},1);
  barrier(c);
  trace({q,bounce,0,0},bounce==0);
  barrier(c,VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,VK_ACCESS_2_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT);
  compute("wave.comp",{q,bounce,2,0},N*2);
  barrier(c);
  if(bounce==0){for(uint32_t pass=0;pass<4;pass++){compute("restir.comp",{q,bounce,pass,0},N);
  barrier(c);
  }}
  compute("wave.comp",{1-q,bounce,3,0},1);
  barrier(c,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT|VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT|VK_ACCESS_2_SHADER_READ_BIT);
  trace({q,bounce,1,0},false);
  barrier(c);
  compute("shadow_resolve.comp",{q,bounce,0,0},N*2);
  barrier(c);
 }
 compute("svgf.comp",{0,0,0,0},N*3);
 barrier(c);
 compute("svgf.comp",{1,0,1,0},N*3);
 barrier(c);
 uint32_t filter=0;
 for(uint32_t it=0;it<5;it++){compute("svgf.comp",{filter,0,2,1u<<it},N*3);
 barrier(c);
 filter=1-filter;
 }
 compute("svgf.comp",{filter,0,3,0},N);
 barrier(c);
 for(auto pair:std::array<std::pair<uint32_t,uint32_t>,3>{{{6,7},{10,11},{12,13}}}){VkBufferCopy cp{0,0,frameBuffers[pair.first].size};
 vkCmdCopyBuffer(c,frameBuffers[pair.first].handle,frameBuffers[pair.second].handle,1,&cp);
 }
 barrier(c);
 imageBarrier(c,swapImages[image],presented[image]?VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_PIPELINE_STAGE_2_NONE,0,VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
 auto attachment=vkinit<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
 attachment.imageView=swapViews[image];
 attachment.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
 attachment.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
 attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
 auto rendering=vkinit<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
 rendering.renderArea.extent=extent;
 rendering.layerCount=1;
 rendering.colorAttachmentCount=1;
 rendering.pColorAttachments=&attachment;
 vkCmdBeginRendering(c,&rendering);
 VkShaderStageFlagBits stages[]={VK_SHADER_STAGE_VERTEX_BIT,VK_SHADER_STAGE_FRAGMENT_BIT};
 VkShaderEXT objects[]={shaders.at("display.vert"),shaders.at("display.frag")};
 vkCmdBindShadersEXT(c,2,stages,objects);
 VkViewport viewport{0,0,float(extent.width),float(extent.height),0,1};
 VkRect2D scissor{{0,0},extent};
 vkCmdSetViewportWithCount(c,1,&viewport);
 vkCmdSetScissorWithCount(c,1,&scissor);
 vkCmdSetVertexInputEXT(c,0,nullptr,0,nullptr);
 vkCmdSetPrimitiveTopology(c,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
 vkCmdSetPrimitiveRestartEnable(c,VK_FALSE);
 vkCmdSetRasterizerDiscardEnable(c,VK_FALSE);
 vkCmdSetPolygonModeEXT(c,VK_POLYGON_MODE_FILL);
 vkCmdSetCullMode(c,VK_CULL_MODE_NONE);
 vkCmdSetFrontFace(c,VK_FRONT_FACE_COUNTER_CLOCKWISE);
 vkCmdSetDepthBiasEnable(c,VK_FALSE);
 vkCmdSetLineWidth(c,1);
 vkCmdSetDepthTestEnable(c,VK_FALSE);
 vkCmdSetDepthWriteEnable(c,VK_FALSE);
 vkCmdSetDepthCompareOp(c,VK_COMPARE_OP_ALWAYS);
 vkCmdSetDepthBoundsTestEnable(c,VK_FALSE);
 vkCmdSetStencilTestEnable(c,VK_FALSE);
 vkCmdSetRasterizationSamplesEXT(c,VK_SAMPLE_COUNT_1_BIT);
 VkSampleMask mask=~0u;
 vkCmdSetSampleMaskEXT(c,VK_SAMPLE_COUNT_1_BIT,&mask);
 vkCmdSetAlphaToCoverageEnableEXT(c,VK_FALSE);
 VkBool32 blend=VK_FALSE;
 vkCmdSetColorBlendEnableEXT(c,0,1,&blend);
 VkColorBlendEquationEXT equation{VK_BLEND_FACTOR_ONE,VK_BLEND_FACTOR_ZERO,VK_BLEND_OP_ADD,VK_BLEND_FACTOR_ONE,VK_BLEND_FACTOR_ZERO,VK_BLEND_OP_ADD};
 vkCmdSetColorBlendEquationEXT(c,0,1,&equation);
 VkColorComponentFlags write=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
 vkCmdSetColorWriteMaskEXT(c,0,1,&write);
 vkCmdDraw(c,3,1,0,0);
 vkCmdEndRendering(c);
 imageBarrier(c,swapImages[image],VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_2_NONE,0);
 check(vkEndCommandBuffer(c),"frame end");
}
void App::run(){
 auto last=std::chrono::steady_clock::now();
 auto titleStart=last;
 uint32_t titleFrames{};
 bool titleActive=false;
 auto resetTitle=[&]{titleFrames=0;
 titleActive=false;
 glfwSetWindowTitle(window,"MiniVoxelPathTracer | -- FPS | -- ms/frame");
 };
 auto capture=[&](bool on){if(captured!=on){captured=on;
 mouseInit=false;
 glfwSetInputMode(window,GLFW_CURSOR,on?GLFW_CURSOR_DISABLED:GLFW_CURSOR_NORMAL);
 }};
 auto updateInput=[&]{
  glfwPollEvents();
  auto now=std::chrono::steady_clock::now();
  float dt=std::chrono::duration<float>(now-last).count();
  last=now;
  bool focused=glfwGetWindowAttrib(window,GLFW_FOCUSED)!=0;
  int width{},height{};
  glfwGetFramebufferSize(window,&width,&height);
  bool renderable=width>0&&height>0;
  if(!focused||!wasFocused||!renderable||!wasRenderable)dt=0;
  wasFocused=focused;
  wasRenderable=renderable;
  if(!focused)capture(false);
  auto key=[&](int k){return glfwGetKey(window,k)==GLFW_PRESS;
  };
  if(focused){
   bool rightButton=glfwGetMouseButton(window,GLFW_MOUSE_BUTTON_RIGHT)==GLFW_PRESS;
   if(!rightButton)captureSuppressed=false;
   capture(rightButton&&!captureSuppressed);
   bool escape=key(GLFW_KEY_ESCAPE);
   if(escape&&!escapeHeld){if(captured){capture(false);
   captureSuppressed=true;
   }else glfwSetWindowShouldClose(window,GLFW_TRUE);
   }
   escapeHeld=escape;
   bool r=key(GLFW_KEY_R);
   if(r&&!rHeld){camera=Camera{};
   reset=true;
   }rHeld=r;
   if(captured){double x{},y{};
   glfwGetCursorPos(window,&x,&y);
   if(mouseInit){camera.yaw+=float(x-mouseX)*.002f;
   camera.pitch=std::clamp(camera.pitch-float(y-mouseY)*.002f,-1.553343f,1.553343f);
   }mouseX=x;
   mouseY=y;
   mouseInit=true;
   }
   float speed=3*(key(GLFW_KEY_LEFT_SHIFT)||key(GLFW_KEY_RIGHT_SHIFT)?3.0f:1.0f);
   V3 forward{std::sin(camera.yaw),0,-std::cos(camera.yaw)},move{};
   if(key(GLFW_KEY_W))move=move+forward;
   if(key(GLFW_KEY_S))move=move-forward;
   if(key(GLFW_KEY_D))move=move+camera.right();
   if(key(GLFW_KEY_A))move=move-camera.right();
   if(key(GLFW_KEY_E))move.y+=1;
   if(key(GLFW_KEY_Q))move.y-=1;
   if(length(move)>0)camera.position=camera.position+unit(move)*(speed*dt);
  }
 };
 while(!glfwWindowShouldClose(window)){
  updateInput();
  int width{},height{};
  glfwGetFramebufferSize(window,&width,&height);
  if(width==0||height==0){if(titleActive)resetTitle();
  glfwWaitEventsTimeout(.05);
  continue;
  }if(resize){rebuild();
  last=std::chrono::steady_clock::now();
  resetTitle();
  }if(glfwWindowShouldClose(window))break;
  if(!titleActive){titleStart=std::chrono::steady_clock::now();
  titleActive=true;
  }
  Slot& slot=slots[slotIndex];
  // 帧资源等待以短 timeout 处理输入，避免低帧率时把按键和失焦事件滞留在队列中。
  if(slot.done){
   auto waiting=vkinit<VkSemaphoreWaitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO);
   waiting.semaphoreCount=1;
   waiting.pSemaphores=&timeline;
   waiting.pValues=&slot.done;
   VkResult status;
   while((status=vkWaitSemaphores(device,&waiting,5000000))==VK_TIMEOUT){updateInput();
   if(glfwWindowShouldClose(window))break;
   }
   if(glfwWindowShouldClose(window))break;
   check(status,"frame slot wait");
  }
  if(resize)continue;
  check(vkResetCommandPool(device,slot.pool,0),"frame pool reset");
  uint32_t image{};
  VkResult acquired=vkAcquireNextImageKHR(device,swapchain,UINT64_MAX,slot.acquire,VK_NULL_HANDLE,&image);
  if(acquired==VK_ERROR_OUT_OF_DATE_KHR){resize=true;
  continue;
  }if(acquired!=VK_SUCCESS&&acquired!=VK_SUBOPTIMAL_KHR)check(acquired,"acquire");
  FrameGPU frame{};
  float tanFov=std::tan(3.14159265359f/6),aspect=float(extent.width)/float(extent.height);
  frame.cam[0]=f4(camera.position);
  frame.cam[1]=f4(camera.forward());
  frame.cam[2]=f4(camera.right(),tanFov*aspect);
  frame.cam[3]=f4(camera.up(),tanFov);
  frame.cam[4]=f4(previousCamera.position);
  frame.cam[5]=f4(previousCamera.forward());
  frame.cam[6]=f4(previousCamera.right(),tanFov*aspect);
  frame.cam[7]=f4(previousCamera.up(),tanFov);
  frame.extent={extent.width,extent.height,frameNumber,uint32_t(reset)};
  float jx=float(Scene::hash(frameNumber*2+1)>>8)/16777216.0f,jy=float(Scene::hash(frameNumber*2+2)>>8)/16777216.0f;
  frame.jitter={jx,jy,previousJitter.x,previousJitter.y};
  bool moved=length(camera.position-previousCamera.position)>0||camera.yaw!=previousCamera.yaw||camera.pitch!=previousCamera.pitch;
  frame.settings={uint32_t(scene.lights.size()),uint32_t(std::min(uint64_t(1024),uint64_t(properties.limits.maxComputeWorkGroupCount[0])*properties.limits.maxComputeWorkGroupSize[0])),scene.textureSize|(uint32_t(scene.ids.at("glass"))<<16),uint32_t(moved)};
  std::memcpy(slot.frame.mapped,&frame,sizeof(frame));
  record(slot,image);
  VkSemaphoreSubmitInfo waits[2]={vkinit<VkSemaphoreSubmitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO),vkinit<VkSemaphoreSubmitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO)};
  waits[0].semaphore=slot.acquire;
  waits[0].stageMask=VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  waits[1].semaphore=timeline;
  waits[1].value=submitted;
  waits[1].stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkSemaphoreSubmitInfo signals[2]={vkinit<VkSemaphoreSubmitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO),vkinit<VkSemaphoreSubmitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO)};
  signals[0].semaphore=timeline;
  signals[0].value=submitted+1;
  signals[0].stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  signals[1].semaphore=presentSemaphores[image];
  signals[1].stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  auto command=vkinit<VkCommandBufferSubmitInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO);
  command.commandBuffer=slot.command;
  auto submit=vkinit<VkSubmitInfo2>(VK_STRUCTURE_TYPE_SUBMIT_INFO_2);
  submit.waitSemaphoreInfoCount=2;
  submit.pWaitSemaphoreInfos=waits;
  submit.commandBufferInfoCount=1;
  submit.pCommandBufferInfos=&command;
  submit.signalSemaphoreInfoCount=2;
  submit.pSignalSemaphoreInfos=signals;
  check(vkQueueSubmit2(queue,1,&submit,VK_NULL_HANDLE),"render submit");
  slot.done=++submitted;
  previousCamera=camera;
  previousJitter=frame.jitter;
  reset=false;
  frameNumber++;
  slotIndex=1-slotIndex;
  auto present=vkinit<VkPresentInfoKHR>(VK_STRUCTURE_TYPE_PRESENT_INFO_KHR);
  present.waitSemaphoreCount=1;
  present.pWaitSemaphores=&presentSemaphores[image];
  present.swapchainCount=1;
  present.pSwapchains=&swapchain;
  present.pImageIndices=&image;
  VkResult result=vkQueuePresentKHR(queue,&present);
  presented[image]=true;
  if(result==VK_ERROR_OUT_OF_DATE_KHR||result==VK_SUBOPTIMAL_KHR||acquired==VK_SUBOPTIMAL_KHR)resize=true;
  else check(result,"present");
  if(result==VK_SUCCESS||result==VK_SUBOPTIMAL_KHR){
   // 用完整帧循环的墙钟时间统计吞吐，包含资源等待；这不是 GPU pass 的独立耗时。
   ++titleFrames;
   auto now=std::chrono::steady_clock::now();
   double seconds=std::chrono::duration<double>(now-titleStart).count();
   if(seconds>=.5){std::ostringstream title;
   title<<"MiniVoxelPathTracer | "<<std::fixed<<std::setprecision(1)<<double(titleFrames)/seconds
        <<" FPS | "<<std::setprecision(2)<<seconds*1000/double(titleFrames)<<" ms/frame";
   glfwSetWindowTitle(window,title.str().c_str());
   titleStart=now;
   titleFrames=0;
   }
  }
  if(failed)throw std::runtime_error("Vulkan validation 出现失败诊断");
 }
 wait(submitted);
 check(vkDeviceWaitIdle(device),"退出排空");
 if(failed)throw std::runtime_error("Vulkan validation 出现失败诊断");
}
void App::destroy(){
 if(device){
  VkResult drained=vkDeviceWaitIdle(device);
  if(drained!=VK_SUCCESS){failed=true;std::cerr<<"资源释放等待失败: "<<drained<<std::endl;}
  for(auto& b:frameBuffers)release(b);
  for(auto& slot:slots){release(slot.descriptor);
  release(slot.frame);
  if(slot.acquire)vkDestroySemaphore(device,slot.acquire,nullptr);
  if(slot.pool)vkDestroyCommandPool(device,slot.pool,nullptr);
  }
  for(auto sem:presentSemaphores)vkDestroySemaphore(device,sem,nullptr);
  for(auto v:swapViews)vkDestroyImageView(device,v,nullptr);
  if(swapchain)vkDestroySwapchainKHR(device,swapchain,nullptr);
  for(auto [name,shader]:shaders){(void)name;
  vkDestroyShaderEXT(device,shader,nullptr);
  }if(rtPipeline)vkDestroyPipeline(device,rtPipeline,nullptr);
  release(sbt);
  if(layout)vkDestroyPipelineLayout(device,layout,nullptr);
  if(setLayout)vkDestroyDescriptorSetLayout(device,setLayout,nullptr);
  if(sampler)vkDestroySampler(device,sampler,nullptr);
  release(roughTexture);
  release(colorTexture);
  release(tlas);
  release(glassBlas);
  release(blas);
  release(vertices);
  release(lightTable);
  release(materials);
  release(geometry);
  if(initPool)vkDestroyCommandPool(device,initPool,nullptr);
  if(timeline)vkDestroySemaphore(device,timeline,nullptr);
  vkDestroyDevice(device,nullptr);device=VK_NULL_HANDLE;
 }
 if(surface)vkDestroySurfaceKHR(instance,surface,nullptr);
 if(messenger)vkDestroyDebugUtilsMessengerEXT(instance,messenger,nullptr);
 if(instance)vkDestroyInstance(instance,nullptr);
 if(window)glfwDestroyWindow(window);
 glfwTerminate();surface=VK_NULL_HANDLE;messenger=VK_NULL_HANDLE;instance=VK_NULL_HANDLE;window=nullptr;
}
int main(int argc,char** argv){
 try{
  if(argc<1)throw std::runtime_error("缺少可执行路径");
  std::filesystem::path executable=argv[0];
  if(!executable.has_parent_path()&&!std::filesystem::is_regular_file(executable)){
   #ifdef _WIN32
   char* pathAllocation=nullptr;
   size_t pathLength=0;
   if(_dupenv_s(&pathAllocation,&pathLength,"PATH")!=0)throw std::runtime_error("无法读取 PATH");
   std::unique_ptr<char,decltype(&std::free)> pathOwner(pathAllocation,&std::free);
   const char* pathValue=pathOwner.get();
#else
   const char* pathValue=std::getenv("PATH");
#endif
   if(!pathValue)throw std::runtime_error("无法解析可执行路径");
#ifdef _WIN32
   constexpr char separator=';';
#else
   constexpr char separator=':';
#endif
   std::istringstream search(pathValue);
   std::string part;
   bool located=false;
   while(std::getline(search,part,separator)){auto candidate=std::filesystem::path(part)/executable;
   if(std::filesystem::is_regular_file(candidate)){executable=candidate;
   located=true;
   break;
   }}
   if(!located)throw std::runtime_error("PATH 中无法解析可执行路径");
  }
  executable=std::filesystem::absolute(executable);
  App app(std::filesystem::canonical(executable).parent_path());
  app.initialize();
  app.run();app.destroy();if(app.failed)throw std::runtime_error("退出时 Vulkan 验证或资源释放失败");
  return 0;
 }catch(const std::exception& e){std::cerr<<"MiniVoxelPathTracer: "<<e.what()<<"\n";
 return 1;
 }
}
