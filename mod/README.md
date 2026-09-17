Private Storage Master 1.0.0 for Crimson Desert 2.02.00
=======================================================

Opens storage from anywhere in the game's own warehouse screen and sets each
storage's size.

Install
-------

1. Remove PrivateStorageAnywhere.asi from bin64 and disable it in your mod
   manager. Leave its ini in place for the first launch so your bindings are
   copied over.
2. Ultimate ASI Loader must be in bin64 next to CrimsonDesert.exe. If it is
   named version.dll and nothing loads, rename it to winmm.dll.
3. With the game closed, copy PrivateStorageMaster.asi into bin64.

The first launch writes PrivateStorageMaster.ini beside the plugin with every
setting explained. To uninstall, delete the PrivateStorageMaster files from
bin64; storage sizes go back to the game's on the next start.

Keys
----

    Ctrl+F1   Private Storage              LB + left stick
    Ctrl+F2   Gatherables Chest            LB + right stick
    Ctrl+F3   Wardrobe
    Ctrl+F4   Kuku Cooler
    Ctrl+F5   Collectibles Chest
    Ctrl+F6   Camp Straw (feed bin)
    Ctrl+F7   Bird Feed (bird feeder)
    Ctrl+F8   Camp Provisions (town warehouse)
    Ctrl+F9   Abyss gear storage (Kuku Pot bag)
    Ctrl+F12  write every storage's size to the log

The same key closes. Another storage's key switches straight to it. Esc and B
close too. Keys only work in free play.

Sizes
-----

<Name>Slots in the ini sets a storage's size, up to 1460, from the next start.
0 keeps the game's size, and the mod never makes storage smaller than the game
would. PrivateStorageSlots is a total that includes bought expansions. The
Collectibles Chest is always 958. LeaveCapacityAlone=1 turns every size off,
for use with JSON capacity mods.

With Master Looter installed, its menu has a Storage tab for all of this.

Reporting a problem
-------------------

Set DebugLog=1, play until it happens, close the game and attach
PrivateStorageMaster.log from bin64. Older sessions are kept as
PrivateStorageMaster.01.log and up.

Building
--------

MSVC Build Tools 2022 with its bundled CMake and Ninja. Run mod\build.bat by
full path; the plugin lands in mod\dist. mod/src/game/addresses.cpp lists
every pattern the plugin searches for.
