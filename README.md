# Private Storage Master

A Crimson Desert plugin that opens Private Storage, the housing chests, the
camp bins, Camp Provisions and Abyss gear storage from anywhere, in the game's
own warehouse screen, and sets each storage's size up to 1,460 slots. Built for
the Crimson Desert DLC update, exe 1.0.0.3392 through 1.0.0.3663 (2.05.00).
For 2.02.00 to 2.03.02, use 1.1.7.

New in 1.1.12: the plugin runs on 2.05.00, exe 1.0.0.3663. That update moved
the item move, the bag lookup and the table that lists every inventory, so
1.1.11 switched loot storing off and Master Looter's deposits never left the
bag. The plugin now reads where that table is from the game's own code, and
the storage keys work alongside Camp Funds Withdraw on 3663 as well.

New in 1.1.9: the storage keys work alongside Camp Funds Withdraw. That mod
points one entry of the storage screen's function table into its own DLL, and
the plugin stopped reading the table there, so it never found the screen's
handler and switched every storage key off. It now reads past entries another
mod has taken over. majahkil reported it.

New in 1.1.8: the plugin runs on the DLC update. That update reordered the
storage and item records and moved the chest screen, so 1.1.7 opened nothing,
set no sizes and stored no loot on it. A storage on the new build also starts
with no slots and gets 100 at a time as items go in, which the first port read
as a limit of 100. Sizes go by the game's 1,460 ceiling again, and loot storing
fills a storage that is still empty.

New in 1.1.2: `StackMultiplier` raises how much of an item one slot holds, as a
multiple of the game's own limit for that item, which is the Fat Stacks idea done
without replacing any game data. It is 1, off, until you change it. The same
feature ships as its own mod, [Stack Master](https://www.nexusmods.com/crimsondesert/mods/3548),
for people who want bigger stacks and not the storage mod; with both installed
Stack Master applies and this one stands down. Since 1.1.3, Replenishing
Arrows, Bullets and Cannonballs keep the game's own stack size.

New in 1.1.0: with [Master Looter](https://www.nexusmods.com/crimsondesert/mods/3402)
1.6.28 or later installed too, what Master Looter picks up goes straight into
your storage. Ore and plants go to the Gatherables Chest, food to the Kuku
Cooler, collectibles to the Collectibles Chest. Since 1.1.1, with Master Looter
1.6.29, it works as Damiane and Oongka too. Only what was just picked up
moves and money stays with you. Since 1.1.4, so do arrows you pick back up. It is off until you tick "Put what Master
Looter picks up into storage" on Master Looter's Storage tab. How it works and what it
needs is in [mod/README.md](mod/README.md#loot-straight-into-storage).

New in 1.1.7: a cutscene, dialog, minigame or fade that starts while a storage is
open closes the storage, where it used to leave the chest menu on screen with Esc
and the storage keys doing nothing. When the storage keys are off, the log and
Master Looter's Last error now name what the plugin could not find. INI Master
shows every setting with its range and help, and no longer offers the retired
Bird Feed size.

A housing chest left at a size of 0 has the game's 10 slots, and the game adds
your furniture's slots to them. A report saw those extra slots only inside the
house, so a chest opened from far away can show 10. Give the chest a size above
10 and it has that many anywhere. [mod/README.md](mod/README.md) has more under
Sizes.

It takes over from Private Storage Anywhere by Stevi2195 and PrivateStoragePlus
2.0 by jkiip. It was written from scratch and contains no code from either; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

- Download and full description: [Nexus Mods](https://www.nexusmods.com/crimsondesert/mods/3521)
- Install, keys, settings and building: [mod/README.md](mod/README.md)
- For other plugins: [mod/include/psm_api.h](mod/include/psm_api.h) is the
  versioned C interface Master Looter's Storage tab uses, including
  PsmDeposit, PsmDepositResults and PsmFreePlay for loot storing.
  [mod/include/stack_api.h](mod/include/stack_api.h) is the separate interface
  for stack sizes, which Stack Master exports under the same names, so a caller
  works with whichever of the two is installed.

## Antivirus

A scanner or two may flag the plugin, because the shape of what it does looks
like a trainer to a model: it is a DLL loaded into the game that searches the
game's code for byte patterns, writes jumps over five of its functions, swaps
the game's XInput import for a filter and reads the keyboard and pad first. It
imports only kernel32 and user32, so there is no network code in it, and it
touches no registry key or game file. It never edits your save; with loot
storing on it asks the game to move items the way the warehouse screen does,
and the game saves that as it would any move. Every line is here to read or
build yourself.

SHA-256 for 1.1.12:

    9b9e67b8b725f218ce7e55808909720a81ef29e55bc2ecb1e80d465a0b3e4c1a  PrivateStorageMaster-1.1.12-DMM.zip
    8f37a42c9e07aeb8091780e716bf6a7c88f5a0d36adbfb14039bed5b2e66c133  PrivateStorageMaster-1.1.12.zip
    d39ab1b6a958a8e0b9c6fe27cbdb60ea184e293c0ddd015573e36221f1da9a7f  PrivateStorageMaster.asi

## Discord and Patreon

[Shin234's Mods 'n Stuff](https://discord.gg/AZ2ztQYy74) is the Discord, for
questions and for watching what is in progress. Bugs are best as GitHub issues
or on the Nexus bugs tab so they get tracked. Patreon is
[patreon.com/cw/Shin234](https://www.patreon.com/cw/Shin234). Everything
published stays free and nothing is held back for it.
