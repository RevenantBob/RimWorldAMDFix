// RimWorldAMDFix: a Vulkan layer that works around an AMD Windows driver bug hit by RimWorld 1.7.
//
// The bug
//   A 1:1 vkCmdBlitImage from a block-compressed (BC1-BC7) image divides the source offset and extent
//   by the 4x4 block size, so only the top-left (w/4 x h/4) of the region is copied. RimWorld 1.7
//   (Waylith engine) builds its item texture atlases with exactly that call, so items render as corners.
//
// The fix
//   Scaled blits from BC images are correct on the affected driver. Each affected 1:1 blit is therefore
//   replaced by two blits through a scratch image in the destination's format:
//     1. a 2x nearest upscale of the source region into the scratch image, then
//     2. a 2:1 nearest downscale of the scratch region into the original destination.
//   With nearest filtering both passes are exact, so the result equals a correct 1:1 blit. Large
//   regions are processed in tiles so the scratch image stays a fixed size.
//
// Scope
//   The layer only acts in Waylith/RimWorld instances, and only when a self-test at device creation
//   shows that the driver actually has the bug. Everything else passes straight through to the driver.

#define _CRT_SECURE_NO_WARNINGS

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// build.ps1 defines the real version through a generated header.
#ifndef RWAMDFIX_VERSION
#define RWAMDFIX_VERSION "dev"
#endif

#define EXPORT __declspec(dllexport)

// Size of the scratch image used for the two-pass blit. Each tile of the source covers half of it,
// because pass 1 doubles the tile in both directions.
#define SCRATCH_DIM 4096u
#define TILE_DIM (SCRATCH_DIM / 2)

// Only the first few fixed blits are logged individually; the rest are counted.
#define MAX_LOGGED_REWRITES 10

// The log file is started over when it grows past this size.
#define MAX_LOG_BYTES (1024 * 1024)

// ============================================================================================ logging

// The log lives at %LOCALAPPDATA%\RimWorldAMDFix\RimWorldAMDFix.log. It is opened on first use, which
// only happens in programs the layer acts in, so other Vulkan programs never create or touch it.
static SRWLOCK g_logLock = SRWLOCK_INIT;
static FILE *g_log;

// Appends one timestamped line to the log (printf-style).
static void logf_(const char *fmt, ...)
{
    AcquireSRWLockExclusive(&g_logLock);

    if (!g_log)
    {
        char path[MAX_PATH];
        DWORD length = GetEnvironmentVariableA("LOCALAPPDATA", path, MAX_PATH);

        // Leave room for the folder and file names appended below.
        if (length && length < MAX_PATH - 40)
        {
            strcat(path, "\\RimWorldAMDFix");
            CreateDirectoryA(path, NULL);
            strcat(path, "\\RimWorldAMDFix.log");

            // Start a fresh log instead of appending once it has grown too large.
            WIN32_FILE_ATTRIBUTE_DATA attributes;
            int tooBig = GetFileAttributesExA(path, GetFileExInfoStandard, &attributes) &&
                         (attributes.nFileSizeHigh || attributes.nFileSizeLow > MAX_LOG_BYTES);

            g_log = fopen(path, tooBig ? "w" : "a");
        }
    }

    if (g_log)
    {
        SYSTEMTIME now;
        GetLocalTime(&now);
        fprintf(g_log, "%04d-%02d-%02d %02d:%02d:%02d.%03d [pid %lu] ", now.wYear, now.wMonth, now.wDay, now.wHour,
                now.wMinute, now.wSecond, now.wMilliseconds, GetCurrentProcessId());

        va_list args;
        va_start(args, fmt);
        vfprintf(g_log, fmt, args);
        va_end(args);

        fputc('\n', g_log);
        fflush(g_log);
    }

    ReleaseSRWLockExclusive(&g_logLock);
}

// ============================================================================================ settings

// Returns 1 if the environment variable is set to "1".
static int envIs1(const char *name)
{
    char value[8] = {0};
    return GetEnvironmentVariableA(name, value, sizeof value) && value[0] == '1';
}

// Decides from VkApplicationInfo whether the layer should act in this Vulkan instance.
// RimWorld 1.7 reports application "RimWorld by Ludeon Studios" and engine "Waylith".
// RIMWORLDAMDFIX_ANY_APP=1 makes the layer act in any program (used by the repro tools).
static int appWantsFix(const VkApplicationInfo *app)
{
    if (envIs1("RIMWORLDAMDFIX_ANY_APP"))
    {
        return 1;
    }

    if (!app)
    {
        return 0;
    }

    if (app->pEngineName && _stricmp(app->pEngineName, "Waylith") == 0)
    {
        return 1;
    }

    return app->pApplicationName && _strnicmp(app->pApplicationName, "RimWorld", 8) == 0;
}

// ============================================================================================ dispatch bookkeeping

// Every dispatchable Vulkan handle (instance, physical device, device, queue, command buffer) starts
// with a pointer to the loader's dispatch table. Handles that belong to the same instance or device
// share that pointer, so it identifies which of our records a handle belongs to.
typedef void *DispatchKey;

static DispatchKey keyOf(const void *handle)
{
    return *(DispatchKey *)handle;
}

// What the layer keeps per VkInstance.
typedef struct
{
    DispatchKey key;
    VkInstance instance;

    // Set when this instance belongs to RimWorld (see appWantsFix).
    int active;

    // The next layer's (or the driver's) entry points.
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
} InstanceData;

// A scratch image for the two-pass blit. There is one per destination format, created on first use.
typedef struct
{
    VkFormat format;
    VkImage image;
    VkDeviceMemory memory;

    // Set if the image could not be created; blits into this format are then left unchanged.
    int failed;
} Scratch;

