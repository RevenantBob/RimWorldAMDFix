RimWorldAMDFix
==============

Fixes RimWorld 1.7 (Vulkan) drawing items as only their top-left corner, or not at all,
on AMD graphics cards.

INSTALL
  1. Close RimWorld.
  2. Double-click Install.bat.
  3. Start RimWorld as usual. The first start after installing takes a little longer
     while the game rebuilds its texture atlases.

UNINSTALL
  Double-click Uninstall.bat, or use Settings > Apps > Installed apps >
  "RimWorld AMD Fix (Vulkan layer)" > Uninstall.

NOTES
  - No administrator rights needed. RimWorld's own files are not changed.
  - Everything is installed to %LOCALAPPDATA%\RimWorldAMDFix.
  - The fix only switches on inside RimWorld, and only if your graphics driver actually has
    the bug (it checks at startup). On other PCs, or once AMD fixes the driver, it does nothing.
  - Log file: %LOCALAPPDATA%\RimWorldAMDFix\RimWorldAMDFix.log

More information and source code: https://github.com/RevenantBob/RimWorldAMDFix
