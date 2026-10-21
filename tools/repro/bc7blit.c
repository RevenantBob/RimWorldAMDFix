// bc7blit: the minimal repro for the driver bug.
//
// Each case blits a solid opaque red sprite 1:1 into a larger RGBA8 image that was cleared to
// transparent black, then reads the image back and counts the opaque texels. A correct driver writes
// exactly width x height of them. This is the same kind of call RimWorld 1.7 uses to build its item
// texture atlases.
//
// The source is either RGBA8 (the control, which works everywhere) or a block-compressed format.
// On an affected driver every compressed case writes only (width/4) x (height/4) texels.
//
// Optional environment variables, for testing the RimWorldAMDFix layer:
//   BC7BLIT_APPNAME  application name reported to Vulkan (default "bc7blit")
//   BC7BLIT_ENGINE   engine name reported to Vulkan (default none)

#include "repro.h"

// The destination image, and where in it each sprite is blitted.
#define DST_WIDTH 256
#define DST_HEIGHT 256
#define DST_X 16
#define DST_Y 16

// Fills `buffer` with a solid opaque red sprite in `format`, ready to upload into a width x height image.
static void fillSolidRed(Buffer buffer, VkFormat format, uint32_t width, uint32_t height)
{
    uint32_t blockCount = (width / 4) * (height / 4);

    if (format == VK_FORMAT_R8G8B8A8_UNORM)
    {
        // Uncompressed: 4 bytes per texel.
        for (uint32_t i = 0; i < width * height; i++)
        {
            uint8_t *texel = buffer.data + i * 4;
            texel[0] = 255;
            texel[1] = 0;
            texel[2] = 0;
            texel[3] = 255;
        }
    }
    else if (format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK)
    {
        // BC1: 8 bytes per 4x4 block.
        for (uint32_t i = 0; i < blockCount; i++)
        {
            encodeBc1SolidRed(buffer.data + i * 8);
        }
    }
    else
    {
        // BC7 (UNORM or sRGB): 16 bytes per 4x4 block.
        for (uint32_t i = 0; i < blockCount; i++)
        {
            encodeBc7Solid(255, 0, 0, 255, buffer.data + i * 16);
        }
    }
}

// Size in bytes of a width x height image in `format`.
static VkDeviceSize imageBytes(VkFormat format, uint32_t width, uint32_t height)
{
    if (format == VK_FORMAT_R8G8B8A8_UNORM)
    {
        return (VkDeviceSize)width * height * 4;
    }

    VkDeviceSize blockBytes = (format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK) ? 8 : 16;
    return (VkDeviceSize)(width / 4) * (height / 4) * blockBytes;
}

// Runs one case and prints its result line. Returns 1 if the blit was correct.
static int runCase(const char *name, VkFormat format, uint32_t size, VkFilter filter)
{
    // The driver's claimed support, printed with the result: the bug is that this is advertised
    // but does not work for 1:1 blits.
    VkFormatProperties formatProperties;
    vkGetPhysicalDeviceFormatProperties(g_physical, format, &formatProperties);
    int blitSrcSupported = (formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0;

    // Resources: the sprite, its upload buffer, the destination and a readback buffer.
    Buffer upload = createBuffer(imageBytes(format, size, size), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    fillSolidRed(upload, format, size, size);

    Image src = createImage(format, size, size);
    Image dst = createImage(VK_FORMAT_R8G8B8A8_UNORM, DST_WIDTH, DST_HEIGHT);
    Buffer readback = createBuffer((VkDeviceSize)DST_WIDTH * DST_HEIGHT * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    VkCommandBuffer cb = beginCommands();

    recordUpload(cb, upload, src, size, size);
    recordClear(cb, dst);

    // The call under test: the whole sprite, 1:1, to an offset inside the destination.
    VkImageBlit blit = {0};
    blit.srcSubresource = colorLayer();
    blit.dstSubresource = colorLayer();
    blit.srcOffsets[0] = (VkOffset3D){0, 0, 0};
    blit.srcOffsets[1] = (VkOffset3D){(int32_t)size, (int32_t)size, 1};
    blit.dstOffsets[0] = (VkOffset3D){DST_X, DST_Y, 0};
    blit.dstOffsets[1] = (VkOffset3D){DST_X + (int32_t)size, DST_Y + (int32_t)size, 1};

    vkCmdBlitImage(cb, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, filter);

    recordReadback(cb, dst, readback, DST_WIDTH, DST_HEIGHT);
    submitAndWait(cb);

    // Count the opaque texels and the bounding box they cover.
    uint32_t written = 0;
    uint32_t minX = DST_WIDTH;
    uint32_t minY = DST_HEIGHT;
    uint32_t maxX = 0;
    uint32_t maxY = 0;

    for (uint32_t y = 0; y < DST_HEIGHT; y++)
    {
        for (uint32_t x = 0; x < DST_WIDTH; x++)
        {
            uint8_t alpha = readback.data[(y * DST_WIDTH + x) * 4 + 3];
            if (alpha == 0)
            {
                continue;
            }

            written++;

            if (x < minX)
            {
                minX = x;
            }

            if (y < minY)
            {
                minY = y;
            }

            if (x > maxX)
            {
                maxX = x;
            }

            if (y > maxY)
            {
                maxY = y;
            }
        }
    }

    // Report.
    uint32_t expected = size * size;
    int correct = written == expected;
    const char *filterName = (filter == VK_FILTER_LINEAR) ? "LINEAR" : "NEAREST";

    printf("%-26s %3ux%-3u %-7s BLIT_SRC=%d  written=%6u expected=%6u  bbox=", name, size, size, filterName,
           blitSrcSupported, written, expected);

    if (written)
    {
        printf("(%u,%u)-(%u,%u) %ux%u", minX, minY, maxX, maxY, maxX - minX + 1, maxY - minY + 1);
    }
    else
    {
        printf("none");
    }

    printf("  %s\n", correct ? "OK" : "WRONG");

    // Clean up.
    destroyBuffer(upload);
    destroyBuffer(readback);
    destroyImage(src);
    destroyImage(dst);

    return correct;
}

int main(void)
{
    const char *appName = getenv("BC7BLIT_APPNAME");
    const char *engineName = getenv("BC7BLIT_ENGINE");
    initVulkan(appName ? appName : "bc7blit", engineName);

    // Every combination of filter, sprite size and source format.
    const VkFilter filters[] = {VK_FILTER_NEAREST, VK_FILTER_LINEAR};
    const uint32_t sizes[] = {64, 128, 192};

    int wrongCases = 0;

    for (int f = 0; f < 2; f++)
    {
        for (int s = 0; s < 3; s++)
        {
            VkFilter filter = filters[f];
            uint32_t size = sizes[s];

            wrongCases += !runCase("R8G8B8A8_UNORM (control)", VK_FORMAT_R8G8B8A8_UNORM, size, filter);
            wrongCases += !runCase("BC7_UNORM", VK_FORMAT_BC7_UNORM_BLOCK, size, filter);
            wrongCases += !runCase("BC7_SRGB", VK_FORMAT_BC7_SRGB_BLOCK, size, filter);
            wrongCases += !runCase("BC1_RGBA_UNORM", VK_FORMAT_BC1_RGBA_UNORM_BLOCK, size, filter);
        }
    }

    printf("\n%d case(s) wrong\n", wrongCases);

    shutdownVulkan();

    // Exit code 2 when the bug shows, so scripts can tell.
    return wrongCases ? 2 : 0;
}
