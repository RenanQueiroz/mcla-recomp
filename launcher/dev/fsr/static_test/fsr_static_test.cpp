// FSR's static runtime, linked as the PS5's title links it, run on this PC's
// GPU (CMakeLists.txt here says how it is built and run).
//
// Opens vulkan-1.dll at run time (nothing links a Vulkan loader library, as on
// the console), makes an instance and a device with the features the GPU plugin
// enables for FSR (src/ui/vulkan/vulkan_device.cpp), hands the plugin's file of
// forwarded Vulkan functions (src/graphics/vulkan/temporal_aa_static.cpp) its
// tables as the plugin does, then creates FSR with the plugin's flags
// (kTaaFsrCreateFlags) and dispatches it for a few frames of a flat picture.
// The output must be that picture. Exit code 0 when everything worked.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

#include <rex/ui/vulkan/device.h>

#include "frame_generation_fsr.h"
#include "temporal_aa_fsr.h"

#include <ffx_api/vk/ffx_api_vk.h>

namespace rex::graphics::vulkan {
void TaaSetStaticVulkanFunctions(const ui::vulkan::VulkanInstance::Functions* ifn,
                                 const ui::vulkan::VulkanDevice::Functions* dfn);
}

namespace {

using rex::ui::vulkan::VulkanDevice;
using rex::ui::vulkan::VulkanInstance;

constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;
constexpr float kColor[4] = {0.5f, 0.25f, 0.125f, 1.0f};

int failures = 0;

bool Check(bool ok, const char* what, long long code = 0) {
  std::printf("%s: %s", ok ? "ok" : "FAIL", what);
  if (!ok) {
    std::printf(" (%lld)", code);
    ++failures;
  }
  std::printf("\n");
  return ok;
}

void FfxMessage(uint32_t type, const wchar_t* message) {
  std::printf("FidelityFX %s: %ls\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning",
              message);
}

float HalfToFloat(uint16_t half) {
  const uint32_t sign = (half >> 15) & 1, exponent = (half >> 10) & 31, mantissa = half & 1023;
  float value;
  if (exponent == 0) {
    value = std::ldexp(float(mantissa), -24);
  } else if (exponent == 31) {
    value = mantissa ? NAN : INFINITY;
  } else {
    value = std::ldexp(float(mantissa | 1024), int(exponent) - 25);
  }
  return sign ? -value : value;
}

// A crash's address, for llvm-symbolizer (the program's own offset).
LONG WINAPI OnException(EXCEPTION_POINTERS* info) {
  HMODULE module = nullptr;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     static_cast<LPCSTR>(info->ExceptionRecord->ExceptionAddress), &module);
  char name[MAX_PATH] = "?";
  GetModuleFileNameA(module, name, MAX_PATH);
  std::printf("FAIL: exception %08lx at %s+0x%llx\n", info->ExceptionRecord->ExceptionCode, name,
              static_cast<unsigned long long>(
                  static_cast<const char*>(info->ExceptionRecord->ExceptionAddress) -
                  reinterpret_cast<const char*>(module)));
  return EXCEPTION_CONTINUE_SEARCH;
}

