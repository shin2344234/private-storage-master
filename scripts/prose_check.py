"""Check prose for AI hallmarks before it goes out as Seth's words.

    py -3 scripts/prose_check.py private/nexus/nexus-post-1.6.3.txt
    py -3 scripts/prose_check.py --all

Exit code is 1 if anything in the HARD list is present. Those are mechanical
and there is never a reason to ship one.

The WARN list is heuristic. A hit is not proof, but every hit has to be looked
at and consciously kept, because these are the shapes that survive a clean
vocabulary sweep. Three times now the banned words were all absent and the
structure gave it away anyway.

What no script can check is printed at the end. Read it and do it by eye. The
rule in the project notes is "the grep is not the sweep", and this file is the grep.

Code blocks, bbcode [code] sections, checksum lines and URLs are stripped
before checking, because those are exempt and full of false positives.
"""
import io
import os
import re
import sys
import unicodedata

# ---------------------------------------------------------------- hard bans
DASHES = [
    ("—", "em dash"),
    ("– ", "spaced en dash"),
    (" –", "spaced en dash"),
]
QUOTES = [
    ("‘", "curly apostrophe"), ("’", "curly apostrophe"),
    ("“", "curly quote"), ("”", "curly quote"),
]

VOCAB = """delve leverage utilize harness foster unlock unleash elevate empower
streamline showcase underscore underscores spearhead embark bolster illuminate
unpack revolutionize boast boasts crucial pivotal vital robust seamless
seamlessly vibrant bustling meticulous meticulously intricate multifaceted
holistic invaluable transformative groundbreaking cutting-edge state-of-the-art
ever-evolving fast-paced commendable noteworthy profound nestled tapestry realm
beacon cornerstone testament synergy game-changer game-changing myriad plethora
""".split()

# Words with a real technical sense in this project. Flagged softly.
SOFT_VOCAB = "navigate landscape ecosystem journey dive deep-dive".split()

PHRASES = [
    "in today's fast-paced world", "in an era of", "have you ever wondered",
    "it's important to note", "it is important to note", "it's worth noting",
    "it is worth noting", "keep in mind", "needless to say", "at its core",
    "at the end of the day", "when it comes to", "that being said",
    "plays a crucial role", "plays a vital role", "plays a pivotal role",
    "plays a significant role", "a testament to", "underscores the importance",
    "leaves a lasting impact", "watershed moment", "key turning point",
    "deeply rooted", "in conclusion", "in summary", "in essence",
    "the bottom line", "but here's the thing", "but here's the kicker",
]

OPENERS = ("furthermore moreover additionally consequently notably "
           "importantly interestingly").split()

# My own tics, added as they get caught. Seth has asked four times now, so the
# ones that keep coming back stop being a judgement call and become a failure.
# Every one of these was written more than once in a single day.
#
# Matched against whitespace-collapsed text, because these span line breaks in a
# wrapped document and a pattern with a space in it silently misses otherwise.
# The first version of this list also had every \b turned into a literal
# backspace by a shell heredoc, so it matched nothing at all and reported a
# clean run. Build the patterns in a file, never in a heredoc.
TICS = [
    (r"which is what .{0,40} looks like", 'which is what X looks like'),
    (r"\bis the tell\b", 'is the tell'),
    (r"\bthe giveaway (?:was|is)\b", 'the giveaway was'),
    (r"\bi would rather\b", 'performed honesty: I would rather'),
    (r"\b(?:my own fault|my fault|i was wrong|i had it wrong)\b", 'narrating my own error'),
    (r"\bwhich is worse than\b", 'which is worse than'),
    (r"\brather than a theory\b", 'rather than a theory'),
    (r"\bthe whole (?:design|point|trick|of it)\b", 'the whole X'),
    (r"\bthat is the (?:whole|entire) \w+", 'that is the whole X'),
    (r"\bwhat actually happened\b", 'what actually happened'),
    (r"\bnot a theory\b", 'not a theory'),
    (r"\boff a log\b", 'off a log'),
    (r"\bworth (?:saying|having|knowing) (?:plainly|rather than)\b", 'worth saying plainly'),
    (r"\bhonest(?:ly)? (?:state|answer|position|about)\b", 'announcing my own honesty'),
    (r"\b(?:and )?it was mine\b", 'performed honesty: and it was mine'),
    (r"\bwas (?:the )?(?:mod page|this mod)'s fault\b", 'blaming the mod as a flourish'),
    (r"\bwas the clue\b", 'was the clue'),
    (r"\b(?:back )?through a different door\b", 'through a different door'),
    (r"\bnothing in .{0,30} was to blame\b", 'nothing was to blame'),
    (r"\bwrong on the (?:first|second|last) (?:point|count|half)\b", 'wrong on the second point'),
]

