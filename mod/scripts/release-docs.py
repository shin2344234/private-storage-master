"""Draft, stamp and check the release documents for the current version.

    py -3 release-docs.py new      create the four drafts from private/templates
    py -3 release-docs.py stamp    fill checksums in from private/checksums/<version>.json
    py -3 release-docs.py check    everything that must be true before publishing

Synced from release-kit. Do not edit it here; the next sync overwrites it.

The four documents every release writes:

    private/github/release-<version>.md          GitHub release notes, the full account
    private/nexus/nexus-changelog-<version>.txt  Nexus changelog, one line per change
    private/nexus/nexus-post-<version>.bbcode    Nexus update post
    private/discord/release-<version>.txt        Discord announcement

new never overwrites a file that exists. Template lines that start with TODO
are guidance, and check refuses while any are left.

stamp replaces {{SHA256_DMM}}, {{SHA256_MANUAL}} and {{SHA256_PLUGIN}}, and
rewrites any line that pairs a checksum with one of this mod's release file
names, in the four documents and in the files listed under evergreen in
release.json (the Nexus description and the root README by default). A file
that ships inside the manual archive is never stamped: its checksum is part of
the archive it would describe. VirusTotal links are left alone; they change in
the antivirus step, once the scores are in.

check refuses when a document is missing, a placeholder or TODO is left, the
checksums on record no longer match dist, the Discord text lacks a link, the
in-game verification record is missing, or prose_check.py fails.

The verification record is private/verified/<version>.txt. Its first line is
"tested" followed by the log lines that show the change working, or
"untested", in which case the GitHub notes and the Discord text must both
say the build was not tested in play.
"""
import hashlib
import io
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import release_config as rc

CFG = rc.load()
TEMPLATES = {
    "github": "github-release.md",
    "changelog": "nexus-changelog.txt",
    "post": "nexus-post.bbcode",
    "discord": "discord-release.txt",
}
UNTESTED = "not tested in play"
HEX = re.compile(r"\b[0-9a-f]{64}\b")
SHA_HEADING = re.compile(r"(SHA-256 for )\d+\.\d+\.\d+")


def read(path):
    return io.open(path, encoding="utf-8").read()


def write(path, text):
    d = os.path.dirname(path)
    if not os.path.isdir(d):
        os.makedirs(d)
    io.open(path, "w", encoding="utf-8", newline="").write(text)


def rel(path):
    return os.path.relpath(path, CFG.root)


def tokens():
    return {
        "NAME": CFG.name,
        "FILEBASE": CFG.file_base,
        "VERSION": CFG.version,
        "NEXUS_FILES_URL": CFG.nexus_files_url or "{{NEXUS_FILES_URL}}",
        "GITHUB_RELEASE_URL": CFG.github_release_url(),
        "GITHUB_REPO_URL": CFG.github_repo_url,
        "GITHUB_REPO": CFG.github,
    }


def fill(text, values):
    for k, v in values.items():
        text = text.replace("{{%s}}" % k, v)
    return text


def load_checksums():
    p = CFG.checksums_path()
    if not os.path.exists(p):
        return None
    data = json.loads(read(p))
    if data.get("version") != CFG.version:
        raise SystemExit("%s is for %s, not %s." % (rel(p), data.get("version"), CFG.version))
    return data["files"]


def sha_tokens(files):
    return {
        "SHA256_DMM": files["dmm"]["sha256"],
        "SHA256_MANUAL": files["manual"]["sha256"],
        "SHA256_PLUGIN": files["plugin"]["sha256"],
    }


def cmd_new():
    tdir = CFG.path("private", "templates")
    if not os.path.isdir(tdir):
        raise SystemExit("No %s. Run release-kit's sync.ps1 for this mod." % rel(tdir))
    values = tokens()
    files = load_checksums()
    if files:
        values.update(sha_tokens(files))
    for role, path in CFG.release_docs():
        if os.path.exists(path):
            print("exists   %s" % rel(path))
            continue
        write(path, fill(read(os.path.join(tdir, TEMPLATES[role])), values))
        print("created  %s" % rel(path))
    ver = CFG.path("private", "verified", "%s.txt" % CFG.version)
    if not os.path.exists(ver):
        print("\nStill needed: %s, first line 'tested' plus the log lines, or 'untested'." % rel(ver))
    return 0


