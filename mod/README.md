# Private Storage Master

Opens Private Storage and the five housing chests in Crimson Desert 2.02.00
from anywhere, with a key or a controller button, in the game's own
warehouse screen.

Not released yet. This is Private Storage Anywhere 1.6.0 by Stevi2195 built
under a new name, and it behaves the same way.

## Install

1. Install an ASI loader in `bin64` next to `CrimsonDesert.exe`.
2. Copy `PrivateStorageMaster.asi` and `PrivateStorageMaster.ini` into `bin64`.
3. Play. With `DebugLog=1` the plugin writes `PrivateStorageMaster.log` next
   to itself.

Remove any copy of `PrivateStorageAnywhere.asi` first, so only one of them is
loaded. Remove the two files to uninstall.

## Keys

| Key | Opens |
|---|---|
| F4 | Private Storage |
| F5 | Gatherables |
| F6 | Dresser |
| F7 | Refrigerator |
| F8 | Symbol storage |
| F9 | Collecting storage |
| F11 | Reloads the ini |

Controller bindings, modifiers and the full key table are in the ini.

## Building

MSVC Build Tools 2022 with its bundled CMake and Ninja. Run `build.bat` from
the `mod` folder by full path; it stages `dist\PrivateStorageMaster.asi` and
the ini.
