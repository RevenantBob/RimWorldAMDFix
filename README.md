# RimWorld AMD Fix

Fixes **RimWorld 1.7 (beta) items drawing as only their top-left corner, or not at all,** on AMD graphics cards.

If your stockpiles look empty, packaged meals show as a small corner, and "forbidden" X marks float over bare ground, this is the bug. Pawns, buildings and terrain draw fine; items on the ground do not.

> [!WARNING]
> **Use at your own risk.** This is an unofficial, community-made fix, not affiliated with Ludeon Studios or AMD. It is provided "as is", without warranty of any kind (see [LICENSE](LICENSE)), and I am not responsible for any problems that may result from using it. I made this to help other players hit by the same bug until it is fixed properly.

## Testing status

So far this has only been tested on one PC:

- AMD Radeon RX 9070 XT, driver 26.10.1, Windows 11
- RimWorld 1.7.4984 (beta)

It checks your driver at startup and only switches itself on if the bug is present, but it has not yet been tried on other GPUs, drivers or setups. If you try it, please [open an issue](https://github.com/RevenantBob/RimWorldAMDFix/issues) saying whether it worked, with your GPU, driver version and the log file (see [Is it working?](#is-it-working)).

## Install

1. Close RimWorld.
2. Download the latest `RimWorldAMDFix-vX.Y.Z.zip` from [Releases](https://github.com/RevenantBob/RimWorldAMDFix/releases), right-click it and choose **Extract All...**
3. Open the extracted folder and double-click **`Install.bat`**.
4. Start RimWorld as usual. The first start takes a little longer while the game rebuilds its texture atlases.

No administrator rights are needed, and RimWorld's own files are not changed.

From a terminal, run `.\Install.bat` inside the extracted folder. A bare `install` does not work: PowerShell never runs commands from the current folder, so it would start some other program's `install` instead.

## Uninstall

Double-click **`Uninstall.bat`**, or go to **Settings > Apps > Installed apps > RimWorld AMD Fix (Vulkan layer) > Uninstall**.

## Is it working?

Items should draw normally. The fix also keeps a short log at `%LOCALAPPDATA%\RimWorldAMDFix\RimWorldAMDFix.log`. On an affected PC it looks like this:

```
RimWorldAMDFix 1.0.0 loaded by "RimWorld by Ludeon Studios" (engine "Waylith")
GPU: AMD Radeon RX 9070 XT (vendor 0x1002, driver version 0x008001a0)
self-test: 1:1 BC7 blit copied 4/64 texels; driver bug present, fix ENABLED
fixed blit #1: 192x192 from format 145 into format 37
...
```

If the self-test line says `64/64 texels; this driver does not have the bug`, your driver is fine and the fix stays out of the way.

## FAQ

**What does it actually do?**
It is a Vulkan layer: a small DLL that Windows' Vulkan loader places between a game and the graphics driver. When RimWorld builds its item texture atlases, it copies each item sprite with a Vulkan call the AMD driver gets wrong for compressed textures. The layer catches exactly that call and performs the copy in a way the driver handles correctly. Nothing else is changed. See [docs/TECHNICAL.md](docs/TECHNICAL.md) for the details.

**Does it affect other games?**
Vulkan loads the layer into every Vulkan program, but it only does anything in RimWorld (it checks the application and engine name the game reports to Vulkan). In other programs it passes every call straight through and writes nothing.

**I don't have an AMD card / my driver is fine. Will it hurt?**
No. At startup it runs a tiny test copy on your GPU, and only enables itself if the result shows the bug. Once AMD fixes the driver, the fix switches itself off automatically.

**Does it change RimWorld or my saves?**
No. It only deletes RimWorld's cached texture atlases once, at install, so the game rebuilds them correctly. Saves and game files are untouched.

**Can I turn it off without uninstalling?**
Set the environment variable `DISABLE_RIMWORLDAMDFIX=1`.

**Windows warned me about the download.**
The scripts and DLL are not code-signed. The full source is in this repository, and the release zips are built by GitHub Actions from it.

## Building

Requires Windows and either Visual Studio (Desktop development with C++) or [zig](https://ziglang.org/).

```
.\build.ps1
```

This downloads the Vulkan headers (the only dependency), builds `RimWorldAMDFix.dll`, and writes the release zip to `dist\`. The code style is defined in `.clang-format`; format with `clang-format -i src\*.c tools\repro\*.c`. Pushing a tag `vX.Y.Z` that matches the `VERSION` file makes GitHub Actions build and publish a release.

## License

[MIT](LICENSE). The software is provided "as is", without warranty of any kind.

## Repository layout

| Path | Contents |
|---|---|
| `src/` | The layer (`RimWorldAMDFix.c`) and its Vulkan manifest |
| `installer/` | `Install.bat`, `Uninstall.bat` and the PowerShell scripts behind them |
| `tools/repro/` | Standalone programs that reproduce the driver bug, for bug reports |
| `docs/TECHNICAL.md` | How the bug was found and how the fix works |
| `.github/release-notes.md` | Template for the release description (version filled in by `build.ps1`) |