def role_patterns():
    b = re.escape(CFG.file_base)
    return {
        "dmm": re.compile(r"%s-\d[\w.]*-DMM\.zip" % b),
        "manual": re.compile(r"%s-\d[\w.]*\.zip" % b),
        "plugin": re.compile(r"%s\.asi" % b),
    }


def line_role(line, pats):
    """The one release file a line names, or None when it names none or several."""
    found = set()
    for m in pats["dmm"].finditer(line):
        found.add(("dmm", m.group(0)))
    for m in pats["manual"].finditer(line):   # never matches a -DMM name: [\w.] has no hyphen
        found.add(("manual", m.group(0)))
    for m in pats["plugin"].finditer(line):
        found.add(("plugin", m.group(0)))
    roles = set(r for r, _ in found)
    return roles.pop() if len(roles) == 1 else None


def stamp_text(text, files, pats):
    text = fill(text, sha_tokens(files))
    out, touched, leftover = [], 0, []
    for line in text.splitlines(True):
        role = line_role(line, pats) if HEX.search(line) else None
        if role:
            new = HEX.sub(files[role]["sha256"], line)
            if role in ("dmm", "manual"):
                new = pats[role].sub(files[role]["name"], new)
            if new != line:
                touched += 1
            line = new
        elif HEX.search(line):
            leftover.append(line.strip()[:110])
        # The heading over the checksum block names the version.
        new = SHA_HEADING.sub(lambda m: m.group(1) + CFG.version, line)
        if new != line:
            touched += 1
            line = new
        out.append(line)
    return "".join(out), touched, leftover


def cmd_stamp():
    files = load_checksums()
    if not files:
        raise SystemExit("No %s. Run package.ps1 first." % rel(CFG.checksums_path()))
    pats = role_patterns()
    shipped = set(os.path.normcase(CFG.path(p)) for p in CFG.data.get("manualZip", []))
    evergreen = CFG.data.get("evergreen", ["private/nexus/nexus-description.bbcode", "README.md"])
    targets = [p for _, p in CFG.release_docs()] + [CFG.path(p) for p in evergreen]
    for path in targets:
        if not os.path.exists(path):
            print("missing  %s" % rel(path))
            continue
        if os.path.normcase(path) in shipped:
            print("skipped  %s ships in the manual archive, so it carries no checksums" % rel(path))
            continue
        before = read(path)
        after, touched, leftover = stamp_text(before, files, pats)
        if after != before:
            write(path, after)
        print("%-8s %s (%d lines changed)" % ("stamped" if after != before else "current", rel(path), touched))
        for l in leftover:
            print("         left alone, check it by hand: %s" % l)
    return 0


