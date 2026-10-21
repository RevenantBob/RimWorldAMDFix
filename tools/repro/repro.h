// Shared helpers for the repro tools: Vulkan setup, buffers, images, barriers, command submission,
// and BC encoding of solid-colour blocks.
//
// Each tool is a single .c file that includes this header, so the helpers are static functions.
// The tools are deliberately simple: one device, one queue, and every command buffer is submitted
// and waited on immediately.

#pragma once

#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Aborts the program with the failing call and its result if a Vulkan call does not succeed.
#define CHECK(call)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        VkResult result_ = (call);                                                                                     \
        if (result_ != VK_SUCCESS)                                                                                     \
        {                                                                                                              \
            printf("FAIL %s -> %d (line %d)\n", #call, result_, __LINE__);                                             \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

// ============================================================================================ global state

static VkInstance g_instance;
static VkPhysicalDevice g_physical;
static VkDevice g_device;
static VkQueue g_queue;
static VkCommandPool g_pool;
static uint32_t g_queueFamily;

// ============================================================================================ setup

// Creates the instance, picks the first GPU, and creates a device with one graphics queue.
// The application and engine names are what the Vulkan layer sees in VkApplicationInfo; engineName
// may be NULL.
static inline void initVulkan(const char *appName, const char *engineName)
{
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = appName;
    app.applicationVersion = 1;
    app.pEngineName = engineName;
    app.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo instanceInfo = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    CHECK(vkCreateInstance(&instanceInfo, NULL, &g_instance));

    // Use the first GPU. The tools are meant to be run on the machine being tested.
    uint32_t gpuCount = 1;
    VkResult enumResult = vkEnumeratePhysicalDevices(g_instance, &gpuCount, &g_physical);
    if (enumResult < 0 || gpuCount == 0)
    {
        printf("FAIL no Vulkan GPU found\n");
        exit(1);
    }

    // Print the GPU and driver, so results can be attached to bug reports as they are.
    VkPhysicalDeviceDriverProperties driver = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 properties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &driver;
    vkGetPhysicalDeviceProperties2(g_physical, &properties);

    printf("GPU: %s | driver: %s %s | Vulkan %u.%u.%u\n\n", properties.properties.deviceName, driver.driverName,
           driver.driverInfo, VK_VERSION_MAJOR(properties.properties.apiVersion),
           VK_VERSION_MINOR(properties.properties.apiVersion), VK_VERSION_PATCH(properties.properties.apiVersion));

    // Blits need a graphics-capable queue.
    VkQueueFamilyProperties families[16];
    uint32_t familyCount = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(g_physical, &familyCount, families);

    g_queueFamily = 0;
    while (g_queueFamily < familyCount && !(families[g_queueFamily].queueFlags & VK_QUEUE_GRAPHICS_BIT))
    {
        g_queueFamily++;
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = g_queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    VkDeviceCreateInfo deviceInfo = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    CHECK(vkCreateDevice(g_physical, &deviceInfo, NULL, &g_device));

    vkGetDeviceQueue(g_device, g_queueFamily, 0, &g_queue);

    VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = g_queueFamily;
    CHECK(vkCreateCommandPool(g_device, &poolInfo, NULL, &g_pool));
}

// Destroys everything initVulkan created.
static inline void shutdownVulkan(void)
{
    vkDestroyCommandPool(g_device, g_pool, NULL);
    vkDestroyDevice(g_device, NULL);
    vkDestroyInstance(g_instance, NULL);
}

// ============================================================================================ memory

// Returns the first memory type allowed by `bits` that has all `want` flags. Aborts if there is none.
static inline uint32_t findMemoryType(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(g_physical, &properties);

    for (uint32_t i = 0; i < properties.memoryTypeCount; i++)
    {
        if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & want) == want)
        {
            return i;
        }
    }

    printf("FAIL no suitable memory type\n");
    exit(1);
}

// A buffer in host-visible memory, mapped for its whole lifetime.
typedef struct
{
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t *data;
} Buffer;

// Creates a host-visible, coherent, mapped buffer for uploads and readbacks.
static inline Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage)
{
    Buffer b;

    VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CHECK(vkCreateBuffer(g_device, &info, NULL, &b.buffer));

    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(g_device, b.buffer, &requirements);

    VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(vkAllocateMemory(g_device, &allocInfo, NULL, &b.memory));

    CHECK(vkBindBufferMemory(g_device, b.buffer, b.memory, 0));
    CHECK(vkMapMemory(g_device, b.memory, 0, size, 0, (void **)&b.data));

    return b;
}

static inline void destroyBuffer(Buffer b)
{
    vkUnmapMemory(g_device, b.memory);
    vkDestroyBuffer(g_device, b.buffer, NULL);
    vkFreeMemory(g_device, b.memory, NULL);
}

// ============================================================================================ images

// A 2D, single-mip image in GPU memory.
typedef struct
{
    VkImage image;
    VkDeviceMemory memory;
} Image;

