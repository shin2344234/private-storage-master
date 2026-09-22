Private Storage Master 1.1.2 for Crimson Desert 2.02.00 and 2.03.00
===================================================================

Opens storage from anywhere in the game's own warehouse screen, sets each
storage's size, raises how much one slot holds of an item, and with Master
Looter puts your loot straight into storage.

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

Loot straight into storage
--------------------------

New in 1.1.0. With Master Looter 1.6.28 or later installed as well, what
Master Looter picks up for you goes into your storage on its own: ore and
plants to the Gatherables Chest, food to the Kuku Cooler, collectibles to the
Collectibles Chest, Abyss gear to its storage. It is off until you turn it on.

To turn it on, open the Master Looter menu (Insert by default), go to the
Storage tab and tick "Put what Master Looter picks up into storage" under
Store loot. AutoStore=1 in PrivateStorageMaster.ini does the same.

Each item goes to the first storage in this list that is ticked and takes it:

    Collectibles Chest     one of each collectible, only when it has none yet
    Abyss gear storage
    Gatherables Chest
    Kuku Cooler
    Bird Feed
    Camp Straw             also takes teas and cooked food
    Wardrobe               off by default, so new gear stays with you
    Private Storage        off by default, since it takes almost anything

Camp Provisions holds trade goods only and never receives loot. The game's
own rules decide what each storage takes, and anything none of them takes
stays in your bag. A full storage is skipped and the next one is tried.

Only what was just picked up moves. If you carry 20 bread and Master Looter
picks up 5, the 5 go to the Kuku Cooler and your 20 stay. Untick "Only move
what was picked up" (AutoStoreOnlyGained=0) to move the whole stack.

Never moved:
- Money and every other currency: copper, silver, the pouches, gold bars,
  camp funds, tokens and the rest. The Never move list on the Storage tab
  (AutoStoreNeverMove in the ini) holds up to 64 items and you can add or
  remove any of them.
- Anything you pick up or gather by hand, buy, craft, or take out of a storage.
- Quest items and documents.
- Anything while a storage or menu is open, or in a cutscene or load, and for
  a moment after a menu closes.

Each time something is stored, a notice such as "Stored 3 items: Kuku Cooler
3" comes up. "Show a notice when loot is stored" on the same tab turns it off.

Needs: Master Looter 1.6.28 or later, this plugin 1.1.0 or later, and Master
Looter looting (auto-loot on, or its loot-everything key). Playing as Damiane
or Oongka, who carry Kliff's bag, needs Master Looter 1.6.29 and this plugin
1.1.1. With an older
Master Looter nothing happens; with an older Private Storage Master the tab
says "Storing loot needs a newer Private Storage Master."

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
    Ctrl+F10  hold F1 to F12 back while Ctrl is down, on or off
    Ctrl+F12  write every storage's size to the log

The same key closes. Another storage's key switches straight to it. Esc and B
close too. Keys only work in free play.

Holding Ctrl keeps F1 to F12 from the game, so a slip from one storage key to
the next F key goes nowhere. Every other key gets through, because Ctrl is also
the game's guard and Examine and the game uses the rest with it held: kicking
on F, talking and trading on E and R, gifts on G, the weapon swap on Z.
Alt+F4 always works. Ctrl+F10 turns it off and on and saves the choice;
HideKeysWithModifier and HideKeysToggleKey in the ini set the same thing.

Bigger stacks
-------------

New in 1.1.2. StackMultiplier in the [ItemStacks] section of the ini sets how
much of an item one slot holds, as a multiple of the game's own limit for that
item. 1, the default, changes nothing and the mod does not touch stacks at all.
5 turns a stack of 100 into 500 and a stack of 20 into 100, so the game's own
differences between item kinds stay.

    StackMultiplier=5

Editing the ini takes effect the next time the game starts. From Master Looter
1.6.33's Stacks tab, raising it takes hold straight away while you are in free
play with no storage open, and a smaller number waits for the next start.

No game file is changed: the game reads its item table as it starts and the mod
raises each item's own limit as that happens, which is why a game patch does not
break it.

Items the game does not stack, such as gear and quest items, never start
stacking. No stack goes past 999999, and an item the game already lets you hold
more of than that, like money, is left as it is. Replenishing Arrows, Bullets
and Cannonballs keep the game's own stack size.

Turning it back down does not shrink stacks you already built. A slot holding
more than the game allows keeps what is in it until you take some out, so empty
the big stacks before setting it back to 1.

Stack Master, the same feature as its own mod, can be used instead. With both
installed Stack Master is the one that applies, this mod leaves stacks alone and
says so in the log, and the multiplier to change is the one in StackMaster.ini.
Do not run a stack-size data mod such as Fat Stacks as well.

Sizes
-----

<Name>Slots in the ini sets a storage's size, up to 1460, from the next start.
0 keeps the game's size, and the mod never makes storage smaller than the game
would. PrivateStorageSlots is a total that includes bought expansions. The
Collectibles Chest is always 958. LeaveCapacityAlone=1 turns every size off,
for use with JSON capacity mods.

With Master Looter installed, its menu has a Storage tab for all of this,
loot storing included, and from 1.6.33 a Stacks tab for the multiplier above.

Reporting a problem
-------------------

Set DebugLog=1, play until it happens, close the game and attach
PrivateStorageMaster.log from bin64. For stacks the log says how many items were
raised and the biggest limit written, and names the first ten. The two sessions before it are kept as
PrivateStorageMaster.01.log and .02.log, or the last 24 with DebugLog=1. For
loot storing, attach MasterLooter.log too; with DebugLog=1 every item moved
or left in the bag is in PrivateStorageMaster.log with the reason.

Building
--------

MSVC Build Tools 2022 with its bundled CMake and Ninja. Run mod\build.bat by
full path; the plugin lands in mod\dist. mod/src/game/addresses.cpp lists
every pattern the plugin searches for.
