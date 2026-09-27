"""Hold the INI Master metadata up against the settings code that reads and
writes PrivateStorageMaster.ini.

mod/data/PrivateStorageMaster.inimeta is compiled into the plugin as the
INIMETA resource, and INI Master shows people what it says: the type of each
key, its default, its range and its help. None of that is read from the code.
A new storage, a changed default, a changed clamp: each has to be copied
across by hand, and a copy that is wrong does not fail anywhere. This is the
check that it was copied.

core/settings.h and core/settings.cpp are the authority:

  - kInfo in settings.cpp names the nine storages, their ini key prefix, the
    in-game label, the "what it is" comment and the default key and pad, and
    Defaults() reads that table. Every storage gets a {key}Key and a
    {key}Pad in the metadata with those defaults.
  - kFixedSlots in settings.h says which storages have no size setting
    (Collecting and Bird Feed): WriteIni never writes a {key}Slots for one of
    those, so the metadata must not describe one either.
  - kTownWarehouse says which storage never receives auto-stored loot:
    WriteIni skips its AutoStore{key}, so the metadata must too.
  - Values in settings.h gives the default of every scalar and array field,
    and Clamp() in settings.cpp gives the bounds the plugin actually accepts
    for slots, StackMultiplier and PrivateStorageExpansions.
  - Defaults() also holds the two literal key defaults that are not struct
    initialisers: CapacityDumpKey and HideKeysToggleKey.

Exits non-zero on any mismatch, so it can gate a build:

    py -3 scripts/check_inimeta.py
"""

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
META = os.path.join(ROOT, "mod", "data", "PrivateStorageMaster.inimeta")
SETTINGS_H = os.path.join(ROOT, "mod", "src", "core", "settings.h")
SETTINGS_CPP = os.path.join(ROOT, "mod", "src", "core", "settings.cpp")
VERSION_H = os.path.join(ROOT, "mod", "src", "version.h")


def read(p):
    with open(p, encoding="utf-8") as f:
        return f.read()


def load_meta():
    text = read(META)
    text = re.sub(r"^\s*//.*\n", "", text, flags=re.M)
    return json.loads(text)


def must(pattern, src, what, flags=0):
    m = re.search(pattern, src, flags)
    if not m:
        raise ValueError("could not find %s" % what)
    return m


def bool_default(expr):
    return "1" if expr.strip() == "true" else "0"


def parse_constants(h):
    """The kStorages-shaped constants settings.h defines once."""
    c = {}
    c["kStorages"] = int(must(r"kStorages = (\d+);", h, "kStorages").group(1))
    c["kMaxSlots"] = int(must(r"kMaxSlots = (\d+);", h, "kMaxSlots").group(1))
    fixed = must(r"kFixedSlots\[kStorages\]\s*=\s*\{([^}]*)\};", h, "kFixedSlots").group(1)
    c["kFixedSlots"] = [int(x.strip()) for x in fixed.split(",")]
    c["kTownWarehouse"] = int(must(r"kTownWarehouse = (\d+);", h, "kTownWarehouse").group(1))
    c["kNeverMoveMax"] = int(must(r"kNeverMoveMax = (\d+);", h, "kNeverMoveMax").group(1))
    c["kNeverMoveVersion"] = int(must(r"kNeverMoveVersion = (\d+);", h, "kNeverMoveVersion").group(1))
    c["kMaxStackMultiplier"] = int(must(r"kMaxStackMultiplier = (\d+);", h, "kMaxStackMultiplier").group(1))
    if len(c["kFixedSlots"]) != c["kStorages"]:
        raise ValueError("kFixedSlots has %d entries, kStorages says %d" % (len(c["kFixedSlots"]), c["kStorages"]))
    return c