# The opening move of a reply, which is its own failure and was not checked
# until Sov1737 named it on 15 September 2026: "Gotta love how LLMs always lead
# with 'this is now solved', 'you did great', 'this is the fix'." He was reading
# a reply that opened "Your last message solved it." and the build under it was
# broken, so the verdict was wrong as well as unearned.
#
# These are matched against the FIRST sentence only. Telling a reporter they
# were right is fine in the body, where it comes after the evidence. Leading
# with it is the tell, and it is also where the claim is least likely to have
# been checked.
FIRST_SENTENCE = [
    (r"\b(?:solved|solves) it\b", "opening with a verdict"),
    (r"\bthat(?:'s| is) it\b", "opening with a verdict"),
    (r"\b(?:found|nailed|cracked) it\b", "opening with a verdict"),
    (r"\byou(?:'re| are|'ve| have)? (?:were )?(?:right|correct|spot on|onto)\b",
     "opening by telling the reporter they were right"),
    (r"\byou (?:nailed|called|spotted|caught)\b",
     "opening by telling the reporter they were right"),
    (r"\b(?:good|great|nice|excellent|brilliant|perfect) (?:catch|call|find|question|report|spot|work)\b",
     "opening with praise"),
    (r"\bthanks? (?:for|so much)\b", "opening with thanks"),
    (r"\bthis is (?:the|your) (?:fix|answer|cause|culprit)\b",
     "opening with a verdict"),
    (r"\b(?:now|already) (?:solved|fixed|sorted|resolved)\b",
     "opening with a verdict"),
    (r"\bgood news\b", "opening with a verdict"),
]

# Declaring a build good before anybody has played it. A changelog saying a
# thing is fixed is the genre working correctly, and the first version of this
# rule failed nine documents that were all right, so it is narrow now: only the
# forms that hand somebody an untested build and tell them it works.
#
# The equip probe of 14 September was introduced as the answer and was refused
# four lines later by a second copy of its own rule, so the reporter spent an
# evening measuring my code. Say what a build changes. Let them say whether it
# worked.
FIX_CLAIM = [
    (r"\b(?:this|that|here) is (?:the|your) (?:fix|solution)\b",
     "declaring a build good before it is played"),
    (r"\bproblem solved\b", "declaring a build good before it is played"),
    (r"\bthat should (?:do it|be it|sort it)\b",
     "declaring a build good before it is played"),
    (r"\b(?:this|it) (?:should|will) (?:fix|solve|sort) (?:it|this|that|the)\b",
     "declaring a build good before it is played"),
    (r"\byou (?:should|will) (?:now )?(?:see|get|find) (?:it|them) (?:work|pick)",
     "declaring a build good before it is played"),
]

# Praise as filler. Crediting a reporter by name is house style and stays; this
# is the chat-assistant reflex of grading their message before answering it.
PRAISE = [
    r"\b(?:exactly|absolutely|completely) right\b",
    r"\byou (?:nailed|absolutely nailed) (?:it|this)\b",
    r"\b(?:great|excellent|brilliant|perfect|fantastic) (?:report|catch|question|find|work|point)\b",
    r"\bthat(?:'s| is) (?:a )?(?:great|excellent|really good) (?:point|question|catch)\b",
]

