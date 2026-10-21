RimWorldAMDFix
==============

Fixes RimWorld 1.7 (Vulkan) drawing items as only their top-left corner, or not at all,
on AMD graphics cards.

USE AT YOUR OWN RISK
  This is an unofficial, community-made fix, not affiliated with Ludeon Studios or AMD.
  It is provided "as is", without warranty of any kind (see LICENSE.txt), and the author
  is not responsible for any problems that may result from using it. So far it has only
  been tested on one PC (AMD Radeon RX 9070 XT, driver 26.10.1, Windows 11).

INSTALL
  1. Close RimWorld.
  2. Make sure this zip is extracted (right-click the zip > Extract All...).
  3. Double-click Install.bat. From a terminal, run .\Install.bat in this folder.
  4. Start RimWorld as usual. The first start after installing takes a little longer
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