// What the layer keeps per VkDevice.
typedef struct
{
    DispatchKey key;
    VkDevice device;
    VkPhysicalDevice physical;
    InstanceData *inst;

    // Set when affected blits on this device are rewritten (decided once, in decideFix).
    int fix;

    // The next layer's (or the driver's) entry points used by the fix.
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkDestroyDevice DestroyDevice;
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkCmdBlitImage CmdBlitImage;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;

    // Entry points used only by the self-test.
    PFN_vkGetDeviceQueue GetDeviceQueue;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkQueueWaitIdle QueueWaitIdle;
    PFN_vkCreateBuffer CreateBuffer;
    PFN_vkDestroyBuffer DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkBindBufferMemory BindBufferMemory;
    PFN_vkMapMemory MapMemory;
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
    PFN_vkCmdClearColorImage CmdClearColorImage;

    // Scratch images, one per destination format.
    SRWLOCK scratchLock;
    Scratch scratch[8];
    int scratchCount;

    // Number of blits fixed so far, for the log.
    volatile LONG64 rewrites;
} DeviceData;

#define MAX_INSTANCES 16
#define MAX_DEVICES 16
#define MAX_PHYS_KEYS 64

// All live instance and device records. A program creates very few, so plain arrays are enough.
static SRWLOCK g_tableLock = SRWLOCK_INIT;
static InstanceData *g_instances[MAX_INSTANCES];
static DeviceData *g_devices[MAX_DEVICES];

// Physical devices can dispatch through a different table than their instance, so their keys are
// recorded separately and mapped back to the owning instance.
static struct
{
    DispatchKey key;
    InstanceData *inst;
} g_physKeys[MAX_PHYS_KEYS];
static int g_physKeyCount;

// Finds the instance record for an instance or physical-device dispatch key.
static InstanceData *findInstance(DispatchKey key)
{
    InstanceData *found = NULL;

    AcquireSRWLockShared(&g_tableLock);

    for (int i = 0; i < MAX_INSTANCES && !found; i++)
    {
        if (g_instances[i] && g_instances[i]->key == key)
        {
            found = g_instances[i];
        }
    }

    for (int i = 0; i < g_physKeyCount && !found; i++)
    {
        if (g_physKeys[i].key == key)
        {
            found = g_physKeys[i].inst;
        }
    }

    ReleaseSRWLockShared(&g_tableLock);
    return found;
}

// Finds the device record for a device, queue or command-buffer dispatch key.
static DeviceData *findDevice(DispatchKey key)
{
    DeviceData *found = NULL;

    AcquireSRWLockShared(&g_tableLock);

    for (int i = 0; i < MAX_DEVICES && !found; i++)
    {
        if (g_devices[i] && g_devices[i]->key == key)
        {
            found = g_devices[i];
        }
    }

    ReleaseSRWLockShared(&g_tableLock);
    return found;
}

// ============================================================================================ image format tracking

// vkCmdBlitImage only receives image handles, but the fix needs to know each image's format. The
// layer records the format of every image created while the fix is on, in an open-addressing hash
// table. An empty slot has image 0; a removed entry is marked with TOMBSTONE so lookups keep probing.
typedef struct
{
    VkImage image;
    VkFormat format;
} ImageEntry;

static SRWLOCK g_imgLock = SRWLOCK_INIT;
static ImageEntry *g_imgs;
static size_t g_imgCapacity;
static size_t g_imgCount;

#define TOMBSTONE ((VkImage)(uintptr_t)1)

// Hashes an image handle to a slot index. The capacity is always a power of two.
static size_t imgHash(VkImage img, size_t capacity)
{
    uint64_t h = (uint64_t)(uintptr_t)img;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return (size_t)(h & (capacity - 1));
}

static void imgInsertLocked(VkImage img, VkFormat format);

// Doubles the table and re-inserts the live entries (dropping tombstones). Caller holds g_imgLock.
static void imgGrowLocked(void)
{
    size_t oldCapacity = g_imgCapacity;
    ImageEntry *old = g_imgs;

    g_imgCapacity = oldCapacity ? oldCapacity * 2 : 4096;
    g_imgs = calloc(g_imgCapacity, sizeof *g_imgs);
    g_imgCount = 0;

    for (size_t i = 0; i < oldCapacity; i++)
    {
        if (old[i].image && old[i].image != TOMBSTONE)
        {
            imgInsertLocked(old[i].image, old[i].format);
        }
    }

    free(old);
}

// Records (or updates) the format of an image. Caller holds g_imgLock.
static void imgInsertLocked(VkImage img, VkFormat format)
{
    // Keep the table at most half full so probe sequences stay short.
    if ((g_imgCount + 1) * 2 > g_imgCapacity)
    {
        imgGrowLocked();
    }

    size_t i = imgHash(img, g_imgCapacity);
    while (g_imgs[i].image && g_imgs[i].image != TOMBSTONE && g_imgs[i].image != img)
    {
        i = (i + 1) & (g_imgCapacity - 1);
    }

    if (g_imgs[i].image != img)
    {
        g_imgCount++;
    }

    g_imgs[i].image = img;
    g_imgs[i].format = format;
}

// Forgets an image when it is destroyed.
static void imgRemove(VkImage img)
{
    AcquireSRWLockExclusive(&g_imgLock);

    if (g_imgCapacity)
    {
        size_t i = imgHash(img, g_imgCapacity);
        while (g_imgs[i].image)
        {
            if (g_imgs[i].image == img)
            {
                g_imgs[i].image = TOMBSTONE;
                break;
            }

            i = (i + 1) & (g_imgCapacity - 1);
        }
    }

    ReleaseSRWLockExclusive(&g_imgLock);
}

