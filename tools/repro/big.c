// big: one large 1:1 BC7 blit, checked texel by texel.
//
// A 4100x2600 BC7 source is blitted 1:1 into a slightly larger, cleared RGBA8 image. The size is
// chosen to exercise the RimWorldAMDFix layer's tiling: tiles of 2048, plus a 4-texel-wide edge tile.
//
// Every 4x4 source block carries its own position, packed into its colour:
//   value = blockX | (blockY << 11)   (11 bits for blockX, up to 2047; 10 bits for blockY)
//   red = bits 0-6, green = bits 7-13, blue = bits 14-20
// Each channel holds 7 bits because BC7 mode 6 forces the lowest bit of every channel to 1.
//
// The check also makes sure nothing was written outside the destination rect.

#include "repro.h"

#define SRC_WIDTH 4100
#define SRC_HEIGHT 2600
#define BORDER 16
#define DST_WIDTH (SRC_WIDTH + 2 * BORDER)
#define DST_HEIGHT (SRC_HEIGHT + 2 * BORDER)

// Packs a block position into 7-bit channel values, shifted up one bit as BC7 mode 6 stores them.
static void positionToColor(uint32_t blockX, uint32_t blockY, uint8_t rgb[3])
{
    uint32_t value = blockX | (blockY << 11);

    rgb[0] = (uint8_t)((value & 127) << 1);
    rgb[1] = (uint8_t)(((value >> 7) & 127) << 1);
    rgb[2] = (uint8_t)(((value >> 14) & 127) << 1);
}

// Recovers the block position from a decoded texel (the inverse of positionToColor).
static void colorToPosition(const uint8_t *texel, uint32_t *blockX, uint32_t *blockY)
{
    uint32_t value = (texel[0] >> 1) | ((uint32_t)(texel[1] >> 1) << 7) | ((uint32_t)(texel[2] >> 1) << 14);

    *blockX = value & 2047;
    *blockY = value >> 11;
}

int main(void)
{
    initVulkan("bc7big", NULL);

    const uint32_t blocksPerRow = SRC_WIDTH / 4;
    const uint32_t blockRows = SRC_HEIGHT / 4;

    // Build the position-coded source.
    Buffer upload = createBuffer((VkDeviceSize)blocksPerRow * blockRows * 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);

    for (uint32_t blockY = 0; blockY < blockRows; blockY++)
    {
        for (uint32_t blockX = 0; blockX < blocksPerRow; blockX++)
        {
            uint8_t rgb[3];
            positionToColor(blockX, blockY, rgb);

            uint8_t *block = upload.data + ((VkDeviceSize)blockY * blocksPerRow + blockX) * 16;
            encodeBc7Solid(rgb[0], rgb[1], rgb[2], 255, block);
        }
    }

    Image src = createImage(VK_FORMAT_BC7_UNORM_BLOCK, SRC_WIDTH, SRC_HEIGHT);
    Image dst = createImage(VK_FORMAT_R8G8B8A8_UNORM, DST_WIDTH, DST_HEIGHT);
    Buffer readback = createBuffer((VkDeviceSize)DST_WIDTH * DST_HEIGHT * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    VkCommandBuffer cb = beginCommands();

    recordUpload(cb, upload, src, SRC_WIDTH, SRC_HEIGHT);
    recordClear(cb, dst);

    // The whole source, 1:1, with LINEAR filtering as RimWorld uses.
    VkImageBlit blit = {0};
    blit.srcSubresource = colorLayer();
    blit.dstSubresource = colorLayer();
    blit.srcOffsets[0] = (VkOffset3D){0, 0, 0};
    blit.srcOffsets[1] = (VkOffset3D){SRC_WIDTH, SRC_HEIGHT, 1};
    blit.dstOffsets[0] = (VkOffset3D){BORDER, BORDER, 0};
    blit.dstOffsets[1] = (VkOffset3D){BORDER + SRC_WIDTH, BORDER + SRC_HEIGHT, 1};

    vkCmdBlitImage(cb, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_LINEAR);

    recordReadback(cb, dst, readback, DST_WIDTH, DST_HEIGHT);
    submitAndWait(cb);

    // Check every texel: inside the rect it must come from the right block, outside it must be untouched.
    uint64_t inside = 0;
    uint64_t correct = 0;
    uint64_t writtenOutside = 0;

    for (uint32_t y = 0; y < DST_HEIGHT; y++)
    {
        for (uint32_t x = 0; x < DST_WIDTH; x++)
        {
            const uint8_t *texel = readback.data + ((size_t)y * DST_WIDTH + x) * 4;

            int insideRect = x >= BORDER && y >= BORDER && x < BORDER + SRC_WIDTH && y < BORDER + SRC_HEIGHT;
            if (!insideRect)
            {
                if (texel[3] != 0)
                {
                    writtenOutside++;
                }

                continue;
            }

            inside++;

            uint32_t blockX;
            uint32_t blockY;
            colorToPosition(texel, &blockX, &blockY);

            if (texel[3] == 255 && blockX == (x - BORDER) / 4 && blockY == (y - BORDER) / 4)
            {
                correct++;
            }
        }
    }

    // Report.
    int ok = (correct == inside && writtenOutside == 0);

    printf("BC7 %ux%u 1:1 blit (LINEAR, like the game): correct %llu of %llu, written outside rect %llu  -> %s\n",
           SRC_WIDTH, SRC_HEIGHT, (unsigned long long)correct, (unsigned long long)inside,
           (unsigned long long)writtenOutside, ok ? "OK" : "WRONG");

    // Clean up.
    destroyBuffer(upload);
    destroyBuffer(readback);
    destroyImage(src);
    destroyImage(dst);
    shutdownVulkan();

    return ok ? 0 : 2;
}