def parse_kinfo(cpp, count):
    """[(key, label, comment, defKey, defPad), ...] in storage order."""
    body = must(r"const StorageInfo kInfo\[kStorages\]\s*=\s*\{(.*?)\n\s*\};", cpp, "kInfo", re.S).group(1)
    row_re = re.compile(
        r'\{\s*"([^"]*)",\s*"([^"]*)",\s*(?:"[^"]*"|nullptr),\s*"([^"]*)",\s*"([^"]*)",\s*"([^"]*)"\s*\}'
    )
    rows = row_re.findall(body)
    if len(rows) != count:
        raise ValueError("kInfo has %d rows, kStorages says %d" % (len(rows), count))
    return [
        {"key": key, "label": label, "comment": comment, "defKey": defKey, "defPad": defPad}
        for key, label, comment, defKey, defPad in rows
    ]


def parse_values_defaults(h):
    """Scalar and per-storage-array defaults straight from struct Values."""
    d = {}
    d["Enabled"] = bool_default(must(r"bool\s+enabled\s*=\s*(\w+);", h, "enabled").group(1))
    d["DebugLog"] = bool_default(must(r"bool\s+debugLog\s*=\s*(\w+);", h, "debugLog").group(1))
    d["HideKeysWithModifier"] = bool_default(must(r"bool\s+hideKeysWithModifier\s*=\s*(\w+);", h, "hideKeysWithModifier").group(1))
    d["LeaveCapacityAlone"] = bool_default(must(r"bool\s+leaveCapacityAlone\s*=\s*(\w+);", h, "leaveCapacityAlone").group(1))
    slots = must(r"int\s+slots\[kStorages\]\s*=\s*\{([^}]*)\};", h, "slots default").group(1)
    d["slots"] = [int(x.strip()) for x in slots.split(",")]
    d["PrivateStorageExpansions"] = must(r"privateStorageExpansions\s*=\s*(-?\d+);", h, "privateStorageExpansions").group(1)
    d["StackMultiplier"] = must(r"int\s+stackMultiplier\s*=\s*(\d+);", h, "stackMultiplier").group(1)
    d["AutoStore"] = bool_default(must(r"bool\s+autoStore\s*=\s*(\w+);", h, "autoStore").group(1))
    autoTo = must(r"bool\s+autoStoreTo\[kStorages\]\s*=\s*\{([^}]*)\};", h, "autoStoreTo default").group(1)
    d["autoStoreTo"] = [bool_default(x.strip()) for x in autoTo.split(",")]
    d["AutoStoreOnlyGained"] = bool_default(must(r"bool\s+autoStoreOnlyGained\s*=\s*(\w+);", h, "autoStoreOnlyGained").group(1))
    neverMove = must(r"uint16_t autoStoreNeverMove\[kNeverMoveMax\]\s*=\s*\{([^}]*)\};", h, "autoStoreNeverMove default").group(1)
    d["autoStoreNeverMove"] = [int(x.strip()) for x in neverMove.split(",")]
    return d


def parse_key_defaults(cpp):
    """The two key defaults Defaults() sets literally, not from Values."""
    d = {}
    d["CapacityDumpKey"] = must(r'ParseKey\("([^"]+)",\s*v\.dumpKey\)', cpp, "dumpKey default").group(1)
    d["HideKeysToggleKey"] = must(r'ParseKey\("([^"]+)",\s*v\.hideKeysToggleKey\)', cpp, "hideKeysToggleKey default").group(1)
    return d


def parse_clamp(cpp):
    """Bounds Clamp() actually enforces, which are what the plugin accepts."""
    body = must(r"void Clamp\(Values& v\)\s*\{(.*?)\n\s\s\s\s\}\n", cpp, "Clamp()", re.S).group(1)
    slots_min = int(must(r"if \(s < (-?\d+)\) s = ", body, "slots min").group(1))
    slots_max = must(r"if \(s > (\w+)\) s = ", body, "slots max").group(1)
    stack_min = int(must(r"if \(v\.stackMultiplier < (\d+)\) v\.stackMultiplier = ", body, "stackMultiplier min").group(1))
    stack_max = must(r"if \(v\.stackMultiplier > (\w+)\) v\.stackMultiplier = ", body, "stackMultiplier max").group(1)
    expand_min = int(must(r"if \(v\.privateStorageExpansions < (-?\d+)\) v\.privateStorageExpansions = ", body,
                           "privateStorageExpansions min").group(1))
    expand_max = must(r"if \(v\.privateStorageExpansions > (\w+)\) v\.privateStorageExpansions = ", body,
                       "privateStorageExpansions max").group(1)
    return {
        "slots_min": slots_min, "slots_max": slots_max,
        "stack_min": stack_min, "stack_max": stack_max,
        "expand_min": expand_min, "expand_max": expand_max,
    }


