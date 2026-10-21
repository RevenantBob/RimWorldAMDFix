Fixes RimWorld 1.7 (beta, Vulkan) drawing items as only their top-left corner, or not at all, on AMD graphics cards.

> [!WARNING]
> **Use at your own risk.** This is an unofficial, community-made fix, not affiliated with Ludeon Studios or AMD. It is provided "as is", without warranty of any kind (see the [MIT License](https://github.com/RevenantBob/RimWorldAMDFix/blob/main/LICENSE)), and I am not responsible for any problems that may result from using it. I made this to help other players hit by the same bug until it is fixed properly.

### Testing
So far this has only been tested on one PC:
- AMD Radeon RX 9070 XT, driver 26.10.1, Windows 11
- RimWorld 1.7.4984 (beta)

It checks your driver at startup and only switches itself on if the bug is present, but it has not yet been tried on other GPUs, drivers or setups. If you try it, please [open an issue](https://github.com/RevenantBob/RimWorldAMDFix/issues) saying whether it worked, with your GPU, driver version and the log file (`%LOCALAPPDATA%\RimWorldAMDFix\RimWorldAMDFix.log`).

### Install
1. Close RimWorld.
2. Download **RimWorldAMDFix-v@VERSION@.zip** below, right-click it and choose **Extract All...**
3. Open the extracted folder and double-click **Install.bat**. (From a terminal, run `.\Install.bat`.)
4. Start RimWorld. The first start takes a little longer while the game rebuilds its texture atlases.

### Uninstall
Double-click **Uninstall.bat**, or go to **Settings > Apps > Installed apps > RimWorld AMD Fix (Vulkan layer) > Uninstall**. RimWorld's own files and your saves are never modified.
