// Temporal upscalers -- AMD FSR 3 and NVIDIA DLSS -- on a Vulkan device of the port's own
// (albw_vkup.h).
#include "albw_vkup.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <vector>

#if !defined(ALBW_HAVE_FSR3) && !defined(ALBW_HAVE_DLSS)

int albw_fsr3_available(void) { return 0; }
int albw_dlss_available(const char **why) {
    if (why) *why = "Not in this build: it needs NVIDIA's DLSS SDK in external/.";
    return 0;
}
void albw_vkup_jitter(uint32_t, int, int, float *x, float *y) { *x = *y = 0.0f; }
int albw_fsr3_upscale(const struct albw_vkup_frame *, uint32_t *, int, int, int) { return 0; }
int albw_dlss_upscale(const struct albw_vkup_frame *, uint32_t *, int, int, int) { return 0; }
void albw_vkup_close(void) {}

#else

#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#ifdef ALBW_HAVE_FSR3
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>

// The backend hands this to frame generation's own swapchain, which the port does not use: it
// shows its frames through SDL. Defined here so the backend links without that swapchain's sources.
FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const *) {
    return FFX_ERROR_INVALID_ARGUMENT;
}
#endif
#ifdef ALBW_HAVE_DLSS
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>
#endif

namespace {

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t w = 0, h = 0;
};

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = nullptr;
    VkDeviceSize size = 0;
};

struct Device {
    int tried = 0, ok = 0;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkPhysicalDeviceMemoryProperties memory;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
} dev;

// One upscaler's surfaces: the inputs at the picture's size, the output at the window's, and the
// buffers they go up and come down through.
struct Surfaces {
    int render_w = 0, render_h = 0, out_w = 0, out_h = 0;
    Image color, depth, motion, reactive, output;
    Buffer upload, readback;
};

bool said(const char *what, VkResult r) {
    std::printf("[vkup ] %s failed (%d); FSR 3 and DLSS are not available\n", what, (int)r);
    return false;
}

// The FSR backend asks for some functions by their extension names -- vkGetBufferMemoryRequirements2KHR
// and the like -- which a Vulkan 1.3 device only answers to when the extension is enabled, and then
// calls them without looking. Where the extension name finds nothing, the core name it was promoted
// to does.
PFN_vkVoidFunction device_proc(VkDevice device, const char *name) {
    PFN_vkVoidFunction f = vkGetDeviceProcAddr(device, name);
    if (f) return f;
    size_t n = std::strlen(name);
    if (n > 3 && (!std::strcmp(name + n - 3, "KHR") || !std::strcmp(name + n - 3, "EXT"))) {
        char core[128];
        if (n - 3 < sizeof core) {
            std::memcpy(core, name, n - 3);
            core[n - 3] = '\0';
            f = vkGetDeviceProcAddr(device, core);
        }
    }
    return f;
}

uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < dev.memory.memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (dev.memory.memoryTypes[i].propertyFlags & want) == want) return i;
    }
    return UINT32_MAX;
}

bool has_extension(const std::vector<VkExtensionProperties> &list, const char *name) {
    for (const VkExtensionProperties &e : list) {
        if (!std::strcmp(e.extensionName, name)) return true;
    }
    return false;
}

bool open_device() {
    // The extensions NGX asks for, where the loader and the device have them: DLSS runs only on
    // NVIDIA's cards, which have them all; anywhere else they are simply left out.
    std::vector<const char *> inst_ext, dev_ext_ngx;
#ifdef ALBW_HAVE_DLSS
    {
        unsigned int ni = 0, nd = 0;
        const char **ie = nullptr, **de = nullptr;
        if (!NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_RequiredExtensions(&ni, &ie, &nd, &de))) {
            uint32_t n = 0;
            vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
            std::vector<VkExtensionProperties> have(n);
            vkEnumerateInstanceExtensionProperties(nullptr, &n, have.data());
            for (unsigned i = 0; i < ni; i++) {
                if (has_extension(have, ie[i])) inst_ext.push_back(ie[i]);
            }
            for (unsigned i = 0; i < nd; i++) dev_ext_ngx.push_back(de[i]);
        }
    }
