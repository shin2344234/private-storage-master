#pragma once

// Nothing has shipped under this name yet. Bump it with the release that goes
// with it, never on its own.
#define PSM_VERSION  "1.0.0"
#define PSM_NAME     "Private Storage Master"
// The game builds the plugin was last checked against. 2.03.00 is exe
// 1.0.0.2944, released 17 September 2026; 2.02.00 is 1.0.0.2850. Every anchor
// resolves on both, so the string names both rather than replacing one.
#define PSM_GAME     "2.02.00 and 2.03.00"
// Base name of the plugin's files next to it: PrivateStorageMaster.asi, .ini, .log.
#define PSM_FILEBASE L"PrivateStorageMaster"
#define PSM_INI      L"PrivateStorageMaster.ini"
