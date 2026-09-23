"""Post a release to the Discord server's mod-releases channel.

    py -3 announce-discord.py            print what would be posted
    py -3 announce-discord.py --apply    post it

Synced from release-kit. Do not edit it here; the next sync overwrites it.

Reads the version from the header named in release.json and posts
private/discord/release-<version>.txt as it is: a bold first line, two or three
plain lines saying what changed, then the Nexus files page and the GitHub
release. Write that file first and read it over. The script refuses without
it, refuses if either link is missing, and refuses if it still has a
{{PLACEHOLDER}}.

Posts as the bot, using DISCORD_BOT_TOKEN from the environment or from
keys.local.env beside this script. This is the only route for release
announcements in every mod. The Discord connector is not used for them,
because this script checks the links and writes the marker itself.

The channel is mod-releases. Its id comes from discord.channel in
release.json, and DISCORD_RELEASES_CHANNEL overrides it for a one-off.

Refuses to post a version twice: private/discord/announced-<version> is
written after a successful post and checked before the next one.
"""

import argparse
import io
import json
import os
import re
import sys
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import release_config as rc

LIMIT = 1900


def chunks(body):
    cur, out = "", []
    for p in body.split("\n\n"):
        piece = (cur + "\n\n" + p) if cur else p
        if len(piece) > LIMIT and cur:
            out.append(cur)
            cur = p
        else:
            cur = piece
    if cur:
        out.append(cur)
    return out


def post(token, channel, content, agent):
    url = "https://discord.com/api/v10/channels/%s/messages" % channel
    req = urllib.request.Request(url, data=json.dumps({"content": content}).encode("utf-8"),
                                 headers={"Content-Type": "application/json",
                                          "Authorization": "Bot " + token,
                                          "User-Agent": agent})
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.status


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true", help="post instead of printing")
    args = ap.parse_args()
    cfg = rc.load()
    ver = cfg.version
    nexus_files = cfg.nexus_files_url
    github_release = cfg.github_release_url(ver)
    if not nexus_files:
        print("nexus.page is empty in release.json. Create the Nexus page first, then set it.")
        return 3

    marker = cfg.path("private", "discord", "announced-%s" % ver)
    if os.path.exists(marker):
        print("%s was already announced (%s). Delete the marker to post again." % (ver, marker))
        return 1
    hand = cfg.path("private", "discord", "release-%s.txt" % ver)
    if not os.path.exists(hand):
        print("No %s. Write the announcement there, two or three plain lines and the links, then run this again." % hand)
        return 3
    body = io.open(hand, encoding="utf-8").read().strip()
    if re.search(r"\{\{[A-Z_]+\}\}", body):
        print("%s still has a {{PLACEHOLDER}}. Run release-docs.py stamp and finish the text." % hand)
        return 3
    if nexus_files not in body or github_release not in body:
        print("The announcement must carry both links:\n  %s\n  %s" % (nexus_files, github_release))
        return 3

    messages = chunks(body) if len(body) > LIMIT else [body]
    for i, m in enumerate(messages, 1):
        print("--- message %d of %d (%d chars) ---" % (i, len(messages), len(m)))
        print(m)
    if not args.apply:
        print("\nREPORT ONLY - nothing was sent. Re-run with --apply to post.")
        return 0

    token = rc.read_key("DISCORD_BOT_TOKEN")
    if not token:
        print("No DISCORD_BOT_TOKEN in the environment or keys.local.env.")
        return 2
    channel = rc.read_key("DISCORD_RELEASES_CHANNEL") or str(cfg.discord.get("channel", "")).strip()
    if not channel:
        print("discord.channel is empty in release.json.")
        return 3
    for m in messages:
        post(token, channel, m, "%s-announce" % cfg.file_base)
    os.makedirs(os.path.dirname(marker), exist_ok=True)
    io.open(marker, "w", encoding="utf-8").write("posted\n")
    print("\nPosted %d message(s) for %s." % (len(messages), ver))
    return 0


if __name__ == "__main__":
    sys.exit(main())