# TICS and PHRASES are matched against text that has already been lowercased,
# so a capital letter inside one of these patterns makes the rule dead and
# silent. `\bI would rather\b` sat here unable to match anything until 15
# September 2026, when a reply went out carrying the very phrase it is for and
# this script passed it. One of twenty was dead and nothing said so.
#
# DeadRules() reports any pattern that cannot match its own lowercase form.
# --all runs it, so the next one announces itself.
def DeadRules():
    out = []
    for pat, why in TICS:
        bare = re.sub(r"\\[a-zA-Z]", "", pat)
        if any(ch.isupper() for ch in bare):
            out.append((pat, why))
    return out


# Saying "I got this wrong" once is candour. Five times in one document is a
# mannerism, and it reads as performance.
CONFESSION = r"\b(?:my own|my fault|I was wrong|I had assumed|I never checked|I should have|I failed)\b"

# Product and proper names that are capitalised legitimately, so the Title Case
# heading test does not fire on "With Definitive Mod Manager (DMM)".
PROPER = [
    "Definitive Mod Manager", "Crimson Desert", "Master Looter",
    "Private Storage Master", "Private Storage Anywhere", "Private Storage",
    "Ultimate ASI Loader", "Dear ImGui", "Nexus Mods", "Vortex",
    "Mod Organizer", "Direct3D", "DirectX", "Windows", "Steam", "GitHub",
    "VirusTotal", "Microsoft", "Symantec", "CrowdStrike Falcon",
    "Deep Instinct", "Take-or-Steal", "Damiane", "Oongka", "Kliff",
]

# ---------------------------------------------------------------- heuristics
NEG_PARALLEL = [
    r"\bnot just \w+[^.]{0,40}, (?:it's|it is|but)\b",
    r",\s*not (?:a|an|the|just|only)\b",
    r"\bit is not \w+[^.]{0,30}, it is\b",
    r"\bisn't \w+[^.]{0,30}, it's\b",
]
TRAILING_PARTICIPLE = (r",\s+(highlighting|ensuring|underscoring|reflecting|"
                       r"showcasing|emphasizing|demonstrating)\b")
SERVES_AS = r"\b(serves as|stands as|functions as)\b"
VAGUE_ATTRIB = r"\b(experts say|studies show|research shows)\b"

CLAUSE_VERB = (r"\b(is|are|was|were|has|have|does|do|can|never|carries|names|"
               r"walks|goes|reads|pays|drops|lands|gets|takes)\b")


def strip_exempt(text):
    """Drop what the rules exempt, so the checks only see prose."""
    text = re.sub(r"```.*?```", " ", text, flags=re.S)
    text = re.sub(r"\[code\].*?\[/code\]", " ", text, flags=re.S | re.I)
    text = re.sub(r"\[url=[^\]]*\]", " ", text, flags=re.I)
    text = re.sub(r"https?://\S+", " ", text)
    text = re.sub(r"^\s{4,}\S.*$", " ", text, flags=re.M)
    text = re.sub(r"^[0-9a-f]{64}\s+\S+$", " ", text, flags=re.M)
    text = re.sub(r"`[^`\n]*`", " ", text)
    # Quoted reporter text is exempt, and reporters write however they like.
    # Markdown blockquotes and bbcode [quote] both carry someone else's words.
    text = re.sub(r"^\s*>.*$", " ", text, flags=re.M)
    text = re.sub(r"\[quote.*?\[/quote\]", " ", text, flags=re.S | re.I)
    # A row of equals or dashes with a label in it separates one reply from the
    # next. It is not a sentence and it was being read as one document's
    # opening line.
    text = re.sub(r"^.*={6,}.*$", " ", text, flags=re.M)
    text = re.sub(r"^\s*[-=_*]{4,}\s*$", " ", text, flags=re.M)
    return text


def sentences(text):
    flat = re.sub(r"\s+", " ", text)
    return [s.strip() for s in re.split(r"(?<=[.?])\s+", flat) if s.strip()]