// What the plugin gives FidelityFX as vkGetDeviceProcAddr (vulkan/temporal_aa.cpp,
// TaaGetDeviceProcAddr): the device's, with the KHR names FidelityFX asks for
// answered by their Vulkan 1.1 and 1.3 core functions.
PFN_vkGetDeviceProcAddr device_proc_addr = nullptr;
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name) {
  if (PFN_vkVoidFunction function = device_proc_addr(device, name)) {
    return function;
  }
  static const char* const kAliases[][2] = {
      {"vkGetBufferMemoryRequirements2KHR", "vkGetBufferMemoryRequirements2"},
      {"vkGetImageMemoryRequirements2KHR", "vkGetImageMemoryRequirements2"},
      {"vkBindBufferMemory2KHR", "vkBindBufferMemory2"},
      {"vkBindImageMemory2KHR", "vkBindImageMemory2"},
      {"vkCmdPipelineBarrier2KHR", "vkCmdPipelineBarrier2"},
      {"vkGetDeviceBufferMemoryRequirementsKHR", "vkGetDeviceBufferMemoryRequirements"},
      {"vkGetDeviceImageMemoryRequirementsKHR", "vkGetDeviceImageMemoryRequirements"},
  };
  for (const auto& alias : kAliases) {
    if (!std::strcmp(name, alias[0])) {
      return device_proc_addr(device, alias[1]);
    }
  }
  return nullptr;
}

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImageUsageFlags usage = 0;
};

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  AddVectoredExceptionHandler(1, OnException);
  HMODULE loader = LoadLibraryA("vulkan-1.dll");
  if (!Check(loader != nullptr, "vulkan-1.dll loaded at run time")) {
    return 1;
  }
  auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      reinterpret_cast<void*>(GetProcAddress(loader, "vkGetInstanceProcAddr")));
  auto create_instance =
      reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
  VkApplicationInfo app_info = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app_info.pApplicationName = "fsr_static_test";
  app_info.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo instance_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  instance_info.pApplicationInfo = &app_info;
  VkInstance instance;
  VkResult result = create_instance(&instance_info, nullptr, &instance);
  if (!Check(result == VK_SUCCESS, "Vulkan 1.3 instance", result)) {
    return 1;
  }

  // The plugin's instance function table, filled as VulkanInstance fills it.
  VulkanInstance::Functions ifn = {};
  ifn.vkGetInstanceProcAddr = gipa;
  ifn.vkDestroyInstance =
      reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
#define XE_UI_VULKAN_FUNCTION(name) ifn.name = reinterpret_cast<PFN_##name>(gipa(instance, #name));
#define XE_UI_VULKAN_FUNCTION_PROMOTED(extension_name, core_name) \
  ifn.core_name = reinterpret_cast<PFN_##core_name>(gipa(instance, #core_name));
#include <rex/ui/vulkan/functions/instance_1_0.inc>
#include <rex/ui/vulkan/functions/instance_1_1_khr_get_physical_device_properties2.inc>
#undef XE_UI_VULKAN_FUNCTION
#undef XE_UI_VULKAN_FUNCTION_PROMOTED

  // A Vulkan 1.3 device, discrete if there is one.
  uint32_t count = 0;
  ifn.vkEnumeratePhysicalDevices(instance, &count, nullptr);
  std::vector<VkPhysicalDevice> physical_devices(count);
  ifn.vkEnumeratePhysicalDevices(instance, &count, physical_devices.data());
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties properties = {};
  for (VkPhysicalDevice candidate : physical_devices) {
    VkPhysicalDeviceProperties candidate_properties;
    ifn.vkGetPhysicalDeviceProperties(candidate, &candidate_properties);
    if (candidate_properties.apiVersion < VK_API_VERSION_1_3) {
      continue;
    }
    if (physical_device == VK_NULL_HANDLE ||
        candidate_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
      physical_device = candidate;
      properties = candidate_properties;
    }
  }
  if (!Check(physical_device != VK_NULL_HANDLE, "a Vulkan 1.3 GPU")) {
    return 1;
  }
  std::printf("GPU: %s\n", properties.deviceName);
  ifn.vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  ifn.vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, families.data());
  uint32_t family = UINT32_MAX;
  for (uint32_t i = 0; i < count; ++i) {
    if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
        (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
      family = i;
      break;
    }
  }

  // The features vulkan_device.cpp enables for FSR, where the GPU has them.
  VkPhysicalDeviceVulkan13Features supported_13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan12Features supported_12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                                  &supported_13};
  VkPhysicalDeviceVulkan11Features supported_11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                                  &supported_12};
  VkPhysicalDeviceFeatures2 supported = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &supported_11};
  ifn.vkGetPhysicalDeviceFeatures2(physical_device, &supported);
  VkPhysicalDeviceVulkan13Features enabled_13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan12Features enabled_12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                                &enabled_13};
  VkPhysicalDeviceVulkan11Features enabled_11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                                &enabled_12};
  VkPhysicalDeviceFeatures2 enabled = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &enabled_11};
  enabled.features.shaderInt16 = supported.features.shaderInt16;
  enabled.features.shaderStorageImageExtendedFormats = supported.features.shaderStorageImageExtendedFormats;
  enabled_11.storageBuffer16BitAccess = supported_11.storageBuffer16BitAccess;
  enabled_12.shaderFloat16 = supported_12.shaderFloat16;
  enabled_13.subgroupSizeControl = supported_13.subgroupSizeControl;
  std::printf("features: shaderInt16 %u, extended storage formats %u, 16-bit storage %u, "
              "shaderFloat16 %u, subgroup size control %u\n",
              enabled.features.shaderInt16, enabled.features.shaderStorageImageExtendedFormats,
              enabled_11.storageBuffer16BitAccess, enabled_12.shaderFloat16,
              enabled_13.subgroupSizeControl);
  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue_info.queueFamilyIndex = family;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;
  VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &enabled};
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  VkDevice device;
  result = ifn.vkCreateDevice(physical_device, &device_info, nullptr, &device);
  if (!Check(result == VK_SUCCESS, "device with FSR's features", result)) {
    return 1;
  }

  // The plugin's device function table.
  VulkanDevice::Functions dfn = {};
