// probe: shows exactly what the driver does wrong.
//
// The 128x128 BC7 source gives every 4x4 block its own colour: red = blockX * 8, green = blockY * 8,
// blue = 255. So every texel that arrives in the destination says which source block it came from.
// Each case blits part or all of the source into a cleared RGBA8 image, then checks every destination
// texel against where it should have come from.
//
// The cases show that only 1:1 blits are broken. On an affected driver, a 1:1 blit has its source
// offset and extent divided by 4, while scaled blits are correct.
//
//   probe            run the diagnosis cases
//   probe twopass    run the same 1:1 blits through the two-pass workaround the layer uses

#include "repro.h"

#define SRC_SIZE 128
#define DST_WIDTH 256
#define DST_HEIGHT 256
#define DST_X 16
#define DST_Y 16

// With twopass set, each blit goes through a scratch image: a 2x nearest upscale, then a 2:1
// nearest downscale. This is the workaround the layer uses, and both passes are exact.
static int g_twoPass = 0;

// Fills `buffer` with the position-coded source, in `format` (RGBA8 or BC7).
static void fillPositionCoded(Buffer buffer, VkFormat format)
{
    const uint32_t blocksPerRow = SRC_SIZE / 4;

    if (format == VK_FORMAT_R8G8B8A8_UNORM)
    {
        // Uncompressed: give each texel the colour of the block it is in. "+ 1" matches the low bit
        // that BC7 mode 6 forces on, so both formats decode to the same values.
        for (uint32_t y = 0; y < SRC_SIZE; y++)
        {
            for (uint32_t x = 0; x < SRC_SIZE; x++)
            {
                uint8_t *texel = buffer.data + (y * SRC_SIZE + x) * 4;
                texel[0] = (uint8_t)((x / 4) * 8 + 1);
                texel[1] = (uint8_t)((y / 4) * 8 + 1);
                texel[2] = 255;
                texel[3] = 255;
            }
        }

        return;
    }

    for (uint32_t blockY = 0; blockY < blocksPerRow; blockY++)
    {
        for (uint32_t blockX = 0; blockX < blocksPerRow; blockX++)
        {
            uint8_t *block = buffer.data + (blockY * blocksPerRow + blockX) * 16;
            encodeBc7Solid((uint8_t)(blockX * 8), (uint8_t)(blockY * 8), 255, 255, block);
        }
    }
}