def check(path):
    raw = io.open(path, encoding="utf-8").read()
    body = strip_exempt(raw)
    low = body.lower()
    flat = re.sub(r"\s+", " ", low)
    hard, warn = [], []

    for ch, what in DASHES + QUOTES:
        if ch in body:
            hard.append("%s present" % what)
    for ch in body:
        if ord(ch) > 0x2100 and unicodedata.category(ch) == "So":
            hard.append("emoji or pictograph %r" % ch)
            break
    if "!" in re.sub(r"!\w", "", body):
        hard.append("exclamation point")

    for w in VOCAB:
        if re.search(r"\b%s\b" % re.escape(w), low):
            hard.append("banned word: %s" % w)
    for p in PHRASES:
        if p in flat:
            hard.append("stock phrase: %s" % p)
    for s in sentences(body):
        first = s.split(" ")[0].strip(",").lower()
        if first in OPENERS:
            hard.append("sentence opens with %s" % first.capitalize())

    for line in body.splitlines():
        m = re.match(r"^\s*(?:#{1,6}\s+|\[b\])(.+?)(?:\[/b\])?\s*$", line)
        if not m:
            continue
        head = m.group(1)
        for name in PROPER:
            head = head.replace(name, " ")
        words = [w for w in re.findall(r"[A-Za-z']+", head) if len(w) > 3]
        if len(words) >= 3 and sum(w[0].isupper() for w in words) > len(words) * 0.6:
            hard.append("Title Case heading: %s" % m.group(1)[:50])

    for pat, why in TICS:
        for m in re.finditer(pat, flat):
            hard.append("tic: %s -> ...%s..." % (why, m.group(0)[:60]))

    ss_all = [x for x in sentences(body)
              if len(re.findall(r"[A-Za-z]", x)) >= max(8, len(x) * 0.4)]
    if ss_all:
        opening = ss_all[0].lower()
        for pat, why in FIRST_SENTENCE:
            m = re.search(pat, opening)
            if m:
                hard.append("%s: %r" % (why, ss_all[0][:70]))
    for pat, why in FIX_CLAIM:
        for m in re.finditer(pat, flat):
            hard.append("%s -> ...%s..." % (why, m.group(0)[:60]))
    for pat in PRAISE:
        for m in re.finditer(pat, flat):
            warn.append("praise as filler: ...%s..." % m.group(0)[:60])

    n_conf = len(re.findall(CONFESSION, flat))
    if n_conf > 2:
        warn.append("%d self-corrections in one piece; once is honest, five is a mannerism" % n_conf)

    for w in SOFT_VOCAB:
        if re.search(r"\b%s\b" % re.escape(w), low):
            warn.append("soft word, fine if literal: %s" % w)
    for pat in NEG_PARALLEL:
        for m in re.finditer(pat, low):
            warn.append("negative parallelism: ...%s..." % m.group(0)[:60])
    for m in re.finditer(TRAILING_PARTICIPLE, low):
        warn.append("trailing participle: %s" % m.group(0).strip())
    for m in re.finditer(SERVES_AS, low):
        warn.append("%s instead of is" % m.group(0))
    for m in re.finditer(VAGUE_ATTRIB, low):
        warn.append("vague attribution: %s" % m.group(0))

    n = len(re.findall(r"\brather than\b", low))
    if n > 1:
        warn.append('"rather than" %d times, check it is contrast not habit' % n)

    for s in sentences(body):
        parts = [p for p in s.split(",") if p.strip()]
        if len(parts) >= 3 and re.search(r",\s*and\b", s):
            if sum(bool(re.search(CLAUSE_VERB, p)) for p in parts) >= 3:
                warn.append("possible triad: %s" % s[:90])

    ss = sentences(body)
    for a, b in zip(ss, ss[1:]):
        if abs(len(a) - len(b)) <= 4 and len(a) > 45:
            warn.append("same-shape pair: %r / %r" % (a[:45], b[:45]))

    return hard, warn


