# Technical details

## The symptom

On affected systems, RimWorld 1.7 draws items on the map as only their top-left corner, or not at all. Everything else renders correctly.

## Where it comes from

RimWorld 1.7 runs on a new engine, Waylith (.NET with a Vulkan renderer). At load time the game packs item sprites into large texture atlases (`Verse.StaticTextureAtlas`). Each sprite is drawn into the atlas render target with `Graphics.DrawTexture`. In Waylith this becomes a **1:1 `vkCmdBlitImage`** from the sprite's texture, which is **BC7-compressed**: vanilla textures ship as pre-compressed `.dds`, and mod textures are compressed at load.

Decoding the atlas pages that the game caches on disk (`TextureCache\...\pages.*.zip`) shows that every slot holds only its sprite's top-left corner, at native scale. The surviving piece is exactly 1/4 of the sprite's width and height: 128 px sprites keep 32 px, 192 keep 48, 256 keep 64. Only about 4% of each atlas page is non-transparent.

## The driver bug

The standalone programs in `tools/repro` reproduce it without the game. Results on AMD Radeon RX 9070 XT, driver 26.10.1 (LLPC):

| Blit | Result |
|---|---|
| RGBA8 source, 1:1 | correct |
| BC7 / BC7 sRGB / BC1 source, 1:1, whole image | only (w/4) x (h/4) texels written, top-left |
| BC7, 1:1, sub-rect (64,64)-(96,96) | 8x8 texels written, read from texel (16,16) |
| BC7, 2:1 downscale | correct |
| BC7, upscale 128 to 240 | correct |
| BC7, mirrored (reversed offsets on both sides) | writes outside the destination rect and loses the device |

The driver advertises `VK_FORMAT_FEATURE_BLIT_SRC_BIT` for BC formats, and the Khronos validation layer reports no errors for these calls, so they are legal. On the 1:1 path, the driver divides the source offset and extent by the 4x4 block size. Scaled blits treat coordinates as texels, as the spec requires. Waylith's call is correct, and the driver produces the wrong result.

## The fix

`src/RimWorldAMDFix.c` is a Vulkan layer that intercepts `vkCmdBlitImage`. When the source is BC1-BC7 and the blit is unmirrored and exactly 1:1, it replaces the blit with:

1. a 2x **nearest** upscale of the source region into a scratch image in the destination's format, then
2. a 2:1 **nearest** downscale of that scratch region into the original destination rect.

Both passes use paths the driver handles correctly. With nearest filtering, pass 1 duplicates each texel into a 2x2 group and pass 2 picks one texel from each group. The result is bit-identical to a correct 1:1 blit. (For a 1:1 blit, LINEAR filtering samples texel centres and gives the same result as NEAREST.) Regions over 2048 px are done in tiles, so the scratch image stays at 4096x4096. Every other call passes through unchanged.

### When the layer acts

- **Application:** only in Vulkan instances whose `VkApplicationInfo` names engine `Waylith` or an application starting with `RimWorld`. RimWorld 1.7 reports "RimWorld by Ludeon Studios" / "Waylith". In other programs the layer returns the next layer's function pointers for everything except instance and device lifetime, so it is not on any hot path.
- **Driver:** at device creation the layer runs a self-test. It blits an 8x8 BC7 image 1:1 into an 8x8 RGBA8 image and counts the texels that arrive: 64 on a correct driver, 4 on an affected one. The fix is enabled only if the bug is present. If the test cannot run, the layer falls back to enabling the fix on AMD GPUs (vendor 0x1002).

### Environment variables

| Variable | Effect |
|---|---|
| `DISABLE_RIMWORLDAMDFIX=1` | The Vulkan loader does not load the layer at all |
| `RIMWORLDAMDFIX_FORCE=1` | Skip the self-test and always enable the fix (in RimWorld) |
| `RIMWORLDAMDFIX_ANY_APP=1` | Act in any application (for testing with `tools/repro`) |

## What the installer does

- Copies the layer to `%LOCALAPPDATA%\RimWorldAMDFix\<version>\`. Each version gets its own folder, so an upgrade never overwrites a DLL that a running program has loaded.
- Registers the manifest under `HKCU\Software\Khronos\Vulkan\ImplicitLayers`, and removes registrations of older versions.
- Adds an uninstall entry under `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\RimWorldAMDFix`, so the fix appears in Windows' Installed apps.
- Deletes RimWorld's cached atlas pages (`pages.*.zip` under `TextureCache`), so the atlases are rebuilt through the fixed path. Otherwise the broken cached atlases would keep being reused.

The uninstaller removes the registry entries and the install folder.

## Reporting

- **AMD:** the bug is in the driver. Attach `tools/repro` and the table above.
- **Ludeon:** the engine can avoid it by not using `vkCmdBlitImage` for block-compressed sources, for example by drawing a textured quad instead.
