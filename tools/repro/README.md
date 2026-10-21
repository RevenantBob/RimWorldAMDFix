# Driver bug repro tools

Small standalone Vulkan programs that show the driver bug without RimWorld. Attach these to bug reports.

| Program | What it shows |
|---|---|
| `bc7blit.c` | 1:1 `vkCmdBlitImage` of a solid sprite from BC7 / BC7 sRGB / BC1 into RGBA8, sizes 64/128/192, NEAREST and LINEAR. An RGBA8 source is the control. Prints how many texels arrived. |
| `probe.c` | Each 4x4 block has its own colour, so every destination texel tells you which source texel it came from. Shows that only 1:1 blits are broken (source offset and extent are divided by 4) while scaled blits are correct. Run `probe.exe twopass` to test the workaround the layer uses. |
| `big.c` | One 4100x2600 1:1 BC7 blit. Checks every texel and that nothing is written outside the destination rect. |

On an affected driver (seen on AMD Radeon RX 9070 XT, driver 26.10.1), every BC case in `bc7blit` arrives as exactly (w/4) x (h/4) texels in the top-left corner. With the Khronos validation layer enabled, no errors are reported for the legal cases. In `probe`, case C intentionally passes an out-of-bounds rect and is expected to fail validation.

## Build

Needs the [Vulkan SDK](https://vulkan.lunarg.com/) (for `vulkan-1.lib`). With zig:

```
zig cc -O2 -target x86_64-windows-gnu -I%VULKAN_SDK%\Include bc7blit.c -L%VULKAN_SDK%\Lib -lvulkan-1 -o bc7blit.exe
```

or from a Visual Studio x64 developer prompt:

```
cl /O2 /I%VULKAN_SDK%\Include bc7blit.c %VULKAN_SDK%\Lib\vulkan-1.lib
```

`probe.c` and `big.c` build the same way. All three share the helpers in `repro.h`.

## Testing the layer with these tools

The layer only acts in RimWorld, so set one of these when running the tools with the layer installed:

- `RIMWORLDAMDFIX_ANY_APP=1`: the layer acts in any app.
- `BC7BLIT_APPNAME="RimWorld by Ludeon Studios"` and `BC7BLIT_ENGINE=Waylith`: `bc7blit` presents itself to Vulkan the way RimWorld does.