#endif
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "World Across";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = (uint32_t)inst_ext.size();
    ici.ppEnabledExtensionNames = inst_ext.data();
    // ALBW_VK_VALIDATE=1 asks for Khronos' validation layer, where it is installed.
    const char *layers[] = { "VK_LAYER_KHRONOS_validation" };
    const char *v = std::getenv("ALBW_VK_VALIDATE");
    if (v && *v && *v != '0') {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = layers;
    }
    VkResult r = vkCreateInstance(&ici, nullptr, &dev.instance);
    if (r != VK_SUCCESS && ici.enabledLayerCount) {
        ici.enabledLayerCount = 0;
        r = vkCreateInstance(&ici, nullptr, &dev.instance);
    }
    if (r != VK_SUCCESS) return said("creating a Vulkan instance", r);

    // A discrete GPU before an integrated one before anything else, with Vulkan 1.3 and a queue
    // that computes.
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(dev.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> all(n);
    vkEnumeratePhysicalDevices(dev.instance, &n, all.data());
    int best = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(all[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        int rank = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
                 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (rank > best) {
            best = rank;
            dev.phys = all[i];
        }
    }
    if (!dev.phys) return said("finding a Vulkan 1.3 device", VK_ERROR_INITIALIZATION_FAILED);
    vkGetPhysicalDeviceProperties(dev.phys, &dev.props);
    vkGetPhysicalDeviceMemoryProperties(dev.phys, &dev.memory);

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev.phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(dev.phys, &qn, qf.data());
    dev.family = UINT32_MAX;
    // A queue that draws as well as computes: NGX records its work as graphics work.
    for (uint32_t i = 0; i < qn && dev.family == UINT32_MAX; i++) {
        if ((qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) dev.family = i;
    }
    for (uint32_t i = 0; i < qn && dev.family == UINT32_MAX; i++) {
        if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) dev.family = i;
    }
    if (dev.family == UINT32_MAX) return said("finding a compute queue", VK_ERROR_INITIALIZATION_FAILED);

    // What FSR 3's shaders use: storage images read and written without a declared format, and,
    // where the device has them, half floats and a chosen subgroup size.
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    f12.pNext = &f13;
    VkPhysicalDeviceFeatures2 have = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    have.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(dev.phys, &have);
    if (!have.features.shaderStorageImageReadWithoutFormat
            || !have.features.shaderStorageImageWriteWithoutFormat) {
        return said("finding storage images without a format", VK_ERROR_FEATURE_NOT_PRESENT);
    }
    VkPhysicalDeviceVulkan13Features w13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    w13.subgroupSizeControl = f13.subgroupSizeControl;
    w13.computeFullSubgroups = f13.computeFullSubgroups;
    VkPhysicalDeviceVulkan12Features w12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    w12.shaderFloat16 = f12.shaderFloat16;
    w12.bufferDeviceAddress = f12.bufferDeviceAddress;   // NGX's, on NVIDIA's cards
    w12.pNext = &w13;
    VkPhysicalDeviceFeatures2 want = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    want.features.shaderStorageImageReadWithoutFormat = VK_TRUE;
    want.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    want.features.shaderInt16 = have.features.shaderInt16;
    want.pNext = &w12;

    // The extensions the FSR backend looks for and then uses, and NGX's, where the device has them.
    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(dev.phys, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> ext(en);
    vkEnumerateDeviceExtensionProperties(dev.phys, nullptr, &en, ext.data());
    std::vector<const char *> enable;
    static const char *const wanted[] = {
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
        VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME,
    };
    for (const char *w : wanted) {
        if (has_extension(ext, w)) enable.push_back(w);
    }
    for (const char *w : dev_ext_ngx) {
        bool dup = false;
        for (const char *e : enable) dup |= !std::strcmp(e, w);
        if (!dup && has_extension(ext, w)) enable.push_back(w);
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = dev.family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &want;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)enable.size();
    dci.ppEnabledExtensionNames = enable.data();
    r = vkCreateDevice(dev.phys, &dci, nullptr, &dev.device);
    if (r != VK_SUCCESS) return said("creating the Vulkan device", r);
    vkGetDeviceQueue(dev.device, dev.family, 0, &dev.queue);

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = dev.family;
    if ((r = vkCreateCommandPool(dev.device, &pci, nullptr, &dev.pool)) != VK_SUCCESS)
        return said("creating a command pool", r);
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = dev.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if ((r = vkAllocateCommandBuffers(dev.device, &cai, &dev.cmd)) != VK_SUCCESS)
        return said("allocating a command buffer", r);
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if ((r = vkCreateFence(dev.device, &fci, nullptr, &dev.fence)) != VK_SUCCESS)
        return said("creating a fence", r);
    std::printf("[vkup ] the upscalers' Vulkan device: %s\n", dev.props.deviceName);
    return true;
}

bool device_ready() {
    if (!dev.tried) {
        dev.tried = 1;
        dev.ok = open_device();
    }
    return dev.ok;
}

void free_image(Image &img) {
    if (img.view) vkDestroyImageView(dev.device, img.view, nullptr);
    if (img.image) vkDestroyImage(dev.device, img.image, nullptr);
    if (img.memory) vkFreeMemory(dev.device, img.memory, nullptr);
    img = Image();
}

bool make_image(Image &img, uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage) {
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = { w, h, 1 };
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = vkCreateImage(dev.device, &ci, nullptr, &img.image);
    if (r != VK_SUCCESS) return said("creating an image", r);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev.device, img.image, &req);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if ((r = vkAllocateMemory(dev.device, &ai, nullptr, &img.memory)) != VK_SUCCESS)
        return said("allocating image memory", r);
    vkBindImageMemory(dev.device, img.image, img.memory, 0);
    // A view as well, which NGX takes its resources by.
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if ((r = vkCreateImageView(dev.device, &vi, nullptr, &img.view)) != VK_SUCCESS)
        return said("creating an image view", r);
    img.w = w;
    img.h = h;
    img.format = format;
    img.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

void free_buffer(Buffer &b) {
    if (b.buffer) vkDestroyBuffer(dev.device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(dev.device, b.memory, nullptr);
    b = Buffer();
}

bool make_buffer(Buffer &b, VkDeviceSize size, VkBufferUsageFlags usage, bool read_back) {
    VkBufferCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    ci.size = size;
    ci.usage = usage;
    VkResult r = vkCreateBuffer(dev.device, &ci, nullptr, &b.buffer);
    if (r != VK_SUCCESS) return said("creating a buffer", r);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev.device, b.buffer, &req);
    VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    // What comes back is read by the processor, so it wants its cache; what goes up is written once
    // in order, so it goes straight into the card's own memory where the card lets it be mapped --
    // all of it, with resizable BAR -- and the copies into the images never cross the bus.
    uint32_t type = read_back ? memory_type(req.memoryTypeBits, want | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
                              : memory_type(req.memoryTypeBits, want | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) type = memory_type(req.memoryTypeBits, want);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if ((r = vkAllocateMemory(dev.device, &ai, nullptr, &b.memory)) != VK_SUCCESS)
        return said("allocating buffer memory", r);
    vkBindBufferMemory(dev.device, b.buffer, b.memory, 0);
    vkMapMemory(dev.device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped);
    b.size = size;
    return true;
}

void free_surfaces(Surfaces &s) {
    free_image(s.color);
    free_image(s.depth);
    free_image(s.motion);
    free_image(s.reactive);
    free_image(s.output);
    free_buffer(s.upload);
    free_buffer(s.readback);
    s.render_w = s.render_h = s.out_w = s.out_h = 0;
}

bool make_surfaces(Surfaces &s, int render_w, int render_h, int out_w, int out_h) {
    free_surfaces(s);
    const VkImageUsageFlags input = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!make_image(s.color, render_w, render_h, VK_FORMAT_R8G8B8A8_UNORM, input)
            || !make_image(s.depth, render_w, render_h, VK_FORMAT_R32_SFLOAT, input)
            || !make_image(s.motion, render_w, render_h, VK_FORMAT_R32G32_SFLOAT, input)
            || !make_image(s.reactive, render_w, render_h, VK_FORMAT_R8_UNORM, input)
            || !make_image(s.output, out_w, out_h, VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                               | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        return false;
    }
    VkDeviceSize pixels = (VkDeviceSize)render_w * render_h;
    if (!make_buffer(s.upload, pixels * (4 + 4 + 8 + 4), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false)
            || !make_buffer(s.readback, (VkDeviceSize)out_w * out_h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            true)) {
        return false;
    }
    s.render_w = render_w;
    s.render_h = render_h;
    s.out_w = out_w;
    s.out_h = out_h;
    return true;
}

void barrier(Image &img, VkImageLayout to, VkAccessFlags src_access, VkAccessFlags dst_access,
             VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = img.layout;
    b.newLayout = to;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(dev.cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
    img.layout = to;
}

void begin() {
    vkResetFences(dev.device, 1, &dev.fence);
    vkResetCommandBuffer(dev.cmd, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(dev.cmd, &bi);
}

bool finish() {
    vkEndCommandBuffer(dev.cmd);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &dev.cmd;
    if (vkQueueSubmit(dev.queue, 1, &si, dev.fence) != VK_SUCCESS) return false;
    vkWaitForFences(dev.device, 1, &dev.fence, VK_TRUE, UINT64_MAX);
    return true;
}

// The frame's inputs into the upload buffer and from there into their images, which are left
// readable by shaders; the output is left writable.
void record_inputs(Surfaces &s, const struct albw_vkup_frame *in) {
    const size_t pixels = (size_t)s.render_w * s.render_h;
    uint8_t *color = (uint8_t *)s.upload.mapped;
    float *depth = (float *)(color + pixels * 4);
    float *motion = depth + pixels;
    uint8_t *reactive = (uint8_t *)(motion + pixels * 2);
    if (in->rgba) {
        std::memcpy(color, in->rgba, pixels * 4);
    } else {
        for (size_t i = 0; i < pixels; i++) {
            color[i * 4 + 0] = in->rgb[i * 3 + 0];
            color[i * 4 + 1] = in->rgb[i * 3 + 1];
            color[i * 4 + 2] = in->rgb[i * 3 + 2];
            color[i * 4 + 3] = 255;
        }
    }
    std::memcpy(depth, in->depth, pixels * sizeof(float));
    std::memcpy(motion, in->motion, pixels * 2 * sizeof(float));
    if (in->reactive) std::memcpy(reactive, in->reactive, pixels);
    else std::memset(reactive, 0, pixels);

    Image *inputs[4] = { &s.color, &s.depth, &s.motion, &s.reactive };
    VkDeviceSize offsets[4] = { 0, pixels * 4, pixels * 8, pixels * 16 };
    for (int i = 0; i < 4; i++) {
        barrier(*inputs[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy = {};
        copy.bufferOffset = offsets[i];
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { (uint32_t)s.render_w, (uint32_t)s.render_h, 1 };
        vkCmdCopyBufferToImage(dev.cmd, s.upload.buffer, inputs[i]->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(*inputs[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    barrier(s.output, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

// The output down into the read-back buffer.
void record_output(Surfaces &s) {
    barrier(s.output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy = {};
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageExtent = { (uint32_t)s.out_w, (uint32_t)s.out_h, 1 };
    vkCmdCopyImageToBuffer(dev.cmd, s.output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           s.readback.buffer, 1, &copy);
    VkBufferMemoryBarrier host = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = s.readback.buffer;
    host.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(dev.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                         nullptr, 1, &host, 0, nullptr);
}

void copy_out(Surfaces &s, uint32_t *out, int out_pitch, bool rgba) {
    VkMappedMemoryRange range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
    range.memory = s.readback.memory;
    range.size = VK_WHOLE_SIZE;
    vkInvalidateMappedMemoryRanges(dev.device, 1, &range);
    const uint8_t *src = (const uint8_t *)s.readback.mapped;
    if (rgba) {
        for (int y = 0; y < s.out_h; y++) {
            std::memcpy((uint8_t *)out + (size_t)y * out_pitch, src + (size_t)y * s.out_w * 4,
                        (size_t)s.out_w * 4);
        }
        return;
    }
    for (int y = 0; y < s.out_h; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)out + (size_t)y * out_pitch);
        const uint8_t *p = src + (size_t)y * s.out_w * 4;
        for (int x = 0; x < s.out_w; x++, p += 4) {
            row[x] = 0xFF000000u | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        }
    }
}

// ---------------------------------------------------------------------------------------------
// AMD FSR 3.
#ifdef ALBW_HAVE_FSR3
struct Fsr3 {
    Surfaces s;
    // The backend's working memory. It keeps structures there that GCC aligns to more than malloc
    // promises, so it is allocated aligned to far more than any of them needs.
    void *scratch = nullptr;
    FfxInterface iface;
    FfxFsr3UpscalerContext *context = nullptr;
    bool context_ok = false, fresh = false;
    // FSR 3.1's upscaler leaves three of its working surfaces to the application -- dilated depth,
    // dilated motion and the previous frame's depth -- so that frame generation can share them.
    // They are made through the backend, in a context of their own, as AMD's FSR 3 wrapper does.
    uint32_t shared_id = 0;
    bool shared_ok = false;
    FfxResourceInternal shared[3];
} fsr;

void message(FfxMsgType type, const wchar_t *text) {
    static unsigned shown;
    if (shown++ < 20) {
        std::printf("[fsr3 ] %s: %ls\n", type == FFX_MESSAGE_TYPE_ERROR ? "error" : "warning", text);
    }
}

void fsr3_destroy() {
    if (dev.device) vkDeviceWaitIdle(dev.device);
    if (fsr.shared_ok) {
        for (FfxResourceInternal &r : fsr.shared) fsr.iface.fpDestroyResource(&fsr.iface, r, fsr.shared_id);
        fsr.iface.fpDestroyBackendContext(&fsr.iface, fsr.shared_id);
    }
    fsr.shared_ok = false;
    if (fsr.context_ok) ffxFsr3UpscalerContextDestroy(fsr.context);
    fsr.context_ok = false;
    std::free(fsr.context);
    fsr.context = nullptr;
    std::free(fsr.scratch);
    fsr.scratch = nullptr;
    free_surfaces(fsr.s);
}

bool fsr3_make(int render_w, int render_h, int out_w, int out_h) {
    fsr3_destroy();
    if (!make_surfaces(fsr.s, render_w, render_h, out_w, out_h)) return false;
    // Room for the upscaler's backend context and the shared surfaces' one.
    const size_t contexts = FFX_FSR3UPSCALER_CONTEXT_COUNT + 1;
    size_t scratch = ffxGetScratchMemorySizeVK(dev.phys, contexts);
    scratch = (scratch + 255) & ~(size_t)255;
    fsr.scratch = std::aligned_alloc(256, scratch);
    if (!fsr.scratch) return false;
    std::memset(fsr.scratch, 0, scratch);
    VkDeviceContext vk = { dev.device, dev.phys, device_proc };
    FfxFsr3UpscalerContextDescription desc;
    std::memset(&desc, 0, sizeof desc);
    FfxErrorCode e = ffxGetInterfaceVK(&desc.backendInterface, ffxGetDeviceVK(&vk), fsr.scratch,
                                       scratch, contexts);
    if (e != FFX_OK) {
        std::printf("[fsr3 ] the FidelityFX Vulkan backend did not start (%d)\n", (int)e);
        return false;
    }
    fsr.iface = desc.backendInterface;
    // Plain colour, motion at the input's size, depth from 0 near to 1 far, and the exposure
    // worked out by FSR itself: the permutations tools/ffx_permutations.py builds are exactly these.
    desc.flags = FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
    desc.maxRenderSize = { (uint32_t)render_w, (uint32_t)render_h };
    desc.maxUpscaleSize = { (uint32_t)out_w, (uint32_t)out_h };
    desc.fpMessage = message;
    size_t size = (sizeof(FfxFsr3UpscalerContext) + 255) & ~(size_t)255;
    fsr.context = (FfxFsr3UpscalerContext *)std::aligned_alloc(256, size);
    if (!fsr.context) return false;
    std::memset(fsr.context, 0, sizeof(FfxFsr3UpscalerContext));
    e = ffxFsr3UpscalerContextCreate(fsr.context, &desc);
    if (e != FFX_OK) {
        std::printf("[fsr3 ] the FSR 3 context was not created (%d)\n", (int)e);
        return false;
    }
    fsr.context_ok = true;

    FfxFsr3UpscalerSharedResourceDescriptions shared;
    std::memset(&shared, 0, sizeof shared);
    e = ffxFsr3UpscalerGetSharedResourceDescriptions(fsr.context, &shared);
    if (e == FFX_OK) {
        e = fsr.iface.fpCreateBackendContext(&fsr.iface, FFX_EFFECT_SHAREDRESOURCES, nullptr, &fsr.shared_id);
    }
    const FfxCreateResourceDescription *make[3] = { &shared.dilatedDepth, &shared.dilatedMotionVectors,
                                                    &shared.reconstructedPrevNearestDepth };
    for (int i = 0; i < 3 && e == FFX_OK; i++) {
        e = fsr.iface.fpCreateResource(&fsr.iface, make[i], fsr.shared_id, &fsr.shared[i]);
    }
    if (e != FFX_OK) {
        std::printf("[fsr3 ] FSR 3's shared surfaces were not created (%d)\n", (int)e);
        return false;
    }
    fsr.shared_ok = true;
    fsr.fresh = true;
    std::printf("[fsr3 ] upscaling %dx%d to %dx%d\n", render_w, render_h, out_w, out_h);
    return true;
}

FfxResource fsr3_resource(Image &img, FfxSurfaceFormat format, bool writable, const wchar_t *name) {
    FfxResourceDescription d;
    std::memset(&d, 0, sizeof d);
    d.type = FFX_RESOURCE_TYPE_TEXTURE2D;
    d.format = format;
    d.width = img.w;
    d.height = img.h;
    d.depth = 1;
    d.mipCount = 1;
    d.flags = FFX_RESOURCE_FLAGS_NONE;
    d.usage = writable ? FFX_RESOURCE_USAGE_UAV : FFX_RESOURCE_USAGE_READ_ONLY;
    return ffxGetResourceVK(img.image, d, name,
                            writable ? FFX_RESOURCE_STATE_UNORDERED_ACCESS : FFX_RESOURCE_STATE_COMPUTE_READ);
}
#endif

// ---------------------------------------------------------------------------------------------
// NVIDIA DLSS, through NGX.
#ifdef ALBW_HAVE_DLSS
struct Dlss {
    int tried = 0, ok = 0;
    const char *why = nullptr;
    NVSDK_NGX_Parameter *caps = nullptr, *params = nullptr;
    NVSDK_NGX_Handle *feature = nullptr;
    Surfaces s;
    bool fresh = false;
} dlss;

// A project id for NGX, which applications without one of NVIDIA's own application ids identify
// themselves by: any fixed GUID of the application's.
const char *const dlss_project = "4b6c1d2e-9f30-4a85-b7c2-5e1d0a3f6c94";

bool dlss_start() {
    if (!device_ready()) {
        dlss.why = "Needs a Vulkan 1.3 graphics card; see the log for what was missing.";
        return false;
    }
    if (dev.props.vendorID != 0x10DE) {
        dlss.why = "Needs an NVIDIA RTX graphics card.";
        return false;
    }
    // NGX keeps its logs here, and looks for the DLSS library beside the program and in the SDK.
    static wchar_t data[600], here[600], sdk[600];
    const char *cache = std::getenv("XDG_CACHE_HOME"), *home = std::getenv("HOME");
    char path[600];
    if (cache && *cache) std::snprintf(path, sizeof path, "%s/albw-port", cache);
    else std::snprintf(path, sizeof path, "%s/.cache/albw-port", home ? home : "/tmp");
    std::mbstowcs(data, path, 599);
    char exe[600];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[n > 0 ? n : 0] = '\0';
    char *slash = std::strrchr(exe, '/');
    if (slash) *slash = '\0';
    std::mbstowcs(here, exe, 599);
    std::mbstowcs(sdk, ALBW_DLSS_LIBRARY_DIR, 599);
    static const wchar_t *paths[2] = { here, sdk };
    NVSDK_NGX_FeatureCommonInfo info;
    std::memset(&info, 0, sizeof info);
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 2;
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        dlss_project, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "0.1", data, dev.instance, dev.phys, dev.device,
        vkGetInstanceProcAddr, vkGetDeviceProcAddr, &info);
    if (NVSDK_NGX_FAILED(r)) {
        std::printf("[dlss ] NGX did not start (0x%08x)\n", (unsigned)r);
        dlss.why = "NVIDIA's NGX did not start on this card; see the log.";
        return false;
    }
    r = NVSDK_NGX_VULKAN_GetCapabilityParameters(&dlss.caps);
    int available = 0;
    if (!NVSDK_NGX_FAILED(r)) {
        NVSDK_NGX_Parameter_GetI(dlss.caps, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    }
    if (!available) {
        int why = 0;
        if (dlss.caps) NVSDK_NGX_Parameter_GetI(dlss.caps, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &why);
        std::printf("[dlss ] DLSS is not available on this card or driver (0x%08x)\n", (unsigned)why);
        dlss.why = "DLSS is not available on this card or driver; see the log.";
        return false;
    }
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_AllocateParameters(&dlss.params))) {
        dlss.why = "NVIDIA's NGX had no parameters to give.";
        return false;
    }
    std::printf("[dlss ] DLSS is available on %s\n", dev.props.deviceName);
    return true;
}

void dlss_destroy() {
    if (dev.device) vkDeviceWaitIdle(dev.device);
    if (dlss.feature) NVSDK_NGX_VULKAN_ReleaseFeature(dlss.feature);
    dlss.feature = nullptr;
    free_surfaces(dlss.s);
}

bool dlss_make(int render_w, int render_h, int out_w, int out_h) {
    dlss_destroy();
    if (!make_surfaces(dlss.s, render_w, render_h, out_w, out_h)) return false;
    NVSDK_NGX_DLSS_Create_Params create;
    std::memset(&create, 0, sizeof create);
    create.Feature.InWidth = (unsigned)render_w;
    create.Feature.InHeight = (unsigned)render_h;
    create.Feature.InTargetWidth = (unsigned)out_w;
    create.Feature.InTargetHeight = (unsigned)out_h;
    // The quality mode is a hint here, the sizes being given; it is the one whose ratio this is.
    float ratio = (float)out_w / (float)render_w;
    create.Feature.InPerfQualityValue = ratio >= 2.9f ? NVSDK_NGX_PerfQuality_Value_UltraPerformance
                                      : ratio >= 1.9f ? NVSDK_NGX_PerfQuality_Value_MaxPerf
                                      : ratio >= 1.6f ? NVSDK_NGX_PerfQuality_Value_Balanced
                                                      : NVSDK_NGX_PerfQuality_Value_MaxQuality;
    // Plain colour, motion at the input's size and without the jitter, depth from 0 near.
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    begin();
    NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT1(dev.device, dev.cmd, 1, 1, &dlss.feature, dlss.params, &create);
    finish();
    if (NVSDK_NGX_FAILED(r)) {
        std::printf("[dlss ] the DLSS feature was not created (0x%08x)\n", (unsigned)r);
        return false;
    }
    dlss.fresh = true;
    std::printf("[dlss ] upscaling %dx%d to %dx%d\n", render_w, render_h, out_w, out_h);
    return true;
}

NVSDK_NGX_Resource_VK dlss_resource(Image &img, bool writable) {
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    return NVSDK_NGX_Create_ImageView_Resource_VK(img.view, img.image, range, img.format, img.w, img.h, writable);
}
#endif

}  // namespace

extern "C" int albw_fsr3_available(void) {
#ifdef ALBW_HAVE_FSR3
    return device_ready();
#else
    return 0;
#endif
}

extern "C" int albw_dlss_available(const char **why) {
#ifdef ALBW_HAVE_DLSS
    if (!dlss.tried) {
        dlss.tried = 1;
        dlss.ok = dlss_start();
    }
    if (why) *why = dlss.ok ? nullptr : dlss.why;
    return dlss.ok;
#else
    if (why) *why = "Not in this build: it needs NVIDIA's DLSS SDK in external/.";
    return 0;
#endif
}

extern "C" void albw_vkup_jitter(uint32_t index, int render_w, int display_w, float *x, float *y) {
    // Halton(2, 3), as both upscalers' samples use it, over enough phases for the ratio: eight
    // per pixel the output has for each of the input's.
    float ratio = (float)display_w / (float)render_w;
    int phases = (int)std::ceil(8.0f * ratio * ratio);
    if (phases < 8) phases = 8;
    uint32_t i = index % (uint32_t)phases + 1u;
    auto halton = [](uint32_t n, uint32_t base) {
        float f = 1.0f, r = 0.0f;
        while (n > 0) {
            f /= (float)base;
            r += f * (float)(n % base);
            n /= base;
        }
        return r;
    };
    *x = halton(i, 2) - 0.5f;
    *y = halton(i, 3) - 0.5f;
}

extern "C" int albw_fsr3_upscale(const struct albw_vkup_frame *in, uint32_t *out, int out_w,
                                 int out_h, int out_pitch) {
#ifdef ALBW_HAVE_FSR3
    if (!device_ready()) return 0;
    if (!fsr.context_ok || fsr.s.render_w != in->render_w || fsr.s.render_h != in->render_h
            || fsr.s.out_w != out_w || fsr.s.out_h != out_h) {
        if (!fsr3_make(in->render_w, in->render_h, out_w, out_h)) {
            fsr3_destroy();
            dev.ok = 0;                                  // said why; not again
            return 0;
        }
    }
    struct timespec tp[5];
    clock_gettime(CLOCK_MONOTONIC, &tp[0]);
    begin();
    record_inputs(fsr.s, in);
    clock_gettime(CLOCK_MONOTONIC, &tp[1]);
    FfxFsr3UpscalerDispatchDescription d;
    std::memset(&d, 0, sizeof d);
    d.commandList = ffxGetCommandListVK(dev.cmd);
    d.color = fsr3_resource(fsr.s.color, FFX_SURFACE_FORMAT_R8G8B8A8_UNORM, false, L"albw colour");
    d.depth = fsr3_resource(fsr.s.depth, FFX_SURFACE_FORMAT_R32_FLOAT, false, L"albw depth");
    d.motionVectors = fsr3_resource(fsr.s.motion, FFX_SURFACE_FORMAT_R32G32_FLOAT, false, L"albw motion");
    d.reactive = fsr3_resource(fsr.s.reactive, FFX_SURFACE_FORMAT_R8_UNORM, false, L"albw reactive");
    d.output = fsr3_resource(fsr.s.output, FFX_SURFACE_FORMAT_R8G8B8A8_UNORM, true, L"albw output");
    d.dilatedDepth = fsr.iface.fpGetResource(&fsr.iface, fsr.shared[0]);
    d.dilatedMotionVectors = fsr.iface.fpGetResource(&fsr.iface, fsr.shared[1]);
    d.reconstructedPrevNearestDepth = fsr.iface.fpGetResource(&fsr.iface, fsr.shared[2]);
    d.jitterOffset.x = in->jitter_x;
    d.jitterOffset.y = in->jitter_y;
    d.motionVectorScale.x = 1.0f;                        // they are in pixels already
    d.motionVectorScale.y = 1.0f;
    d.renderSize = { (uint32_t)in->render_w, (uint32_t)in->render_h };
    d.upscaleSize = { (uint32_t)out_w, (uint32_t)out_h };
    d.enableSharpening = in->sharpness >= 0.0f;
    d.sharpness = in->sharpness < 0.0f ? 0.0f : (in->sharpness > 1.0f ? 1.0f : in->sharpness);
    d.frameTimeDelta = in->frame_ms > 0.0f ? in->frame_ms : 16.7f;
    d.preExposure = 1.0f;
    d.reset = in->reset != 0 || fsr.fresh;
    d.cameraNear = in->camera_near;
    d.cameraFar = in->camera_far;
    d.cameraFovAngleVertical = in->fov_y;
    d.viewSpaceToMetersFactor = 1.0f;
    FfxErrorCode e = ffxFsr3UpscalerContextDispatch(fsr.context, &d);
    if (e != FFX_OK) {
        static int told;
        if (!told++) std::printf("[fsr3 ] dispatch failed (%d)\n", (int)e);
    }
    fsr.fresh = false;
    record_output(fsr.s);
    clock_gettime(CLOCK_MONOTONIC, &tp[2]);
    if (!finish()) return 0;
    clock_gettime(CLOCK_MONOTONIC, &tp[3]);
    copy_out(fsr.s, out, out_pitch, in->out_rgba != 0);
    clock_gettime(CLOCK_MONOTONIC, &tp[4]);
    if (std::getenv("ALBW_VKUP_TIMES")) {
        auto ms = [&](int a, int b) {
            return (double)(tp[b].tv_sec - tp[a].tv_sec) * 1e3 + (double)(tp[b].tv_nsec - tp[a].tv_nsec) * 1e-6;
        };
        std::printf("[fsr3 ] inputs %.2f ms, dispatch %.2f, the card %.2f, output %.2f\n", ms(0, 1),
                    ms(1, 2), ms(2, 3), ms(3, 4));
    }
    return e == FFX_OK;
#else
    (void)in; (void)out; (void)out_w; (void)out_h; (void)out_pitch;
    return 0;
#endif
}

extern "C" int albw_dlss_upscale(const struct albw_vkup_frame *in, uint32_t *out, int out_w,
                                 int out_h, int out_pitch) {
#ifdef ALBW_HAVE_DLSS
    if (!albw_dlss_available(nullptr)) return 0;
    if (!dlss.feature || dlss.s.render_w != in->render_w || dlss.s.render_h != in->render_h
            || dlss.s.out_w != out_w || dlss.s.out_h != out_h) {
        if (!dlss_make(in->render_w, in->render_h, out_w, out_h)) {
            dlss_destroy();
            dlss.ok = 0;
            dlss.why = "DLSS did not start at this size; see the log.";
            return 0;
        }
    }
    begin();
    record_inputs(dlss.s, in);
    NVSDK_NGX_Resource_VK color = dlss_resource(dlss.s.color, false);
    NVSDK_NGX_Resource_VK depth = dlss_resource(dlss.s.depth, false);
    NVSDK_NGX_Resource_VK motion = dlss_resource(dlss.s.motion, false);
    NVSDK_NGX_Resource_VK output = dlss_resource(dlss.s.output, true);
    NVSDK_NGX_VK_DLSS_Eval_Params ev;
    std::memset(&ev, 0, sizeof ev);
    ev.Feature.pInColor = &color;
    ev.Feature.pInOutput = &output;
    ev.pInDepth = &depth;
    ev.pInMotionVectors = &motion;
    // The same jitter FSR 3 is told of, in the same directions as the motion; ALBW_DLSS_JITTER=-1
    // turns it round, should NVIDIA's convention prove to be the other one.
    static float sign = 0.0f;
    if (sign == 0.0f) {
        const char *s = std::getenv("ALBW_DLSS_JITTER");
        sign = s && std::atoi(s) < 0 ? -1.0f : 1.0f;
    }
    ev.InJitterOffsetX = in->jitter_x * sign;
    ev.InJitterOffsetY = in->jitter_y * sign;
    ev.InRenderSubrectDimensions.Width = (unsigned)in->render_w;
    ev.InRenderSubrectDimensions.Height = (unsigned)in->render_h;
    ev.InReset = in->reset || dlss.fresh;
    ev.InMVScaleX = 1.0f;
    ev.InMVScaleY = 1.0f;
    ev.InFrameTimeDeltaInMsec = in->frame_ms > 0.0f ? in->frame_ms : 16.7f;
    NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT(dev.cmd, dlss.feature, dlss.params, &ev);
    if (NVSDK_NGX_FAILED(r)) {
        static int told;
        if (!told++) std::printf("[dlss ] evaluation failed (0x%08x)\n", (unsigned)r);
    }
    dlss.fresh = false;
    // NGX left the output as it found it; on to the copy.
    record_output(dlss.s);
    if (!finish()) return 0;
    copy_out(dlss.s, out, out_pitch, in->out_rgba != 0);
    return !NVSDK_NGX_FAILED(r);
#else
    (void)in; (void)out; (void)out_w; (void)out_h; (void)out_pitch;
    return 0;
#endif
}

extern "C" void albw_vkup_close(void) {
    if (!dev.device) return;
#ifdef ALBW_HAVE_FSR3
    fsr3_destroy();
#endif
#ifdef ALBW_HAVE_DLSS
    dlss_destroy();
    if (dlss.params) NVSDK_NGX_VULKAN_DestroyParameters(dlss.params);
    if (dlss.caps) NVSDK_NGX_VULKAN_DestroyParameters(dlss.caps);
    if (dlss.ok) NVSDK_NGX_VULKAN_Shutdown1(dev.device);
#endif
    vkDestroyFence(dev.device, dev.fence, nullptr);
    vkDestroyCommandPool(dev.device, dev.pool, nullptr);
    vkDestroyDevice(dev.device, nullptr);
    vkDestroyInstance(dev.instance, nullptr);
    dev = Device();
}

#endif