def resolve(name, const):
    return const[name] if name in const else int(name)


def compress_ranges(nums):
    """Mirror WriteIni's run-of-three-or-more-as-a-range compression."""
    out = []
    i = 0
    n = len(nums)
    while i < n:
        end = i
        while end + 1 < n and nums[end + 1] == nums[end] + 1:
            end += 1
        if end - i >= 2:
            out.append("%d-%d" % (nums[i], nums[end]))
        else:
            end = i
            out.append(str(nums[i]))
        i = end + 1
    return ",".join(out)


def ini_pad_default(defPad):
    return defPad if defPad else "None"


def main():
    h, cpp, ver = read(SETTINGS_H), read(SETTINGS_CPP), read(VERSION_H)
    const = parse_constants(h)
    storages = parse_kinfo(cpp, const["kStorages"])
    defaults = parse_values_defaults(h)
    key_defaults = parse_key_defaults(cpp)
    clamp = parse_clamp(cpp)
    meta = load_meta()
    errors = []

    ini_name = must(r'PSM_INI\s+L"([^"]+)"', ver, "PSM_INI").group(1)
    if meta.get("ini") != ini_name:
        errors.append("top-level ini: metadata says %r, version.h's PSM_INI says %r" % (meta.get("ini"), ini_name))
    if meta.get("live") is not False:
        errors.append("top-level live: should be false. The plugin has no ini watcher and only rereads "
                       "PrivateStorageMaster.ini at startup, so a hand edit never applies without a restart.")

    sections = meta.get("sections", {})
    main_keys = sections.get("PrivateStorageMaster", {}).get("keys", {})
    stacks_keys = sections.get("ItemStacks", {}).get("keys", {})
    autostore_keys = sections.get("AutoStore", {}).get("keys", {})

    def check(section_name, keys, key, checks):
        if key not in keys:
            errors.append("%s: %s is not in the metadata's %s section" % (key, "missing", section_name))
            return
        spec = keys[key]
        for field, want in checks.items():
            got = spec.get(field)
            if str(got) != str(want):
                errors.append("%s: %s is %r, the code says %r" % (key, field, got, want))

    # ---------------------------------------------------------------- scalars
    check("PrivateStorageMaster", main_keys, "Enabled", {"type": "bool", "default": defaults["Enabled"]})
    check("PrivateStorageMaster", main_keys, "DebugLog", {"type": "bool", "default": defaults["DebugLog"]})
    check("PrivateStorageMaster", main_keys, "HideKeysWithModifier",
          {"type": "bool", "default": defaults["HideKeysWithModifier"]})
    check("PrivateStorageMaster", main_keys, "LeaveCapacityAlone",
          {"type": "bool", "default": defaults["LeaveCapacityAlone"]})
    check("PrivateStorageMaster", main_keys, "CapacityDumpKey", {"type": "key", "default": key_defaults["CapacityDumpKey"]})
    check("PrivateStorageMaster", main_keys, "HideKeysToggleKey",
          {"type": "key", "default": key_defaults["HideKeysToggleKey"]})
    check("PrivateStorageMaster", main_keys, "PrivateStorageExpansions", {
        "type": "int",
        "default": defaults["PrivateStorageExpansions"],
        "min": clamp["expand_min"],
        "max": resolve(clamp["expand_max"], const),
    })
    check("ItemStacks", stacks_keys, "StackMultiplier", {
        "type": "int",
        "default": defaults["StackMultiplier"],
        "min": clamp["stack_min"],
        "max": resolve(clamp["stack_max"], const),
    })
    check("AutoStore", autostore_keys, "AutoStore", {"type": "bool", "default": defaults["AutoStore"]})
    check("AutoStore", autostore_keys, "AutoStoreOnlyGained", {"type": "bool", "default": defaults["AutoStoreOnlyGained"]})
    check("AutoStore", autostore_keys, "AutoStoreNeverMoveVersion", {
        "type": "int",
        "default": str(const["kNeverMoveVersion"]),
    })
    if not autostore_keys.get("AutoStoreNeverMoveVersion", {}).get("readonly"):
        errors.append("AutoStoreNeverMoveVersion: should be readonly. The comment above it in the written ini "
                       "says to leave it as it is.")

    want_never_move = compress_ranges(defaults["autoStoreNeverMove"])
    got_never_move = autostore_keys.get("AutoStoreNeverMove", {}).get("default")
    if got_never_move != want_never_move:
        errors.append("AutoStoreNeverMove: default %r, the code's default list writes as %r" %
                       (got_never_move, want_never_move))

    # ---------------------------------------------------------------- per storage
    town = const["kTownWarehouse"]
    for i, s in enumerate(storages):
        key, defKey, defPad = s["key"], s["defKey"], s["defPad"]
        check("PrivateStorageMaster", main_keys, key + "Key", {"type": "key", "default": defKey or "None"})
        check("PrivateStorageMaster", main_keys, key + "Pad", {"type": "string", "default": ini_pad_default(defPad)})

        slots_key = key + "Slots"
        if const["kFixedSlots"][i]:
            if slots_key in main_keys:
                errors.append("%s: storage %d's slot count is fixed at %d in the code and WriteIni never writes "
                               "%s, so the metadata must not describe it" % (slots_key, i, const["kFixedSlots"][i], slots_key))
        else:
            check("PrivateStorageMaster", main_keys, slots_key, {
                "type": "int",
                "default": str(defaults["slots"][i]),
                "min": clamp["slots_min"],
                "max": resolve(clamp["slots_max"], const),
            })

        auto_key = "AutoStore" + key
        if i == town:
            if auto_key in autostore_keys:
                errors.append("%s: WriteIni never writes an AutoStore key for the town warehouse (index %d), "
                               "so the metadata must not describe %s" % (auto_key, town, auto_key))
        else:
            check("AutoStore", autostore_keys, auto_key, {"type": "bool", "default": defaults["autoStoreTo"][i]})

    # ---------------------------------------------------------------- nothing extra
    expected_main = {"Enabled", "DebugLog", "HideKeysWithModifier", "LeaveCapacityAlone", "CapacityDumpKey",
                      "HideKeysToggleKey", "PrivateStorageExpansions"}
    for s, fixed in zip(storages, const["kFixedSlots"]):
        expected_main.add(s["key"] + "Key")
        expected_main.add(s["key"] + "Pad")
        if not fixed:
            expected_main.add(s["key"] + "Slots")
    for k in sorted(set(main_keys) - expected_main):
        errors.append("%s: in the metadata's PrivateStorageMaster section but not read by, or not written by, "
                       "the code" % k)

    expected_autostore = {"AutoStore", "AutoStoreOnlyGained", "AutoStoreNeverMove", "AutoStoreNeverMoveVersion"}
    for i, s in enumerate(storages):
        if i != town:
            expected_autostore.add("AutoStore" + s["key"])
    for k in sorted(set(autostore_keys) - expected_autostore):
        errors.append("%s: in the metadata's AutoStore section but not written by the code" % k)

    if set(stacks_keys) != {"StackMultiplier"}:
        for k in sorted(set(stacks_keys) - {"StackMultiplier"}):
            errors.append("%s: in the metadata's ItemStacks section but not written by the code" % k)

    for line in errors:
        print("error  " + line)
    total_keys = len(main_keys) + len(stacks_keys) + len(autostore_keys)
    print("%d keys in the metadata, %d storages, %d errors" % (total_keys, len(storages), len(errors)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
