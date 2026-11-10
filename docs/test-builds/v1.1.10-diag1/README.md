# Private Storage Master 1.1.10-diag1 (diagnostic build)

A diagnostic build for one report, not a release. It is not on Nexus. It does nothing new for play; it only writes a much longer log.

Since a DMM update, on exe 1.0.0.3576, the storage keys and combos do nothing at all for one player. Their logs show the plugin starting cleanly every time: every address found, all four hooks in, `[mod] ready`. Then nothing, not even the line the first key press always writes. Every way the plugin can drop a key press is silent in a normal build, so those logs cannot say which one it is. This build says.

What it adds to the log:

- the game exe's real file version and build stamp, Windows' version, whether the game runs as administrator, and which process started it (Steam, DMM or something else)
- the ini exactly as the plugin read it (the DMM update had deleted it once)
- every module loaded that is not part of Windows, such as DmmRuntime.asi, other plugins, ASI loaders and overlays, with their versions, plus any that load or unload later
- every change of foreground window, with the program it belongs to
- every press of an F key, a number pad key or a bound key, read straight from the keyboard, with the modifiers held and what the plugin did with it: opened a storage, or ignored it and why (game not in front, input paused by another plugin, modifiers that do not match)
- which plugin calls PsmPauseInput, and how often
- a heartbeat every 5 seconds for two minutes, then every 15: frames reaching the plugin, play mode and screen, the window procedures, keyboard focus, raw input registrations, the controller state and the XInput filter
- whether each of the four hooks is still in place or was overwritten by another mod, and where that mod's jump goes
- a watchdog that says when the key poller gets stuck, and where
- faults inside the plugin or on its key poller thread

`DebugLog` is forced on, whatever the ini says.

## Download

- [PrivateStorageMaster-1.1.10-diag1.zip](https://github.com/shin2344234/private-storage-master/raw/ccr-861c82c1-7hgyc0/docs/test-builds/v1.1.10-diag1/PrivateStorageMaster-1.1.10-diag1.zip), for installing by hand
- [PrivateStorageMaster-1.1.10-diag1-DMM.zip](https://github.com/shin2344234/private-storage-master/raw/ccr-861c82c1-7hgyc0/docs/test-builds/v1.1.10-diag1/PrivateStorageMaster-1.1.10-diag1-DMM.zip), for DMM

## Installing

This build was cross-compiled with clang-cl and the MSVC runtime, not with the Visual Studio build the Nexus releases use, and it is not code-signed, so Windows SmartScreen or an antivirus may warn about it. The checksums at the bottom are of exactly the files linked here.

With the game closed, unzip `PrivateStorageMaster-1.1.10-diag1.zip` and copy `PrivateStorageMaster.asi` into the game's `bin64` over the old one. With DMM, drag `PrivateStorageMaster-1.1.10-diag1-DMM.zip` onto DMM, enable it and Mount instead.

The `[mod]` line near the top of `PrivateStorageMaster.log` should say `Private Storage Master 1.1.10-diag1`, and the next line `[diag] DIAGNOSTIC BUILD`. If it says 1.1.10 without `-diag1`, the old plugin is still the one loading.

## What to send

1. Start the game and load into the world.
2. Stand somewhere in free play, no menu open, for at least 30 seconds.
3. Press Ctrl+F1 (or whatever Private Storage is bound to) three times, a couple of seconds apart. If you have a controller, try LB+LS as well.
4. Alt+Tab out and back once.
5. Wait another 30 seconds, then quit the game normally.
6. Send `PrivateStorageMaster.log` from `bin64`, and `MasterLooter.log` if Master Looter is installed, in the Private Storage Master channel on Discord.

The log is long, around a hundred lines a minute and more while keys are pressed. That is expected. It names other programs only by their exe, never by their window titles, but the module list does show folder paths, which can include your Windows user name. Go back to 1.1.10 from Nexus once it has been sent, since this build is only for finding the cause.

## Checksums

    d6809e0b50300bf4e32f92bd1cc1aced4fc429d801675d83f2cb32a9c34e7ad4  PrivateStorageMaster-1.1.10-diag1-DMM.zip
    b0a33dbe0bfde947738ca8712805ec76d75d77e210f247c350ac62f81754ff96  PrivateStorageMaster-1.1.10-diag1.zip
    18362255e25f809d821f9a4636a19131cd3408f01abdb38237be14c0e0bed79d  PrivateStorageMaster.asi

Built from branch `ccr-861c82c1-7hgyc0` with `mod/cmake/clang-cl-xwin.cmake` (clang-cl 19.1.1, lld-link, the MSVC CRT and Windows SDK 10.0.26100 from xwin), `-DPSM_DIAG=ON`, Release.