#define XE_UI_VULKAN_FUNCTION(name) \
  dfn.name = reinterpret_cast<PFN_##name>(ifn.vkGetDeviceProcAddr(device, #name));
#define XE_UI_VULKAN_FUNCTION_PROMOTED(extension_name, core_name) \
  dfn.core_name = reinterpret_cast<PFN_##core_name>(ifn.vkGetDeviceProcAddr(device, #core_name));
#include <rex/ui/vulkan/functions/device_1_0.inc>
#undef XE_UI_VULKAN_FUNCTION
#undef XE_UI_VULKAN_FUNCTION_PROMOTED
  // What VulkanCommandProcessor does before it creates FSR (REX_FSR_STATIC).
  rex::graphics::vulkan::TaaSetStaticVulkanFunctions(&ifn, &dfn);
  std::printf("tables handed to the plugin's Vulkan functions\n");
  VkQueue queue;
  dfn.vkGetDeviceQueue(device, family, 0, &queue);

  // The images FSR reads and writes, as the plugin passes them.
  VkPhysicalDeviceMemoryProperties memory_properties;
  ifn.vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
  auto memory_type = [&](uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
      if ((bits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & flags) == flags) {
        return i;
      }
    }
    return UINT32_MAX;
  };
  auto make_image = [&](VkFormat format, VkImageUsageFlags usage) {
    Image image;
    image.format = format;
    image.usage = usage;
    VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {kWidth, kHeight, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    dfn.vkCreateImage(device, &info, nullptr, &image.image);
    VkMemoryRequirements requirements;
    dfn.vkGetImageMemoryRequirements(device, image.image, &requirements);
    VkMemoryAllocateInfo allocate = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex =
        memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    dfn.vkAllocateMemory(device, &allocate, nullptr, &image.memory);
    dfn.vkBindImageMemory(device, image.image, image.memory, 0);
    return image;
  };
  constexpr VkImageUsageFlags kInput = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  Image color = make_image(VK_FORMAT_R16G16B16A16_SFLOAT, kInput);
  Image depth = make_image(VK_FORMAT_R32_SFLOAT, kInput);
  Image vectors = make_image(VK_FORMAT_R16G16_SFLOAT, kInput | VK_IMAGE_USAGE_STORAGE_BIT);
  Image output = make_image(VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT |
                                                              VK_IMAGE_USAGE_STORAGE_BIT |
                                                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
  auto resource = [](const Image& image, uint32_t state) {
    VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = image.format;
    info.extent = {kWidth, kHeight, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.usage = image.usage;
    return ffxApiGetResourceVK(image.image, ffxApiGetImageResourceDescriptionVK(image.image, info, 0),
                               state);
  };
  // A buffer to read the output's centre pixel back.
  VkBufferCreateInfo buffer_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = 8;
  buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkBuffer readback;
  dfn.vkCreateBuffer(device, &buffer_info, nullptr, &readback);
  VkMemoryRequirements buffer_requirements;
  dfn.vkGetBufferMemoryRequirements(device, readback, &buffer_requirements);
  VkMemoryAllocateInfo buffer_allocate = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  buffer_allocate.allocationSize = buffer_requirements.size;
  buffer_allocate.memoryTypeIndex = memory_type(
      buffer_requirements.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VkDeviceMemory readback_memory;
  dfn.vkAllocateMemory(device, &buffer_allocate, nullptr, &readback_memory);
  dfn.vkBindBufferMemory(device, readback, readback_memory, 0);

  std::printf("images and the read-back buffer made\n");
  // FSR, as the plugin creates it (vulkan/temporal_aa.cpp).
  ffxCreateBackendVKDesc backend = {};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
  backend.vkDevice = device;
  backend.vkPhysicalDevice = physical_device;
  device_proc_addr = ifn.vkGetDeviceProcAddr;
  backend.vkDeviceProcAddr = GetDeviceProcAddr;
  ffxCreateContextDescUpscale create = {};
  create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  create.header.pNext = &backend.header;
  create.flags = rex::graphics::kTaaFsrCreateFlags;
  create.maxRenderSize = {kWidth, kHeight};
  create.maxUpscaleSize = {kWidth, kHeight};
  create.fpMessage = FfxMessage;
  ffxContext context = nullptr;
  ffxReturnCode_t ffx_result = ffxCreateContext(&context, &create.header, nullptr);
  if (!Check(ffx_result == FFX_API_RETURN_OK, "ffxCreateContext (FSR 3.1, the plugin's flags)",
             ffx_result)) {
    return 1;
  }

  VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.queueFamilyIndex = family;
  VkCommandPool pool;
  dfn.vkCreateCommandPool(device, &pool_info, nullptr, &pool);
  VkCommandBufferAllocateInfo command_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  command_info.commandPool = pool;
  command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command_info.commandBufferCount = 1;
  VkCommandBuffer command_buffer;
  dfn.vkAllocateCommandBuffers(device, &command_info, &command_buffer);
  VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  dfn.vkBeginCommandBuffer(command_buffer, &begin);
  auto barrier = [&](const Image& image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dfn.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
  };
  // The inputs: a flat colour, depth and no motion.
  const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkClearColorValue clear_color = {{kColor[0], kColor[1], kColor[2], kColor[3]}};
  VkClearColorValue clear_depth = {{0.5f, 0.0f, 0.0f, 0.0f}};
  VkClearColorValue clear_zero = {{0.0f, 0.0f, 0.0f, 0.0f}};
  for (const Image* image : {&color, &depth, &vectors}) {
    barrier(*image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  }
  dfn.vkCmdClearColorImage(command_buffer, color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           &clear_color, 1, &range);
  dfn.vkCmdClearColorImage(command_buffer, depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           &clear_depth, 1, &range);
  dfn.vkCmdClearColorImage(command_buffer, vectors.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           &clear_zero, 1, &range);
  for (const Image* image : {&color, &depth, &vectors}) {
    barrier(*image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  barrier(output, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

  // Eight frames, as the plugin dispatches it (with sharpening, its default).
  constexpr int kFrames = 8;
  for (int frame = 0; frame < kFrames && ffx_result == FFX_API_RETURN_OK; ++frame) {
    ffxDispatchDescUpscale dispatch = {};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dispatch.commandList = command_buffer;
    dispatch.color = resource(color, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dispatch.depth = resource(depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dispatch.motionVectors = resource(vectors, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dispatch.output = resource(output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.jitterOffset = {frame % 2 ? 0.25f : -0.25f, frame % 3 ? -0.25f : 0.25f};
    dispatch.motionVectorScale = {1.0f, 1.0f};
    dispatch.renderSize = {kWidth, kHeight};
    dispatch.upscaleSize = {kWidth, kHeight};
    dispatch.enableSharpening = true;
    dispatch.sharpness = 0.2f;
    dispatch.frameTimeDelta = 16.7f;
    dispatch.preExposure = 1.0f;
    dispatch.reset = frame == 0;
    dispatch.cameraNear = 0.5f;
    dispatch.cameraFar = 5000.0f;
    dispatch.cameraFovAngleVertical = 1.0f;
    dispatch.viewSpaceToMetersFactor = 1.0f;
    ffx_result = ffxDispatch(&context, &dispatch.header);
  }
  Check(ffx_result == FFX_API_RETURN_OK, "ffxDispatch, 8 frames", ffx_result);
  barrier(output, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkBufferImageCopy copy = {};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageOffset = {int32_t(kWidth / 2), int32_t(kHeight / 2), 0};
  copy.imageExtent = {1, 1, 1};
  dfn.vkCmdCopyImageToBuffer(command_buffer, output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             readback, 1, &copy);
  dfn.vkEndCommandBuffer(command_buffer);
  VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command_buffer;
  result = dfn.vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
  Check(result == VK_SUCCESS, "submission", result);
  result = dfn.vkQueueWaitIdle(queue);
  Check(result == VK_SUCCESS, "the GPU finished (no device loss)", result);

  // The output of a flat picture is that picture.
  void* mapped = nullptr;
  dfn.vkMapMemory(device, readback_memory, 0, 8, 0, &mapped);
  const uint16_t* half = static_cast<const uint16_t*>(mapped);
  float pixel[4];
  for (int i = 0; i < 4; ++i) {
    pixel[i] = HalfToFloat(half[i]);
  }
  dfn.vkUnmapMemory(device, readback_memory);
  std::printf("output centre: %.4f %.4f %.4f (input %.4f %.4f %.4f)\n", pixel[0], pixel[1], pixel[2],
              kColor[0], kColor[1], kColor[2]);
  bool same = true;
  for (int i = 0; i < 3; ++i) {
    same = same && std::fabs(pixel[i] - kColor[i]) <= 0.01f + 0.02f * kColor[i];
  }
  Check(same, "FSR's output is the input picture");

  ffx_result = ffxDestroyContext(&context, nullptr);
  Check(ffx_result == FFX_API_RETURN_OK, "ffxDestroyContext", ffx_result);

  // Frame generation, as the plugin creates it (frame_generation_fsr.h): manual
  // (no swapchain), the scene in FP16 code values told sRGB. Four frames of a
  // still, flat picture: the generated frame is that picture.
  ffxCreateContextDescFrameGeneration fg_create = {};
  fg_create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
  fg_create.header.pNext = &backend.header;
  fg_create.flags = rex::graphics::kFgCreateFlags;
  fg_create.displaySize = {kWidth, kHeight};
  fg_create.maxRenderSize = {kWidth, kHeight};
  fg_create.backBufferFormat = FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
  ffxContext fg = nullptr;
  ffx_result = ffxCreateContext(&fg, &fg_create.header, nullptr);
  if (Check(ffx_result == FFX_API_RETURN_OK, "ffxCreateContext (frame generation, the plugin's flags)",
            ffx_result)) {
    dfn.vkResetCommandPool(device, pool, 0);
    dfn.vkBeginCommandBuffer(command_buffer, &begin);
    barrier(output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    for (uint64_t frame = 1; frame <= 4 && ffx_result == FFX_API_RETURN_OK; ++frame) {
      ffxConfigureDescFrameGeneration configure = {};
      configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
      configure.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
      configure.frameGenerationEnabled = true;
      configure.frameID = frame;
      ffx_result = ffxConfigure(&fg, &configure.header);
      if (ffx_result != FFX_API_RETURN_OK) {
        break;
      }
      ffxDispatchDescFrameGenerationPrepare prepare = {};
      prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
      prepare.frameID = frame;
      prepare.commandList = command_buffer;
      prepare.renderSize = {kWidth, kHeight};
      prepare.motionVectorScale = {1.0f, 1.0f};
      prepare.frameTimeDelta = 8.3f;
      prepare.cameraNear = 0.5f;
      prepare.cameraFar = 5000.0f;
      prepare.cameraFovAngleVertical = 1.0f;
      prepare.viewSpaceToMetersFactor = 1.0f;
      prepare.depth = resource(depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
      prepare.motionVectors = resource(vectors, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
      ffx_result = ffxDispatch(&fg, &prepare.header);
      if (ffx_result != FFX_API_RETURN_OK) {
        break;
      }
      ffxDispatchDescFrameGeneration generate = {};
      generate.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
      generate.commandList = command_buffer;
      generate.presentColor = resource(color, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
      generate.outputs[0] = resource(output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
      generate.numGeneratedFrames = 1;
      generate.reset = frame == 1;
      generate.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
      generate.minMaxLuminance[0] = 0.0f;
      generate.minMaxLuminance[1] = 1.0f;
      generate.frameID = frame;
      ffx_result = ffxDispatch(&fg, &generate.header);
    }
    Check(ffx_result == FFX_API_RETURN_OK, "frame generation: configure, prepare, generate, 4 frames",
          ffx_result);
    barrier(output, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    dfn.vkCmdCopyImageToBuffer(command_buffer, output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback, 1, &copy);
    dfn.vkEndCommandBuffer(command_buffer);
    result = dfn.vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    Check(result == VK_SUCCESS, "frame generation submission", result);
    result = dfn.vkQueueWaitIdle(queue);
    Check(result == VK_SUCCESS, "the GPU finished frame generation (no device loss)", result);
    dfn.vkMapMemory(device, readback_memory, 0, 8, 0, &mapped);
    half = static_cast<const uint16_t*>(mapped);
    for (int i = 0; i < 4; ++i) {
      pixel[i] = HalfToFloat(half[i]);
    }
    dfn.vkUnmapMemory(device, readback_memory);
    std::printf("generated centre: %.4f %.4f %.4f (input %.4f %.4f %.4f)\n", pixel[0], pixel[1],
                pixel[2], kColor[0], kColor[1], kColor[2]);
    bool same_generated = true;
    for (int i = 0; i < 3; ++i) {
      same_generated = same_generated && std::fabs(pixel[i] - kColor[i]) <= 0.01f + 0.02f * kColor[i];
    }
    Check(same_generated, "the generated frame of a still picture is that picture");
    ffx_result = ffxDestroyContext(&fg, nullptr);
    Check(ffx_result == FFX_API_RETURN_OK, "ffxDestroyContext (frame generation)", ffx_result);
  }
  dfn.vkDestroyCommandPool(device, pool, nullptr);
  for (Image* image : {&color, &depth, &vectors, &output}) {
    dfn.vkDestroyImage(device, image->image, nullptr);
    dfn.vkFreeMemory(device, image->memory, nullptr);
  }
  dfn.vkDestroyBuffer(device, readback, nullptr);
  dfn.vkFreeMemory(device, readback_memory, nullptr);
  ifn.vkDestroyDevice(device, nullptr);
  ifn.vkDestroyInstance(instance, nullptr);
  std::printf("%s\n", failures ? "FAILED" : "PASSED");
  return failures ? 1 : 0;
}
