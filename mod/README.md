Private Storage Master 1.0.1 for Crimson Desert 2.02.00 and 2.03.00
===================================================================

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
    Ctrl+F10  hold other keys back while Ctrl is down, on or off
    Ctrl+F12  write every storage's size to the log

The same key closes. Another storage's key switches straight to it. Esc and B
close too. Keys only work in free play.

Holding Ctrl keeps other keys from the game, so a slip off a storage key does
not fire a skill. W, A, S, D and the arrows get through all the same, and so do
the keys the game uses with Ctrl itself. Holding Ctrl is Examine, which reads
Q, E, R and T, so talking and trading with an NPC on Ctrl+E and Ctrl+R still
work. Z is the guard's weapon swap, and Shift and + are the other two. Ctrl is also the game's guard and lock-on key, so any other key
pressed while guarding is held back. Ctrl+F10 turns it off and on and saves the
choice; HideKeysWithModifier and HideKeysToggleKey in the ini set the same thing.

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
PrivateStorageMaster.log from bin64. The two sessions before it are kept as
PrivateStorageMaster.01.log and .02.log, or the last 24 with DebugLog=1.

Building
--------

MSVC Build Tools 2022 with its bundled CMake and Ninja. Run mod\build.bat by
full path; the plugin lands in mod\dist. mod/src/game/addresses.cpp lists
every pattern the plugin searches for.