// Returns the recorded format of an image, or VK_FORMAT_UNDEFINED if it is not known.
static VkFormat imgFormat(VkImage img)
{
    VkFormat format = VK_FORMAT_UNDEFINED;

    AcquireSRWLockShared(&g_imgLock);

    if (g_imgCapacity)
    {
        size_t i = imgHash(img, g_imgCapacity);
        while (g_imgs[i].image)
        {
            if (g_imgs[i].image == img)
            {
                format = g_imgs[i].format;
                break;
            }

            i = (i + 1) & (g_imgCapacity - 1);
        }
    }

    ReleaseSRWLockShared(&g_imgLock);
    return format;
}

// True for the block-compressed formats the driver bug affects (BC1 through BC7).
static int isBc(VkFormat format)
{
    return format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_BC7_SRGB_BLOCK;
}

// ============================================================================================ vulkan helpers

// Returns the first memory type allowed by `bits` that has all `want` property flags, or UINT32_MAX.
static uint32_t pickMemoryType(DeviceData *d, uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties properties;
    d->inst->GetPhysicalDeviceMemoryProperties(d->physical, &properties);

    for (uint32_t i = 0; i < properties.memoryTypeCount; i++)
    {
        if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & want) == want)
        {
            return i;
        }
    }

    return UINT32_MAX;
}

// Creates a 2D, single-mip image usable as a transfer source and destination, with its own memory.
// These calls go straight to the next layer, so the layer's own images are never tracked or fixed.
static VkResult createImage(DeviceData *d, VkFormat format, uint32_t width, uint32_t height, VkImage *img,
                            VkDeviceMemory *mem)
{
    *img = VK_NULL_HANDLE;
    *mem = VK_NULL_HANDLE;

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

    VkResult result = d->CreateImage(d->device, &info, NULL, img);
    if (result != VK_SUCCESS)
    {
        return result;
    }

    // Prefer GPU-local memory, but take any compatible type if there is none.
    VkMemoryRequirements requirements;
    d->GetImageMemoryRequirements(d->device, *img, &requirements);

    uint32_t type = pickMemoryType(d, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX)
    {
        type = pickMemoryType(d, requirements.memoryTypeBits, 0);
    }

    if (type == UINT32_MAX)
    {
        result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    else
    {
        VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, requirements.size, type};
        result = d->AllocateMemory(d->device, &allocInfo, NULL, mem);
    }

    if (result == VK_SUCCESS)
    {
        result = d->BindImageMemory(d->device, *img, *mem, 0);
    }

    // Clean up on any failure so the caller only has to check the result.
    if (result != VK_SUCCESS)
    {
        d->DestroyImage(d->device, *img, NULL);
        *img = VK_NULL_HANDLE;

        if (*mem)
        {
            d->FreeMemory(d->device, *mem, NULL);
            *mem = VK_NULL_HANDLE;
        }
    }

    return result;
}

// Records a layout transition for one image between transfer commands.
static void transferBarrier(DeviceData *d, VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                            VkAccessFlags srcAccess, VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = img;
    barrier.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                          &barrier);
}

// ============================================================================================ driver self-test

// One BC7 block in mode 6 with both endpoints (255, 0, 0, 255) and all indices 0: solid opaque red.
static const uint8_t RED_BC7_BLOCK[16] = {0xc0, 0xff, 0x1f, 0x00, 0x00, 0x00, 0xfe, 0xff, 0x01, 0, 0, 0, 0, 0, 0, 0};