def rehash(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def version_tuple(v):
    return tuple(int(x) for x in re.findall(r"\d+", v)[:3])


def check_requires(problems):
    for req in CFG.data.get("requires", []):
        other = CFG.path(req["dir"])
        try:
            tags = subprocess.run(["git", "-C", other, "tag", "-l", "v*"], capture_output=True,
                                  text=True, check=True).stdout.split()
        except Exception as e:
            problems.append("could not read the tags of %s: %s" % (req["name"], e))
            continue
        released = [t[1:] for t in tags if re.match(r"^v\d+\.\d+\.\d+$", t)]
        if not any(version_tuple(t) >= version_tuple(req["minVersion"]) for t in released):
            problems.append("%s must be at %s or later before this ships (%s)"
                            % (req["name"], req["minVersion"], req.get("why", "see release.json")))


def cmd_check():
    problems, notes = [], []
    docs = CFG.release_docs()
    for role, path in docs:
        if not os.path.exists(path):
            problems.append("missing %s (release-docs.py new)" % rel(path))
            continue
        text = read(path)
        if re.search(r"\{\{[A-Z0-9_]+\}\}", text):
            problems.append("%s still has a {{PLACEHOLDER}} (release-docs.py stamp)" % rel(path))
        todo = [l for l in text.splitlines() if l.strip().startswith("TODO")]
        if todo:
            problems.append("%s still has %d TODO line(s)" % (rel(path), len(todo)))

    files = load_checksums()
    if not files:
        problems.append("no %s (package.ps1)" % rel(CFG.checksums_path()))
    else:
        for role, path in (("dmm", CFG.zip_dmm), ("manual", CFG.zip_manual), ("plugin", CFG.asi)):
            if not os.path.exists(path):
                problems.append("missing %s" % rel(path))
            elif rehash(path) != files[role]["sha256"]:
                problems.append("%s changed since package.ps1 recorded it; package and stamp again" % rel(path))
        # A file that goes into the manual archive and changed after packaging
        # means the archive on record is not what the repo now describes.
        if os.path.exists(CFG.zip_manual):
            built = os.path.getmtime(CFG.zip_manual)
            for p in CFG.data.get("manualZip", []):
                full = CFG.path(p)
                if os.path.exists(full) and os.path.getmtime(full) > built:
                    problems.append("%s changed after the archives were built; package and stamp again" % p)
        for role, path in docs[:2]:
            if os.path.exists(path):
                text = read(path)
                for r in ("dmm", "manual", "plugin"):
                    if files[r]["sha256"] not in text:
                        problems.append("%s lacks the %s checksum" % (rel(path), r))
        desc = CFG.path("private", "nexus", "nexus-description.bbcode")
        if os.path.exists(desc) and files["plugin"]["sha256"] not in read(desc):
            problems.append("%s lacks this release's checksums (release-docs.py stamp)" % rel(desc))

    discord = dict(docs)["discord"]
    if os.path.exists(discord):
        body = read(discord)
        for link in (CFG.nexus_files_url, CFG.github_release_url()):
            if link and link not in body:
                problems.append("%s lacks the link %s" % (rel(discord), link))

    vpath = CFG.path("private", "verified", "%s.txt" % CFG.version)
    if not os.path.exists(vpath):
        problems.append("no %s: first line 'tested' plus the log lines, or 'untested'" % rel(vpath))
    else:
        lines = [l for l in read(vpath).splitlines() if l.strip()]
        head = lines[0].strip().lower() if lines else ""
        if head == "tested":
            if len(lines) < 2:
                problems.append("%s says tested but quotes no log lines" % rel(vpath))
        elif head == "untested":
            for role in ("github", "discord"):
                p = dict(docs)[role]
                if os.path.exists(p) and UNTESTED not in read(p).lower():
                    problems.append("%s must say the build was %s" % (rel(p), UNTESTED))
            notes.append("this release goes out untested in play, and says so")
        else:
            problems.append("%s must start with 'tested' or 'untested'" % rel(vpath))

    check_requires(problems)

    checker = CFG.path("scripts", "prose_check.py")
    prose = [p for _, p in docs if os.path.exists(p)]
    desc = CFG.path("private", "nexus", "nexus-description.bbcode")
    if os.path.exists(desc):
        prose.append(desc)
    if not os.path.exists(checker):
        problems.append("no scripts/prose_check.py (sync.ps1)")
    elif prose:
        r = subprocess.run([sys.executable, checker] + prose, cwd=CFG.root)
        if r.returncode != 0:
            problems.append("prose_check.py fails; rewrite until it is clean")

    print()
    for n in notes:
        print("note     %s" % n)
    if problems:
        for p in problems:
            print("REFUSED  %s" % p)
        print("\n%s %s is not ready to publish." % (CFG.name, CFG.version))
        return 1
    print("%s %s is ready to publish." % (CFG.name, CFG.version))
    return 0


def main():
    cmds = {"new": cmd_new, "stamp": cmd_stamp, "check": cmd_check}
    if len(sys.argv) < 2 or sys.argv[1] not in cmds:
        print(__doc__)
        return 2
    return cmds[sys.argv[1]]()


if __name__ == "__main__":
    sys.exit(main())
