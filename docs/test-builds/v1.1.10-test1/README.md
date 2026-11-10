# Private Storage Master 1.1.10-test1 (test build)

A test build of the fix for storage keys that stop working, not a release. It is not on Nexus.

From 24.86 days after Windows last restarted, every storage key and controller combo in 1.1.10 and earlier does nothing at all, and the log says nothing about it. The plugin decided whether its keys were paused with a 32-bit millisecond count since Windows started, and once that count passed 2^31 the check came out "paused" for good, until Windows restarted or reached 49.7 days. A player on 25.4 days of uptime hit it the same day a DMM update came out, so it looked like DMM's fault; the diagnostic build 1.1.10-diag1 showed what it was. This build uses the 64-bit count, which does not wrap.

Nothing else changed. It is 1.1.10 with that one fix, and it logs the same as 1.1.10.

## Download

- [PrivateStorageMaster-1.1.10-test1.zip](https://github.com/shin2344234/private-storage-master/raw/ccr-861c82c1-7hgyc0/docs/test-builds/v1.1.10-test1/PrivateStorageMaster-1.1.10-test1.zip), for installing by hand
- [PrivateStorageMaster-1.1.10-test1-DMM.zip](https://github.com/shin2344234/private-storage-master/raw/ccr-861c82c1-7hgyc0/docs/test-builds/v1.1.10-test1/PrivateStorageMaster-1.1.10-test1-DMM.zip), for DMM

## Installing

This build was cross-compiled with clang-cl and the MSVC runtime, not with the Visual Studio build the Nexus releases use, and it is not code-signed, so Windows SmartScreen or an antivirus may warn about it. The checksums at the bottom are of exactly the files linked here.

With the game closed, unzip `PrivateStorageMaster-1.1.10-test1.zip` and copy `PrivateStorageMaster.asi` into the game's `bin64` over the old one. With DMM, drag `PrivateStorageMaster-1.1.10-test1-DMM.zip` onto DMM, enable it and Mount instead. It replaces 1.1.10-diag1 too.

The `[mod]` line in `PrivateStorageMaster.log` should say `Private Storage Master 1.1.10-test1`.

## Testing it

Do not restart Windows first: the bug only shows on a PC that has been up more than 24.86 days, and a restart would hide it. (Task Manager, Performance, CPU shows "Up time".)

1. Start the game and load into the world.
2. Press Ctrl+F1. Private Storage should open. Press it again to close.
3. Try a few of the other storage keys, and LB+LS on a controller if you have one.
4. If a key still does nothing, quit and send `PrivateStorageMaster.log` from `bin64`, with `DebugLog=1` set in `PrivateStorageMaster.ini`, in the Private Storage Master channel on Discord.

## Checksums

    eb9e35c4a69862ae91102948e5b34615e68c374f71f5361c020affb66d4fd4db  PrivateStorageMaster-1.1.10-test1-DMM.zip
    3e506308b9b7c3fe9357de65ebb347573e3a4678813d7865daa37374d6324d15  PrivateStorageMaster-1.1.10-test1.zip
    4bc89ea5ebf31496eaf9006b7b8466d6ecf1f74462fd109f0339220856822850  PrivateStorageMaster.asi

Built from branch `ccr-861c82c1-7hgyc0`, commit with "Storage keys work again after 24.86 days of Windows uptime", using `mod/cmake/clang-cl-xwin.cmake` (clang-cl 19.1.1, lld-link, the MSVC CRT and Windows SDK 10.0.26100 from xwin), Release, `-DPSM_BUILD_TAG=-test1`.