// Checks whether this driver has the bug: blits an 8x8 opaque BC7 image 1:1 into a cleared 8x8 RGBA8
// image and counts the opaque texels that arrive. Returns 64 on a correct driver, 4 (2x2) on the
// affected one, or -1 if the test could not run.
//
// Runs inside vkCreateDevice, before the application has the device, so it can use the queue freely.
// Command buffers and queues the layer obtains itself need the loader's dispatch pointer set through
// setLoaderData before use.
static int selfTest(DeviceData *d, uint32_t queueFamily, PFN_vkSetDeviceLoaderData setLoaderData)
{
    VkDevice device = d->device;
    int result = -1;

    VkImage src = VK_NULL_HANDLE;
    VkImage dst = VK_NULL_HANDLE;
    VkDeviceMemory srcMemory = VK_NULL_HANDLE;
    VkDeviceMemory dstMemory = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;

    // One host-visible buffer holds the BC7 upload at UPLOAD_OFFSET and the RGBA8 readback at
    // READBACK_OFFSET (64 texels x 4 bytes).
    const VkDeviceSize UPLOAD_OFFSET = 0;
    const VkDeviceSize READBACK_OFFSET = 256;
    const VkDeviceSize BUFFER_SIZE = 512;

    // A driver that cannot blit from BC7 at all cannot have this bug either; let the caller decide.
    VkFormatProperties formatProperties;
    d->inst->GetPhysicalDeviceFormatProperties(d->physical, VK_FORMAT_BC7_UNORM_BLOCK, &formatProperties);
    if (!(formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT))
    {
        return -1;
    }

    // Images and the staging buffer. Any failure jumps to `cleanup`, which releases whatever exists.
    if (createImage(d, VK_FORMAT_BC7_UNORM_BLOCK, 8, 8, &src, &srcMemory) != VK_SUCCESS)
    {
        goto cleanup;
    }

    if (createImage(d, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, &dst, &dstMemory) != VK_SUCCESS)
    {
        goto cleanup;
    }

    // The staging buffer, used both to upload the BC7 data and to read the result back.
    VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = BUFFER_SIZE;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (d->CreateBuffer(device, &bufferInfo, NULL, &buffer) != VK_SUCCESS)
    {
        goto cleanup;
    }

    // Host-visible and coherent, so the CPU can write and read it without flushes.
    VkMemoryRequirements requirements;
    d->GetBufferMemoryRequirements(device, buffer, &requirements);

    uint32_t type = pickMemoryType(d, requirements.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX)
    {
        goto cleanup;
    }

    VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, requirements.size, type};
    if (d->AllocateMemory(device, &allocInfo, NULL, &bufferMemory) != VK_SUCCESS)
    {
        goto cleanup;
    }

    if (d->BindBufferMemory(device, buffer, bufferMemory, 0) != VK_SUCCESS)
    {
        goto cleanup;
    }

    // An 8x8 BC7 image is 2x2 blocks; fill all four with the solid red block.
    uint8_t *mapped = NULL;
    if (d->MapMemory(device, bufferMemory, 0, BUFFER_SIZE, 0, (void **)&mapped) != VK_SUCCESS)
    {
        goto cleanup;
    }

    memset(mapped, 0, (size_t)BUFFER_SIZE);
    for (int i = 0; i < 4; i++)
    {
        memcpy(mapped + UPLOAD_OFFSET + i * 16, RED_BC7_BLOCK, 16);
    }

    // A command pool and buffer of our own, on the queue family the app asked for.
    VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = queueFamily;

    if (d->CreateCommandPool(device, &poolInfo, NULL, &pool) != VK_SUCCESS)
    {
        goto cleanup;
    }

    VkCommandBufferAllocateInfo cbInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbInfo.commandPool = pool;
    cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbInfo.commandBufferCount = 1;

    VkCommandBuffer cb;
    if (d->AllocateCommandBuffers(device, &cbInfo, &cb) != VK_SUCCESS)
    {
        goto cleanup;
    }

    if (setLoaderData(device, cb) != VK_SUCCESS)
    {
        goto cleanup;
    }

    // Queue 0 of that family; the application has not received the device yet, so nothing else uses it.
    VkQueue queue;
    d->GetDeviceQueue(device, queueFamily, 0, &queue);

    if (setLoaderData(device, queue) != VK_SUCCESS)
    {
        goto cleanup;
    }

    // Record the test.
    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (d->BeginCommandBuffer(cb, &beginInfo) != VK_SUCCESS)
    {
        goto cleanup;
    }

    // Upload the BC7 blocks into the source image.
    VkBufferImageCopy copy = {0};
    copy.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = (VkExtent3D){8, 8, 1};
    copy.bufferOffset = UPLOAD_OFFSET;

    transferBarrier(d, cb, src, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    d->CmdCopyBufferToImage(cb, buffer, src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    transferBarrier(d, cb, src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);

    // Clear the destination to transparent black, so only texels the blit writes are opaque.
    VkClearColorValue clear = {0};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    transferBarrier(d, cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    d->CmdClearColorImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    transferBarrier(d, cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

    // The call under test, made directly on the next layer so the fix itself is not involved.
    VkImageBlit blit = {0};
    blit.srcSubresource = copy.imageSubresource;
    blit.dstSubresource = copy.imageSubresource;
    blit.srcOffsets[1] = (VkOffset3D){8, 8, 1};
    blit.dstOffsets[1] = (VkOffset3D){8, 8, 1};

    d->CmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                    VK_FILTER_NEAREST);

    // Copy the destination into the readback half of the buffer.
    copy.bufferOffset = READBACK_OFFSET;

    transferBarrier(d, cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    d->CmdCopyImageToBuffer(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);

    // Make the copied texels visible to the CPU once the queue is idle.
    VkBufferMemoryBarrier hostBarrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    hostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    hostBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostBarrier.buffer = buffer;
    hostBarrier.size = VK_WHOLE_SIZE;

    d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &hostBarrier,
                          0, NULL);

    if (d->EndCommandBuffer(cb) != VK_SUCCESS)
    {
        goto cleanup;
    }

    // Run it and wait for the result.
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;

    if (d->QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS)
    {
        goto cleanup;
    }

    if (d->QueueWaitIdle(queue) != VK_SUCCESS)
    {
        goto cleanup;
    }

    // Count the texels whose alpha is non-zero.
    result = 0;
    for (int i = 0; i < 64; i++)
    {
        if (mapped[READBACK_OFFSET + i * 4 + 3])
        {
            result++;
        }
    }

cleanup:
    // Destroying the pool also frees its command buffer; destroying memory also unmaps it.
    if (pool)
    {
        d->DestroyCommandPool(device, pool, NULL);
    }

    if (buffer)
    {
        d->DestroyBuffer(device, buffer, NULL);
    }

    if (bufferMemory)
    {
        d->FreeMemory(device, bufferMemory, NULL);
    }

    if (src)
    {
        d->DestroyImage(device, src, NULL);
    }

    if (srcMemory)
    {
        d->FreeMemory(device, srcMemory, NULL);
    }

    if (dst)
    {
        d->DestroyImage(device, dst, NULL);
    }

    if (dstMemory)
    {
        d->FreeMemory(device, dstMemory, NULL);
    }

    return result;
}

// ============================================================================================ scratch images

// Returns the scratch image for a destination format, creating it on first use. Returns NULL if
// this format cannot be used for the two-pass blit; such blits are then left unchanged.
static Scratch *getScratch(DeviceData *d, VkFormat format)
{
    AcquireSRWLockExclusive(&d->scratchLock);

    Scratch *scratch = NULL;
    for (int i = 0; i < d->scratchCount; i++)
    {
        if (d->scratch[i].format == format)
        {
            scratch = &d->scratch[i];
            break;
        }
    }

    int slotsLeft = d->scratchCount < (int)(sizeof d->scratch / sizeof d->scratch[0]);
    if (!scratch && slotsLeft)
    {
        scratch = &d->scratch[d->scratchCount++];
        memset(scratch, 0, sizeof *scratch);
        scratch->format = format;

        // The scratch image is the destination of pass 1 and the source of pass 2.
        VkFormatProperties properties;
        d->inst->GetPhysicalDeviceFormatProperties(d->physical, format, &properties);
        VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;

        if ((properties.optimalTilingFeatures & needed) != needed)
        {
            scratch->failed = 1;
            logf_("format %d cannot be a blit source and destination; blits into it are left as they are", format);
        }
        else
        {
            VkResult result = createImage(d, format, SCRATCH_DIM, SCRATCH_DIM, &scratch->image, &scratch->memory);
            if (result != VK_SUCCESS)
            {
                scratch->failed = 1;
                logf_("creating the %ux%u scratch image for format %d failed (%d); blits into it are left as they are",
                      SCRATCH_DIM, SCRATCH_DIM, format, result);
            }
        }
    }

    ReleaseSRWLockExclusive(&d->scratchLock);

    if (scratch && !scratch->failed)
    {
        return scratch;
    }

    return NULL;
}

// ============================================================================================ the fix

// True for the blit shape the affected driver gets wrong: unmirrored, the same size in x and y on
// both sides, one array layer, and a 2D (depth 1) region.
static int isOneToOne(const VkImageBlit *region)
{
    int32_t srcWidth = region->srcOffsets[1].x - region->srcOffsets[0].x;
    int32_t srcHeight = region->srcOffsets[1].y - region->srcOffsets[0].y;
    int32_t srcDepth = region->srcOffsets[1].z - region->srcOffsets[0].z;
    int32_t dstWidth = region->dstOffsets[1].x - region->dstOffsets[0].x;
    int32_t dstHeight = region->dstOffsets[1].y - region->dstOffsets[0].y;
    int32_t dstDepth = region->dstOffsets[1].z - region->dstOffsets[0].z;

    return srcWidth > 0 && srcHeight > 0 && srcWidth == dstWidth && srcHeight == dstHeight && srcDepth == 1 &&
           dstDepth == 1 && region->srcSubresource.layerCount == 1 && region->dstSubresource.layerCount == 1;
}

// Replaces one affected 1:1 blit region with the two-pass blit through the scratch image, tile by tile.
static void blitTwoPass(DeviceData *d, VkCommandBuffer cb, VkImage srcImage, VkImageLayout srcLayout, VkImage dstImage,
                        VkImageLayout dstLayout, const VkImageBlit *region, Scratch *scratch)
{
    int32_t width = region->srcOffsets[1].x - region->srcOffsets[0].x;
    int32_t height = region->srcOffsets[1].y - region->srcOffsets[0].y;

    for (int32_t tileY = 0; tileY < height; tileY += TILE_DIM)
    {
        for (int32_t tileX = 0; tileX < width; tileX += TILE_DIM)
        {
            // The size of this tile (the last tile in a row or column may be smaller).
            int32_t tileWidth = min((int32_t)TILE_DIM, width - tileX);
            int32_t tileHeight = min((int32_t)TILE_DIM, height - tileY);

            // Where this tile starts in the source and in the destination.
            int32_t srcX = region->srcOffsets[0].x + tileX;
            int32_t srcY = region->srcOffsets[0].y + tileY;
            int32_t dstX = region->dstOffsets[0].x + tileX;
            int32_t dstY = region->dstOffsets[0].y + tileY;

            // The scratch contents are disposable, so it can always start from UNDEFINED. The barrier
            // still waits for earlier transfers, which covers the previous tile reading the scratch image.
            transferBarrier(d, cb, scratch->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                            VK_ACCESS_TRANSFER_WRITE_BIT);

            // Pass 1: upscale the source tile 2x into the top-left of the scratch image. With nearest
            // filtering every source texel becomes an exact 2x2 group.
            VkImageBlit up = *region;
            up.srcOffsets[0] = (VkOffset3D){srcX, srcY, region->srcOffsets[0].z};
            up.srcOffsets[1] = (VkOffset3D){srcX + tileWidth, srcY + tileHeight, region->srcOffsets[1].z};
            up.dstSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            up.dstOffsets[0] = (VkOffset3D){0, 0, 0};
            up.dstOffsets[1] = (VkOffset3D){2 * tileWidth, 2 * tileHeight, 1};

            d->CmdBlitImage(cb, srcImage, srcLayout, scratch->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up,
                            VK_FILTER_NEAREST);

            // Make pass 1's writes visible to pass 2's reads.
            transferBarrier(d, cb, scratch->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_TRANSFER_READ_BIT);

            // Pass 2: downscale 2:1 into the real destination. Nearest picks one texel of each 2x2
            // group, which is the original source texel.
            VkImageBlit down = *region;
            down.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            down.srcOffsets[0] = (VkOffset3D){0, 0, 0};
            down.srcOffsets[1] = (VkOffset3D){2 * tileWidth, 2 * tileHeight, 1};
            down.dstOffsets[0] = (VkOffset3D){dstX, dstY, region->dstOffsets[0].z};
            down.dstOffsets[1] = (VkOffset3D){dstX + tileWidth, dstY + tileHeight, region->dstOffsets[1].z};

            d->CmdBlitImage(cb, scratch->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage, dstLayout, 1, &down,
                            VK_FILTER_NEAREST);
        }
    }
}

// vkCmdBlitImage hook. Fixes 1:1 regions from BC sources; passes everything else through unchanged.
static VKAPI_ATTR void VKAPI_CALL layer_CmdBlitImage(VkCommandBuffer cb, VkImage srcImage, VkImageLayout srcLayout,
                                                     VkImage dstImage, VkImageLayout dstLayout, uint32_t regionCount,
                                                     const VkImageBlit *regions, VkFilter filter)
{
    DeviceData *d = findDevice(keyOf(cb));
    if (!d)
    {
        return;
    }

    // Fast path: the fix is off for this device, or the source is not block-compressed.
    VkFormat srcFormat = VK_FORMAT_UNDEFINED;
    if (d->fix)
    {
        srcFormat = imgFormat(srcImage);
    }

    if (!isBc(srcFormat))
    {
        d->CmdBlitImage(cb, srcImage, srcLayout, dstImage, dstLayout, regionCount, regions, filter);
        return;
    }

    VkFormat dstFormat = imgFormat(dstImage);
    Scratch *scratch = NULL;
    if (dstFormat != VK_FORMAT_UNDEFINED)
    {
        scratch = getScratch(d, dstFormat);
    }

    // Handle each region on its own: only the 1:1 ones need fixing.
    for (uint32_t i = 0; i < regionCount; i++)
    {
        const VkImageBlit *region = &regions[i];

        if (!scratch || !isOneToOne(region))
        {
            d->CmdBlitImage(cb, srcImage, srcLayout, dstImage, dstLayout, 1, region, filter);
            continue;
        }

        LONG64 count = InterlockedIncrement64(&d->rewrites);
        if (count <= MAX_LOGGED_REWRITES)
        {
            int32_t width = region->srcOffsets[1].x - region->srcOffsets[0].x;
            int32_t height = region->srcOffsets[1].y - region->srcOffsets[0].y;
            logf_("fixed blit #%lld: %dx%d from format %d into format %d%s", count, width, height, srcFormat, dstFormat,
                  count == MAX_LOGGED_REWRITES ? " (further blits are counted, not logged)" : "");
        }

        blitTwoPass(d, cb, srcImage, srcLayout, dstImage, dstLayout, region, scratch);
    }
}

// ============================================================================================ image tracking hooks

// vkCreateImage hook: records the new image's format, which the blit hook needs.
static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateImage(VkDevice device, const VkImageCreateInfo *info,
                                                        const VkAllocationCallbacks *alloc, VkImage *img)
{
    DeviceData *d = findDevice(keyOf(device));

    VkResult result = d->CreateImage(device, info, alloc, img);
    if (result == VK_SUCCESS && d->fix)
    {
        AcquireSRWLockExclusive(&g_imgLock);

        imgInsertLocked(*img, info->format);

        ReleaseSRWLockExclusive(&g_imgLock);
    }

    return result;
}

// vkDestroyImage hook: forgets the image, since Vulkan may reuse the handle value later.
static VKAPI_ATTR void VKAPI_CALL layer_DestroyImage(VkDevice device, VkImage img, const VkAllocationCallbacks *alloc)
{
    DeviceData *d = findDevice(keyOf(device));

    if (img && d->fix)
    {
        imgRemove(img);
    }

    d->DestroyImage(device, img, alloc);
}

// ============================================================================================ instance lifetime

// vkCreateInstance hook: creates the instance through the rest of the chain and records it.
static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateInstance(const VkInstanceCreateInfo *info,
                                                           const VkAllocationCallbacks *alloc, VkInstance *instance)
{
    // The loader passes the next layer's vkGetInstanceProcAddr in the pNext chain. Advance the link so
    // the next layer finds its own entry.
    VkLayerInstanceCreateInfo *link = (VkLayerInstanceCreateInfo *)info->pNext;
    while (link &&
           !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
    {
        link = (VkLayerInstanceCreateInfo *)link->pNext;
    }

    if (!link)
    {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    PFN_vkCreateInstance next = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkResult result = next(info, alloc, instance);
    if (result != VK_SUCCESS)
    {
        return result;
    }

    InstanceData *d = calloc(1, sizeof *d);
    d->key = keyOf(*instance);
    d->instance = *instance;
    d->active = appWantsFix(info->pApplicationInfo);
    d->gipa = gipa;

    // Look up the next layer's instance functions. LOAD(Foo) stores vkFoo in d->Foo.
#define LOAD(fn) d->fn = (PFN_vk##fn)gipa(*instance, "vk" #fn)

    LOAD(DestroyInstance);
    LOAD(EnumeratePhysicalDevices);
    LOAD(GetPhysicalDeviceProperties);
    LOAD(GetPhysicalDeviceMemoryProperties);
    LOAD(GetPhysicalDeviceFormatProperties);
    LOAD(GetPhysicalDeviceQueueFamilyProperties);
#undef LOAD

    AcquireSRWLockExclusive(&g_tableLock);

    for (int i = 0; i < MAX_INSTANCES; i++)
    {
        if (!g_instances[i])
        {
            g_instances[i] = d;
            break;
        }
    }

    ReleaseSRWLockExclusive(&g_tableLock);

    // Map each physical device's dispatch key to this instance, for vkCreateDevice.
    uint32_t physicalCount = 0;
    if (d->EnumeratePhysicalDevices(*instance, &physicalCount, NULL) == VK_SUCCESS && physicalCount)
    {
        VkPhysicalDevice *physicals = calloc(physicalCount, sizeof *physicals);
        if (physicals && d->EnumeratePhysicalDevices(*instance, &physicalCount, physicals) >= 0)
        {
            AcquireSRWLockExclusive(&g_tableLock);

            for (uint32_t i = 0; i < physicalCount && g_physKeyCount < MAX_PHYS_KEYS; i++)
            {
                g_physKeys[g_physKeyCount].key = keyOf(physicals[i]);
                g_physKeys[g_physKeyCount].inst = d;
                g_physKeyCount++;
            }

            ReleaseSRWLockExclusive(&g_tableLock);
        }

        free(physicals);
    }

    if (d->active)
    {
        const VkApplicationInfo *app = info->pApplicationInfo;
        const char *appName = (app && app->pApplicationName) ? app->pApplicationName : "";
        const char *engineName = (app && app->pEngineName) ? app->pEngineName : "";
        logf_("RimWorldAMDFix %s loaded by \"%s\" (engine \"%s\")", RWAMDFIX_VERSION, appName, engineName);
    }

    return VK_SUCCESS;
}

// vkDestroyInstance hook: destroys the instance and drops its records.
static VKAPI_ATTR void VKAPI_CALL layer_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc)
{
    InstanceData *d = findInstance(keyOf(instance));
    if (!d)
    {
        return;
    }

    d->DestroyInstance(instance, alloc);

    AcquireSRWLockExclusive(&g_tableLock);

    for (int i = 0; i < MAX_INSTANCES; i++)
    {
        if (g_instances[i] == d)
        {
            g_instances[i] = NULL;
        }
    }

    // Remove this instance's physical-device keys by moving the last entry into each freed slot.
    for (int i = 0; i < g_physKeyCount; i++)
    {
        if (g_physKeys[i].inst == d)
        {
            g_physKeys[i] = g_physKeys[--g_physKeyCount];
            i--;
        }
    }

    ReleaseSRWLockExclusive(&g_tableLock);
    free(d);
}

// ============================================================================================ device lifetime

// Decides whether a device needs the fix: forced on, the self-test shows the bug, or (if the test
// cannot run) the GPU is AMD.
static int decideFix(DeviceData *d, const VkDeviceCreateInfo *info, PFN_vkSetDeviceLoaderData setLoaderData)
{
    VkPhysicalDeviceProperties properties;
    d->inst->GetPhysicalDeviceProperties(d->physical, &properties);
    logf_("GPU: %s (vendor 0x%04x, driver version 0x%08x)", properties.deviceName, properties.vendorID,
          properties.driverVersion);

    if (envIs1("RIMWORLDAMDFIX_FORCE"))
    {
        logf_("fix forced on by RIMWORLDAMDFIX_FORCE=1");
        return 1;
    }

    // The test needs a graphics-capable queue, and may only use queue families the app requested.
    uint32_t familyCount = 0;
    d->inst->GetPhysicalDeviceQueueFamilyProperties(d->physical, &familyCount, NULL);
    VkQueueFamilyProperties *families = calloc(familyCount ? familyCount : 1, sizeof *families);
    d->inst->GetPhysicalDeviceQueueFamilyProperties(d->physical, &familyCount, families);

    int family = -1;

    for (uint32_t i = 0; i < info->queueCreateInfoCount && family < 0; i++)
    {
        uint32_t index = info->pQueueCreateInfos[i].queueFamilyIndex;
        int isGraphics = index < familyCount && (families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT);
        if (isGraphics && info->pQueueCreateInfos[i].queueCount > 0)
        {
            family = (int)index;
        }
    }

    free(families);

    int texels = -1;
    if (family >= 0 && setLoaderData)
    {
        texels = selfTest(d, (uint32_t)family, setLoaderData);
    }

    if (texels == 64)
    {
        logf_("self-test: 1:1 BC7 blit copied 64/64 texels; this driver does not have the bug, the layer stays "
              "inactive");
        return 0;
    }

    if (texels >= 0)
    {
        logf_("self-test: 1:1 BC7 blit copied %d/64 texels; driver bug present, fix ENABLED", texels);
        return 1;
    }

    int isAmd = properties.vendorID == 0x1002;
    logf_("self-test could not run; fix %s based on GPU vendor", isAmd ? "ENABLED" : "not enabled");
    return isAmd;
}

// vkCreateDevice hook: creates the device through the rest of the chain, records it, and decides
// whether the fix is needed on it.
static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                                                         const VkAllocationCallbacks *alloc, VkDevice *device)
{
    // As in vkCreateInstance: take the next layer's entry points and advance the link.
    VkLayerDeviceCreateInfo *link = (VkLayerDeviceCreateInfo *)info->pNext;
    while (link &&
           !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
    {
        link = (VkLayerDeviceCreateInfo *)link->pNext;
    }

    if (!link)
    {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    // The loader also passes a callback for setting up handles the layer creates itself (self-test).
    PFN_vkSetDeviceLoaderData setLoaderData = NULL;
    for (VkLayerDeviceCreateInfo *entry = (VkLayerDeviceCreateInfo *)info->pNext; entry;
         entry = (VkLayerDeviceCreateInfo *)entry->pNext)
    {
        if (entry->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && entry->function == VK_LOADER_DATA_CALLBACK)
        {
            setLoaderData = entry->u.pfnSetDeviceLoaderData;
        }
    }

    InstanceData *inst = findInstance(keyOf(physical));
    if (!inst)
    {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkCreateDevice next = (PFN_vkCreateDevice)gipa(inst->instance, "vkCreateDevice");
    VkResult result = next(physical, info, alloc, device);
    if (result != VK_SUCCESS)
    {
        return result;
    }

    DeviceData *d = calloc(1, sizeof *d);
    d->key = keyOf(*device);
    d->device = *device;
    d->physical = physical;
    d->inst = inst;
    d->gdpa = gdpa;
    InitializeSRWLock(&d->scratchLock);

    // Look up the next layer's device functions. LOAD(Foo) stores vkFoo in d->Foo.
#define LOAD(fn) d->fn = (PFN_vk##fn)gdpa(*device, "vk" #fn)

    // Used by the fix.
    LOAD(DestroyDevice);
    LOAD(CreateImage);
    LOAD(DestroyImage);
    LOAD(GetImageMemoryRequirements);
    LOAD(AllocateMemory);
    LOAD(FreeMemory);
    LOAD(BindImageMemory);
    LOAD(CmdBlitImage);
    LOAD(CmdPipelineBarrier);

    // Used by the self-test.
    LOAD(GetDeviceQueue);
    LOAD(CreateCommandPool);
    LOAD(DestroyCommandPool);
    LOAD(AllocateCommandBuffers);
    LOAD(BeginCommandBuffer);
    LOAD(EndCommandBuffer);
    LOAD(QueueSubmit);
    LOAD(QueueWaitIdle);
    LOAD(CreateBuffer);
    LOAD(DestroyBuffer);
    LOAD(GetBufferMemoryRequirements);
    LOAD(BindBufferMemory);
    LOAD(MapMemory);
    LOAD(CmdCopyBufferToImage);
    LOAD(CmdCopyImageToBuffer);
    LOAD(CmdClearColorImage);
#undef LOAD

    // Only RimWorld instances are considered; everywhere else the fix stays off.
    d->fix = 0;
    if (inst->active)
    {
        d->fix = decideFix(d, info, setLoaderData);
    }

    AcquireSRWLockExclusive(&g_tableLock);

    for (int i = 0; i < MAX_DEVICES; i++)
    {
        if (!g_devices[i])
        {
            g_devices[i] = d;
            break;
        }
    }

    ReleaseSRWLockExclusive(&g_tableLock);

    return VK_SUCCESS;
}

// vkDestroyDevice hook: releases the scratch images, destroys the device and drops its record.
static VKAPI_ATTR void VKAPI_CALL layer_DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc)
{
    DeviceData *d = findDevice(keyOf(device));
    if (!d)
    {
        return;
    }

    if (d->fix)
    {
        logf_("shutting down: %lld blits fixed", d->rewrites);
    }

    for (int i = 0; i < d->scratchCount; i++)
    {
        if (d->scratch[i].image)
        {
            d->DestroyImage(device, d->scratch[i].image, NULL);
        }

        if (d->scratch[i].memory)
        {
            d->FreeMemory(device, d->scratch[i].memory, NULL);
        }
    }

    d->DestroyDevice(device, alloc);

    AcquireSRWLockExclusive(&g_tableLock);

    for (int i = 0; i < MAX_DEVICES; i++)
    {
        if (g_devices[i] == d)
        {
            g_devices[i] = NULL;
        }
    }

    ReleaseSRWLockExclusive(&g_tableLock);

    free(d);
}

// ============================================================================================ entry points

static PFN_vkVoidFunction interceptDevice(const char *name, int fix);

// vkGetDeviceProcAddr for this layer.
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL RimWorldAMDFix_GetDeviceProcAddr(VkDevice device, const char *name)
{
    DeviceData *d = findDevice(keyOf(device));

    PFN_vkVoidFunction hook = interceptDevice(name, d && d->fix);
    if (hook)
    {
        return hook;
    }

    return d ? d->gdpa(device, name) : NULL;
}

// Returns this layer's hook for a device-level function, or NULL to use the next layer's.
// When the fix is off, only the functions the layer needs for bookkeeping are hooked, so it stays off
// every hot path.
static PFN_vkVoidFunction interceptDevice(const char *name, int fix)
{
    if (!strcmp(name, "vkGetDeviceProcAddr"))
    {
        return (PFN_vkVoidFunction)RimWorldAMDFix_GetDeviceProcAddr;
    }

    if (!strcmp(name, "vkDestroyDevice"))
    {
        return (PFN_vkVoidFunction)layer_DestroyDevice;
    }

    if (!fix)
    {
        return NULL;
    }

    if (!strcmp(name, "vkCreateImage"))
    {
        return (PFN_vkVoidFunction)layer_CreateImage;
    }

    if (!strcmp(name, "vkDestroyImage"))
    {
        return (PFN_vkVoidFunction)layer_DestroyImage;
    }

    if (!strcmp(name, "vkCmdBlitImage"))
    {
        return (PFN_vkVoidFunction)layer_CmdBlitImage;
    }

    return NULL;
}

// vkGetInstanceProcAddr for this layer.
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL RimWorldAMDFix_GetInstanceProcAddr(VkInstance instance,
                                                                                   const char *name)
{
    if (!strcmp(name, "vkGetInstanceProcAddr"))
    {
        return (PFN_vkVoidFunction)RimWorldAMDFix_GetInstanceProcAddr;
    }

    if (!strcmp(name, "vkCreateInstance"))
    {
        return (PFN_vkVoidFunction)layer_CreateInstance;
    }

    if (!strcmp(name, "vkDestroyInstance"))
    {
        return (PFN_vkVoidFunction)layer_DestroyInstance;
    }

    if (!strcmp(name, "vkCreateDevice"))
    {
        return (PFN_vkVoidFunction)layer_CreateDevice;
    }

    if (!instance)
    {
        return NULL;
    }

    // Device functions can also be fetched through the instance, before the device exists. Hand out
    // the hooks for RimWorld instances; each hook checks its device's fix flag before doing anything.
    InstanceData *d = findInstance(keyOf(instance));
    PFN_vkVoidFunction hook = interceptDevice(name, d && d->active);
    if (hook)
    {
        return hook;
    }

    return d ? d->gipa(instance, name) : NULL;
}

// Called by the Vulkan loader when it loads the layer, to agree on the layer interface version.
EXPORT VKAPI_ATTR VkResult VKAPI_CALL RimWorldAMDFix_NegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v)
{
    if (v->loaderLayerInterfaceVersion < 2)
    {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    v->loaderLayerInterfaceVersion = 2;
    v->pfnGetInstanceProcAddr = RimWorldAMDFix_GetInstanceProcAddr;
    v->pfnGetDeviceProcAddr = RimWorldAMDFix_GetDeviceProcAddr;
    v->pfnGetPhysicalDeviceProcAddr = NULL;

    return VK_SUCCESS;
}
