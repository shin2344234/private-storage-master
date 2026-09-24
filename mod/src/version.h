#pragma once

// Bump it with the release that goes with it, never on its own.
#define PSM_VERSION  "1.1.4"
#define PSM_NAME     "Private Storage Master"
// The game builds the plugin was last checked against: 2.02.00 (exe
// 1.0.0.2850) through 2.03.02 (1.0.0.2976, 23 September 2026), with 2.03.00
// (1.0.0.2944) and 2.03.01 between. Every anchor resolves on all of them, so the
// string names the range rather than the newest. It has to fit
// PsmStatus::gameVersion, which is char[16] and part of the shipped API, so keep
// it to 15 characters. The longer "2.02.00 and 2.03.00" silently came back from
// PsmGetStatus as "2.02.00 and 2.0".
#define PSM_GAME     "2.02.00-2.03.02"
// Base name of the plugin's files next to it: PrivateStorageMaster.asi, .ini, .log.
#define PSM_FILEBASE L"PrivateStorageMaster"
// The same as narrow text, for the module name another plugin looks us up by.
#define PSM_MODULE   "PrivateStorageMaster.asi"
#define PSM_INI      L"PrivateStorageMaster.ini"