// Creates an image usable as a transfer source and destination.
static inline Image createImage(VkFormat format, uint32_t width, uint32_t height)
{
    Image img;

    VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = (VkExtent3D){width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    CHECK(vkCreateImage(g_device, &info, NULL, &img.image));

    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(g_device, img.image, &requirements);

    VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CHECK(vkAllocateMemory(g_device, &allocInfo, NULL, &img.memory));

    CHECK(vkBindImageMemory(g_device, img.image, img.memory, 0));

    return img;
}

static inline void destroyImage(Image img)
{
    vkDestroyImage(g_device, img.image, NULL);
    vkFreeMemory(g_device, img.memory, NULL);
}

// Records a layout transition with a full memory dependency. The tools favour simplicity over speed.
static inline void imageBarrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                         NULL, 1, &barrier);
}

// The subresource every tool uses: colour aspect, mip 0, layer 0.
static inline VkImageSubresourceLayers colorLayer(void)
{
    VkImageSubresourceLayers layer = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    return layer;
}

// Uploads `buffer` into all of `image` and leaves the image ready to be a transfer source.
static inline void recordUpload(VkCommandBuffer cb, Buffer buffer, Image image, uint32_t width, uint32_t height)
{
    VkBufferImageCopy copy = {0};
    copy.imageSubresource = colorLayer();
    copy.imageExtent = (VkExtent3D){width, height, 1};

    imageBarrier(cb, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cb, buffer.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    imageBarrier(cb, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
}

// Clears `image` to transparent black and leaves it ready to be a transfer destination, so the only
// opaque texels afterwards are the ones a blit wrote.
static inline void recordClear(VkCommandBuffer cb, Image image)
{
    VkClearColorValue clear = {0};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    imageBarrier(cb, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdClearColorImage(cb, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);

    // Same layout on both sides: this only orders the clear before the blit that follows.
    imageBarrier(cb, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
}

// Copies all of `image` (currently a transfer destination) into `buffer` for reading on the CPU.
static inline void recordReadback(VkCommandBuffer cb, Image image, Buffer buffer, uint32_t width, uint32_t height)
{
    VkBufferImageCopy copy = {0};
    copy.imageSubresource = colorLayer();
    copy.imageExtent = (VkExtent3D){width, height, 1};

    imageBarrier(cb, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkCmdCopyImageToBuffer(cb, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.buffer, 1, &copy);
}

// ============================================================================================ commands

// Allocates a one-time command buffer and starts recording.
static inline VkCommandBuffer beginCommands(void)
{
    VkCommandBufferAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = g_pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(g_device, &allocInfo, &cb));

    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHECK(vkBeginCommandBuffer(cb, &beginInfo));

    return cb;
}

// Ends recording, submits, waits for the GPU to finish, and frees the command buffer.
static inline void submitAndWait(VkCommandBuffer cb)
{
    CHECK(vkEndCommandBuffer(cb));

    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    CHECK(vkQueueSubmit(g_queue, 1, &submit, VK_NULL_HANDLE));

    CHECK(vkQueueWaitIdle(g_queue));

    vkFreeCommandBuffers(g_device, g_pool, 1, &cb);
}

// ============================================================================================ BC encoding

// Encodes a BC7 block whose 16 texels all have one colour, using mode 6. Mode 6 stores each channel
// of both endpoints with 7 bits plus a shared "p-bit"; with every p-bit set to 1 a channel decodes as
// (value & ~1) | 1, so the lowest bit of each channel is lost.
//
// Bit layout, from the lowest bit: 7 mode bits (0000001), then R0 R1 G0 G1 B0 B1 A0 A1 at 7 bits
// each, then the two p-bits, then the 63 index bits (all zero, so every texel uses endpoint 0).
static inline void encodeBc7Solid(uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t out[16])
{
    uint64_t low = 0;
    uint64_t high = 0;
    int position = 0;

    // Appends `count` bits of `value` to the 128-bit block, which is stored as two 64-bit halves.
#define PUT_BITS(value, count)                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        uint64_t bits_ = (uint64_t)(value) & ((1ull << (count)) - 1);                                                  \
        if (position < 64)                                                                                             \
        {                                                                                                              \
            low |= bits_ << position;                                                                                  \
            if (position + (count) > 64)                                                                               \
            {                                                                                                          \
                high |= bits_ >> (64 - position);                                                                      \
            }                                                                                                          \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            high |= bits_ << (position - 64);                                                                          \
        }                                                                                                              \
        position += (count);                                                                                           \
    } while (0)

    PUT_BITS(1u << 6, 7);

    uint8_t channels[4] = {r, g, b, a};
    for (int i = 0; i < 4; i++)
    {
        PUT_BITS(channels[i] >> 1, 7);
        PUT_BITS(channels[i] >> 1, 7);
    }

    PUT_BITS(1, 1);
    PUT_BITS(1, 1);

#undef PUT_BITS

    // The remaining 63 index bits stay zero.
    memcpy(out, &low, 8);
    memcpy(out + 8, &high, 8);
}

// Encodes a BC1 block of solid opaque red: colour 0 is 0xF800 (red in RGB565), colour 1 is 0, and all
// indices are 0. Because colour 0 > colour 1 the block is in its opaque four-colour mode.
static inline void encodeBc1SolidRed(uint8_t out[8])
{
    const uint8_t block[8] = {0x00, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    memcpy(out, block, 8);
}
