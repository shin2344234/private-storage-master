# Private Storage Master

A Crimson Desert plugin that opens Private Storage, the housing chests, the
camp bins, Camp Provisions and Abyss gear storage from anywhere, in the game's
own warehouse screen, and sets each storage's size up to 1,460 slots. Built for
Crimson Desert 2.02.00 to 2.03.02.

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

SHA-256 for 1.1.5:

    340940f755500ab78d5a8deef1c8e800043b459f1c14aea8eba6bd80e2f61187  PrivateStorageMaster-1.1.5-DMM.zip
    cb5b9d97c4b3032610fffe4172db58f3b2842bdf5f561ac27aa9649ef3319388  PrivateStorageMaster-1.1.5.zip
    a25aed5fe12eac0b007ec4bd6771b064b0b9485480bf1d2b56e896aeb4c2e446  PrivateStorageMaster.asi

## Discord and Patreon

[Shin234's Mods 'n Stuff](https://discord.gg/AZ2ztQYy74) is the Discord, for
questions and for watching what is in progress. Bugs are best as GitHub issues
or on the Nexus bugs tab so they get tracked. Patreon is
[patreon.com/cw/Shin234](https://www.patreon.com/cw/Shin234). Everything
published stays free and nothing is held back for it.
