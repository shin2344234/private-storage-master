# Private Storage Master

A Crimson Desert plugin that opens Private Storage, the housing chests, the
camp bins, Camp Provisions and Abyss gear storage from anywhere, in the game's
own warehouse screen, and sets each storage's size up to 1,460 slots. Built for
Crimson Desert 2.02.00.

It takes over from Private Storage Anywhere by Stevi2195 and PrivateStoragePlus
2.0 by jkiip. It was written from scratch and contains no code from either; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

- Download and full description: [Nexus Mods](https://www.nexusmods.com/crimsondesert/mods/PAGE_ID)
- Install, keys, settings and building: [mod/README.md](mod/README.md)
- For other plugins: [mod/include/psm_api.h](mod/include/psm_api.h) is the
  versioned C interface Master Looter's Storage tab uses.

## Antivirus

A scanner or two may flag the plugin, because the shape of what it does looks
like a trainer to a model: it is a DLL loaded into the game that searches the
game's code for byte patterns, writes jumps over five of its functions, swaps
the game's XInput import for a filter and reads the keyboard and pad first. It
imports only kernel32 and user32, so there is no network code in it, it touches
no registry key, game file or save, and every line is here to read or build
yourself.

SHA-256 for 1.0.0:

    009610ab0f423b8af96c074bc67c9c19733aa6142a55560be0431cb6b10d2e65  PrivateStorageMaster-1.0.0-DMM.zip
    a818e431dce9d36d93b4764a2dfd23ff95a300b2835778b8b6a009ddf3050844  PrivateStorageMaster-1.0.0.zip
    e022cdcef76dd4c33eb22f2c88995aee27084217643fa9e18e515e005fb13ac7  PrivateStorageMaster.asi

## Discord and Patreon

[Shin234's Mods 'n Stuff](https://discord.gg/AZ2ztQYy74) is the Discord, for
questions and for watching what is in progress. Bugs are best as GitHub issues
or on the Nexus bugs tab so they get tracked. Patreon is
[patreon.com/cw/Shin234](https://www.patreon.com/cw/Shin234). Everything
published stays free and nothing is held back for it.
