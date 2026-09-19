# Private Storage Master

A Crimson Desert plugin that opens Private Storage, the housing chests, the
camp bins, Camp Provisions and Abyss gear storage from anywhere, in the game's
own warehouse screen, and sets each storage's size up to 1,460 slots. Built for
Crimson Desert 2.02.00 and 2.03.00.

New in 1.1.0: with [Master Looter](https://www.nexusmods.com/crimsondesert/mods/3402)
1.6.28 or later installed too, what Master Looter picks up goes straight into
your storage. Ore and plants go to the Gatherables Chest, food to the Kuku
Cooler, collectibles to the Collectibles Chest. Since 1.1.1, with Master Looter
1.6.29, it works as Damiane and Oongka too. Only what was just picked up
moves and money stays with you. It is off until you tick "Put what Master
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

SHA-256 for 1.1.1:

    f95678c3d29fef122d26e02549d1d3aba6fc608c7b6274e7971420b35700670b  PrivateStorageMaster-1.1.1-DMM.zip
    9b196041e9b17c3100d065833fa5580174c94e9fb2097af88fea1b2ab64c9216  PrivateStorageMaster-1.1.1.zip
    7fe333c8ef7400ac2effdbbaa631eedfd1ef0f7a682900bb4c822757a5e094bc  PrivateStorageMaster.asi

## Discord and Patreon

[Shin234's Mods 'n Stuff](https://discord.gg/AZ2ztQYy74) is the Discord, for
questions and for watching what is in progress. Bugs are best as GitHub issues
or on the Nexus bugs tab so they get tracked. Patreon is
[patreon.com/cw/Shin234](https://www.patreon.com/cw/Shin234). Everything
published stays free and nothing is held back for it.