# Files the repeated-sentence rule does not apply to.
#
# The rule exists because the same person reads a release's post and its
# changelog back to back, so a sentence in both reads as copy and paste. That
# reasoning does not reach these:
#
#   nexus-changelog.txt is a deliberate verbatim copy of the current release's
#   changelog, so every sentence in it is a duplicate by construction.
#
#   README.md, mod/README.md, the mod page description and the short
#   description are long-lived reference documents about one mod. They are
#   supposed to say the same things, and thirteen shared sentences between the
#   README and the description are thirteen places that are correctly in step,
#   not thirteen mistakes. Flagging them trained me to skim the output, which
#   is how the check stops working.
SKIP_DUP = {
    "nexus-changelog.txt",
    "README.md",
    "nexus-description.bbcode",
    "nexus-short-description.txt",
}


def release_of(path):
    """The x.y.z in a filename, so only documents of one release are compared."""
    m = re.search(r"(\d+\.\d+\.\d+)", os.path.basename(path))
    return m.group(1) if m else ""


def duplicates(paths):
    """A sentence used in two documents of the same release.

    Readers of a Nexus release see the post and the changelog together, so a
    sentence in both reads as copy and paste. Three constraints keep this from
    crying wolf, which would get the whole check ignored:

      - nexus-changelog.txt is skipped. It is a deliberate verbatim copy of the
        current release's changelog, so every sentence in it is a duplicate.
      - Only documents of the same release are compared. The 1.2.0 and the
        1.6.3 changelogs sharing a line is not copy and paste.
      - A sentence in three or more files is house boilerplate, not laziness.
        "For Crimson Desert 2.02.00 (exe 1.0.0.2850)." heads every changelog.
    """
    paths = [p for p in paths if os.path.basename(p) not in SKIP_DUP]
    everywhere = {}
    per_release = {}
    for p in paths:
        rel = release_of(p)
        for s in sentences(strip_exempt(io.open(p, encoding="utf-8").read())):
            if len(s) < 30:
                continue
            everywhere.setdefault(s, set()).add(os.path.basename(p))
            per_release.setdefault((rel, s), set()).add(os.path.basename(p))
    out = {}
    for (rel, s), files in per_release.items():
        if len(files) > 1 and len(everywhere[s]) < 3:
            out[s] = files
    return out


BY_EYE = """
Now the part no script can do. Reread the whole piece and answer these:

  1. Does the opening line say something, or does it announce the shape of the
     piece? Cut it if it is announcing. The verdict openers are mechanical now;
     this question is about the ones no list has caught yet.
  2. Does the last paragraph add anything, or restate and then moralise? End on
     what the reader does next.
  3. Read the previous two documents of this kind. Does this one share their
     skeleton with the labels swapped?
  4. Any move that is becoming a signature? Performed honesty, the
     self-deprecating fix introduction, the same joke shape twice.
  5. Read it aloud. Two sentences in a row with the same rhythm get rewritten.
"""


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if "--all" in sys.argv or not args:
        # Everything that goes out in Seth's name: the two READMEs, the
        # notices, and the release pieces under private.
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        args = [os.path.join(root, f) for f in
                ("README.md", os.path.join("mod", "README.md"), "THIRD_PARTY_NOTICES.md")
                if os.path.exists(os.path.join(root, f))]
        for sub in ("nexus", "github", "discord"):
            d = os.path.join(root, "private", sub)
            if os.path.isdir(d):
                args += [os.path.join(d, f) for f in sorted(os.listdir(d))
                         if f.endswith((".txt", ".bbcode", ".md")) and "template" not in f]
    for pat, why in DeadRules():
        print("  BROKEN RULE  %s can never match, the text is lowercased first (%s)"
              % (pat, why))
    failed = False
    for p in args:
        hard, warn = check(p)
        if hard or warn:
            print("=== %s ===" % os.path.basename(p))
        for h in sorted(set(hard)):
            print("  HARD  %s" % h)
            failed = True
        for w in sorted(set(warn)):
            print("  warn  %s" % w)
        if hard or warn:
            print()
    if len(args) > 1:
        dup = duplicates(args)
        if dup:
            print("=== repeated across documents ===")
            for s, f in sorted(dup.items()):
                print("  HARD  %s" % ", ".join(sorted(f)))
                print("        %s" % s[:100])
                failed = True
            print()
    print(BY_EYE)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