// Records the two-pass replacement for a 1:1 blit (the same steps as the layer's blitTwoPass).
// Returns the scratch image, which the caller destroys after the commands have run.
static Image recordTwoPass(VkCommandBuffer cb, Image src, Image dst, const VkImageBlit *blit)
{
    int32_t width = blit->srcOffsets[1].x - blit->srcOffsets[0].x;
    int32_t height = blit->srcOffsets[1].y - blit->srcOffsets[0].y;

    Image scratch = createImage(VK_FORMAT_R8G8B8A8_UNORM, (uint32_t)(2 * width), (uint32_t)(2 * height));
    imageBarrier(cb, scratch.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    // Pass 1: upscale the source region 2x into the scratch image.
    VkImageBlit up = *blit;
    up.dstOffsets[0] = (VkOffset3D){0, 0, 0};
    up.dstOffsets[1] = (VkOffset3D){2 * width, 2 * height, 1};

    vkCmdBlitImage(cb, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scratch.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up, VK_FILTER_NEAREST);

    imageBarrier(cb, scratch.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    // Pass 2: downscale the scratch image 2:1 into the original destination rect.
    VkImageBlit down = *blit;
    down.srcOffsets[0] = (VkOffset3D){0, 0, 0};
    down.srcOffsets[1] = (VkOffset3D){2 * width, 2 * height, 1};

    vkCmdBlitImage(cb, scratch.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &down, VK_FILTER_NEAREST);

    return scratch;
}

// Blits the source rect (srcX0, srcY0)-(srcX1, srcY1) into a dstWidth x dstHeight rect at
// (DST_X, DST_Y) and prints what arrived.
static void probe(const char *label, VkFormat format, int32_t srcX0, int32_t srcY0, int32_t srcX1, int32_t srcY1,
                  int32_t dstWidth, int32_t dstHeight)
{
    int compressed = (format != VK_FORMAT_R8G8B8A8_UNORM);
    VkDeviceSize srcBytes = compressed ? (SRC_SIZE / 4) * (SRC_SIZE / 4) * 16 : SRC_SIZE * SRC_SIZE * 4;

    // Resources.
    Buffer upload = createBuffer(srcBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    fillPositionCoded(upload, format);

    Image src = createImage(format, SRC_SIZE, SRC_SIZE);
    Image dst = createImage(VK_FORMAT_R8G8B8A8_UNORM, DST_WIDTH, DST_HEIGHT);
    Buffer srcReadback = createBuffer(srcBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Buffer dstReadback = createBuffer((VkDeviceSize)DST_WIDTH * DST_HEIGHT * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    VkCommandBuffer cb = beginCommands();

    recordUpload(cb, upload, src, SRC_SIZE, SRC_SIZE);

    // Read the source straight back as well: this shows the whole image really is on the GPU, so
    // any missing texels are lost in the blit, not in the upload.
    VkBufferImageCopy srcCopy = {0};
    srcCopy.imageSubresource = colorLayer();
    srcCopy.imageExtent = (VkExtent3D){SRC_SIZE, SRC_SIZE, 1};

    vkCmdCopyImageToBuffer(cb, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, srcReadback.buffer, 1, &srcCopy);

    recordClear(cb, dst);

    // The blit (all coordinates in texels, as the Vulkan spec defines them).
    VkImageBlit blit = {0};
    blit.srcSubresource = colorLayer();
    blit.dstSubresource = colorLayer();
    blit.srcOffsets[0] = (VkOffset3D){srcX0, srcY0, 0};
    blit.srcOffsets[1] = (VkOffset3D){srcX1, srcY1, 1};
    blit.dstOffsets[0] = (VkOffset3D){DST_X, DST_Y, 0};
    blit.dstOffsets[1] = (VkOffset3D){DST_X + dstWidth, DST_Y + dstHeight, 1};

    // Record either the plain blit, or (in twopass mode) the workaround that replaces it. The
    // workaround's scratch image must live until the commands have run.
    Image scratch = {0};

    if (g_twoPass)
    {
        scratch = recordTwoPass(cb, src, dst, &blit);
    }
    else
    {
        vkCmdBlitImage(cb, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    }

    recordReadback(cb, dst, dstReadback, DST_WIDTH, DST_HEIGHT);
    submitAndWait(cb);

    // Check every destination texel.
    uint32_t written = 0;
    uint32_t correct = 0;
    uint32_t minBlockX = 999;
    uint32_t maxBlockX = 0;
    uint32_t minBlockY = 999;
    uint32_t maxBlockY = 0;
    uint32_t minX = DST_WIDTH;
    uint32_t minY = DST_HEIGHT;
    uint32_t maxX = 0;
    uint32_t maxY = 0;

    for (uint32_t y = 0; y < DST_HEIGHT; y++)
    {
        for (uint32_t x = 0; x < DST_WIDTH; x++)
        {
            const uint8_t *texel = dstReadback.data + (y * DST_WIDTH + x) * 4;
            if (texel[3] == 0)
            {
                continue;
            }

            written++;

            // Which source block this texel came from (undo "blockX * 8 + 1").
            uint32_t blockX = texel[0] >> 3;
            uint32_t blockY = texel[1] >> 3;

            minBlockX = (blockX < minBlockX) ? blockX : minBlockX;
            maxBlockX = (blockX > maxBlockX) ? blockX : maxBlockX;
            minBlockY = (blockY < minBlockY) ? blockY : minBlockY;
            maxBlockY = (blockY > maxBlockY) ? blockY : maxBlockY;

            minX = (x < minX) ? x : minX;
            maxX = (x > maxX) ? x : maxX;
            minY = (y < minY) ? y : minY;
            maxY = (y > maxY) ? y : maxY;

            // Where a correct blit would have taken this texel from: scale the texel centre from the
            // destination rect into the source rect.
            int insideRect =
                x >= DST_X && y >= DST_Y && x < (uint32_t)(DST_X + dstWidth) && y < (uint32_t)(DST_Y + dstHeight);
            if (insideRect)
            {
                int expectedX = srcX0 + (int)(((x - DST_X) + 0.5) * (srcX1 - srcX0) / dstWidth);
                int expectedY = srcY0 + (int)(((y - DST_Y) + 0.5) * (srcY1 - srcY0) / dstHeight);

                if ((int)blockX == expectedX / 4 && (int)blockY == expectedY / 4)
                {
                    correct++;
                }
            }
        }
    }

    // Report.
    int uploadIntact = memcmp(upload.data, srcReadback.data, (size_t)srcBytes) == 0;
    uint32_t expectedTexels = (uint32_t)(dstWidth * dstHeight);

    printf("%s\n", label);
    printf("  upload readback matches:      %s\n", uploadIntact ? "yes (whole 128x128 source is on the GPU)" : "NO");
    printf("  src rect passed to blit:      (%d,%d)-(%d,%d)   dst rect: (%d,%d)-(%d,%d)\n", srcX0, srcY0, srcX1, srcY1,
           DST_X, DST_Y, DST_X + dstWidth, DST_Y + dstHeight);

    if (written == 0)
    {
        printf("  texels written:               0\n\n");
    }
    else
    {
        printf("  texels written:               %u of %u   region %ux%u at (%u,%u)\n", written, expectedTexels,
               maxX - minX + 1, maxY - minY + 1, minX, minY);
        printf("  source blocks they came from: bx %u..%u, by %u..%u  (full sprite = 0..31)\n", minBlockX, maxBlockX,
               minBlockY, maxBlockY);
        printf("  texels with correct content:  %u of %u (texel-unit mapping)\n", correct, expectedTexels);

        // The first destination row, sampled every 4 texels: which source block landed there.
        printf("  row 0 source block x for dst +0,+4,+8,..,+28:");
        for (int k = 0; k < 32; k += 4)
        {
            const uint8_t *texel = dstReadback.data + (DST_Y * DST_WIDTH + DST_X + k) * 4;
            if (texel[3])
            {
                printf(" %u", texel[0] >> 3);
            }
            else
            {
                printf(" -");
            }
        }

        printf("\n\n");
    }

    // Clean up.
    destroyBuffer(upload);
    destroyBuffer(srcReadback);
    destroyBuffer(dstReadback);
    destroyImage(src);
    destroyImage(dst);

    if (scratch.image)
    {
        destroyImage(scratch);
    }
}

int main(int argc, char **argv)
{
    initVulkan("bc7probe", NULL);

    if (argc > 1 && strcmp(argv[1], "twopass") == 0)
    {
        // The broken 1:1 cases again, through the workaround. All should be fully correct.
        g_twoPass = 1;

        probe("[T1] two-pass, BC7 1:1 whole image", VK_FORMAT_BC7_UNORM_BLOCK, 0, 0, 128, 128, 128, 128);
        probe("[T2] two-pass, BC7 1:1 sub-rect (64,64)-(96,96)", VK_FORMAT_BC7_UNORM_BLOCK, 64, 64, 96, 96, 32, 32);
        probe("[T3] two-pass, BC7 1:1 sub-rect (16,16)-(48,48)", VK_FORMAT_BC7_UNORM_BLOCK, 16, 16, 48, 48, 32, 32);
        probe("[T4] two-pass, BC7 1:1 non-square 64x128", VK_FORMAT_BC7_UNORM_BLOCK, 0, 0, 64, 128, 64, 128);
        probe("[T5] two-pass, RGBA8 1:1 whole image", VK_FORMAT_R8G8B8A8_UNORM, 0, 0, 128, 128, 128, 128);
    }
    else
    {
        // A: the uncompressed control.
        probe("[A] RGBA8 control, 1:1 whole image", VK_FORMAT_R8G8B8A8_UNORM, 0, 0, 128, 128, 128, 128);

        // B: what RimWorld does. Broken on an affected driver.
        probe("[B] BC7, 1:1 whole image (what Waylith does)", VK_FORMAT_BC7_UNORM_BLOCK, 0, 0, 128, 128, 128, 128);

        // C: the source rect x4, as if the driver wanted block units. This is out of bounds, so the
        // validation layer reports it; it shows the driver does not simply expect block units.
        probe("[C] BC7, src rect x4 (illegal; block-unit hypothesis)", VK_FORMAT_BC7_UNORM_BLOCK, 0, 0, 512, 512, 128,
              128);

        // D, E: scaled blits from the same BC7 source. Correct on the affected driver.
        probe("[D] BC7, 2:1 downscale whole image (legal)", VK_FORMAT_BC7_UNORM_BLOCK, 0, 0, 128, 128, 64, 64);
        probe("[E] BC7, upscale 128->240 (legal)", VK_FORMAT_BC7_UNORM_BLOCK, 0, 0, 128, 128, 240, 240);

        // F, G: 1:1 sub-rects. On an affected driver both the offset and the size come out divided by 4.
        probe("[F] BC7, 1:1 sub-rect (64,64)-(96,96) (legal)", VK_FORMAT_BC7_UNORM_BLOCK, 64, 64, 96, 96, 32, 32);
        probe("[G] BC7, 1:1 sub-rect (16,16)-(48,48) (legal)", VK_FORMAT_BC7_UNORM_BLOCK, 16, 16, 48, 48, 32, 32);

        // Mirrored blits (reversed offsets on both sides) were tried as a workaround and are not
        // included: on the affected driver they write outside the destination and lose the device.
    }

    shutdownVulkan();
    return 0;
}
