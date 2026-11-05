#!/usr/bin/env python3
"""Work with the game's text.

usage: tools/script.py dump [--dir FOLDER] [--out text-dump/ja]
       tools/script.py outline <zen|chu|kou> [--dir FOLDER]
       tools/script.py worksheet --lang en
       tools/script.py merge --lang en <worksheet.tsv>
       tools/script.py status [--lang en]
       tools/script.py lint --lang en
       tools/script.py tm-export | tm-check <out_NN.tsv> | tm-apply --lang en

`dump` writes the Japanese source as TSV from the player's own ROMs. The result
is ROM data, so it stays out of git: anyone can regenerate it in seconds.

`worksheet` puts the Japanese and a language's text side by side in text-dump/, for a
spreadsheet or a text editor. `merge` writes the edited sheet back to translation/.
Only the translation is ever committed. Each row keeps a short hash of the Japanese
it was made from, so `status` can tell when a row has gone stale.

The `tm-` commands translate each unique string once and apply it to every row with
the same Japanese. They work in text-dump/tm, which is local because it holds Japanese.
"""
import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from pvs import script as sc  # noqa: E402
from pvs.roms import INPUTS, load_pack  # noqa: E402

EPISODES = ("zen", "chu", "kou")
ROOT = pathlib.Path(__file__).resolve().parent.parent
TRANSLATION_DIR = ROOT / "translation"
FONT_PATH = ROOT / "assets" / "fonts" / "latin.txt"
NAME_PLACEHOLDER = "Shinnosuke: "  # long enough to fit any speaker's name


def clock(seconds):
    return f"{seconds // 60}:{seconds % 60:02d}"


def outline(data):
    for number, (addr, start) in enumerate(sc.schedule(data)):
        print(f"scene {number} at ${addr:06X}" + (f" starts {clock(start)}" if start is not None else ""))
        start = start or 0
        for item in sc.walk(data, addr):
            if not isinstance(item, sc.Op):
                continue
            if item.op == sc.OP_DEADLINE:
                print(f"  ${item.addr:06X} deadline {clock(start + sc.u16(item.args, 0))}")
            elif item.op in (sc.OP_EVENT, sc.OP_NEXT_EVENT):
                print(f"  ${item.addr:06X} event {clock(start + sc.u16(item.args, 0))}")
            elif item.op == sc.OP_MENU:
                print(f"  ${item.addr:06X} menu of {item.args[0]}")
            elif item.op == sc.OP_SCENE:
                print(f"  ${item.addr:06X} go to scene {item.args[0]}")


def read_source(episode, script_dir="text-dump/ja"):
    path = pathlib.Path(script_dir) / f"{episode}.tsv"
    if not path.exists():
        sys.exit(f"{path} not found. Run: tools/script.py dump")
    rows = []
    for line in path.read_text(encoding="utf-8").split("\n")[1:]:
        if line:
            key, scene, speaker, kind, text = line.split("\t")
            rows.append({"key": key, "scene": scene, "speaker": speaker, "kind": kind, "text": text})
    return rows


def translation_path(lang, episode):
    return TRANSLATION_DIR / lang / f"{episode}.tsv"


def read_translation(lang, episode):
    """Return {key: row} for a language. Rows hold status, text, hash and note."""
    path = translation_path(lang, episode)
    rows = {}
    if not path.exists():
        return rows
    for line in path.read_text(encoding="utf-8").splitlines()[1:]:
        parts = line.split("\t") + [""] * 4
        rows[parts[0]] = {"status": parts[1], "text": parts[2], "hash": parts[3], "note": parts[4]}
    return rows


def write_translation(lang, episode, rows):
    """Rows are in key order, so a re-merge makes a small diff."""
    path = translation_path(lang, episode)
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("key\tstatus\ttext\thash\tnote\n")
        for key in sorted(rows):
            row = rows[key]
            f.write(f"{key}\t{row['status']}\t{row['text']}\t{row['hash']}\t{row['note']}\n")


def worksheet(lang, out):
    path = pathlib.Path(out or f"text-dump/worksheet_{lang}.tsv")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("key\tscene\tspeaker\tkind\tjapanese\ttarget\tstatus\tnote\n")
        for episode in EPISODES:
            done = read_translation(lang, episode)
            for row in read_source(episode):
                t = done.get(row["key"], {})
                f.write("\t".join([row["key"], row["scene"], row["speaker"], row["kind"], row["text"],
                                   t.get("text", ""), t.get("status", ""), t.get("note", "")]) + "\n")
    print(f"wrote {path}")


def merge(lang, sheet):
    sources = {e: {r["key"]: r for r in read_source(e)} for e in EPISODES}
    merged = {e: read_translation(lang, e) for e in EPISODES}
    count = 0
    for line in pathlib.Path(sheet).read_text(encoding="utf-8").splitlines()[1:]:
        parts = line.split("\t") + [""] * 8
        key, target, status, note = parts[0], parts[5], parts[6], parts[7]
        episode = key.split(":")[0]
        if episode not in sources or key not in sources[episode]:
            sys.exit(f"{key}: not a row in the source")
        if not target and key not in merged[episode]:
            continue
        merged[episode][key] = {"status": status or "draft", "text": target,
                                "hash": sc.source_hash(sources[episode][key]["text"]), "note": note}
        count += 1
    for episode in EPISODES:
        if merged[episode]:
            write_translation(lang, episode, merged[episode])
    print(f"merged {count} rows")


def status(lang):
    languages = [lang] if lang else sorted(p.name for p in TRANSLATION_DIR.iterdir()
                                          if p.is_dir() and not p.name.startswith("_"))
    for language in languages:
        for episode in EPISODES:
            done = read_translation(language, episode)
            source = read_source(episode)
            translated = [r for r in source if done.get(r["key"], {}).get("text")]
            stale = [r for r in translated if done[r["key"]]["hash"] != sc.source_hash(r["text"])]
            review = [r for r in translated if done[r["key"]]["status"] == "review"]
            print(f"{language} {episode}: {len(translated)}/{len(source)} rows, "
                  f"{len(review)} to review, {len(stale)} stale")


def lint(lang):
    font = sc.load_font(FONT_PATH)
    problems = 0
    for episode in EPISODES:
        done = read_translation(lang, episode)
        source = {r["key"]: r for r in read_source(episode)}
        for key, row in done.items():
            if key not in source:
                print(f"{key}: not in the source")
                problems += 1
                continue
            jp, text = source[key], row["text"]
            if not text:
                continue
            if sc.kept_tags(jp["text"]) != sc.kept_tags(text):
                print(f"{key}: tags differ. Should be {' '.join(sc.kept_tags(jp['text']))}")
                problems += 1
            if jp["kind"] == "screen":
                source_size = len(jp["text"].replace("{n}", "\n").encode("shift_jis", "replace"))
                size = len(text.replace("{n}", "\n").encode("ascii", "replace"))
                if size > source_size:
                    print(f"{key}: {size} bytes, the screen string has {source_size}: {text}")
                    problems += 1
            elif jp["kind"] == "menu" and len(text.encode("ascii", "replace")) > sc.MENU_LABEL_BYTES:
                print(f"{key}: menu label over {sc.MENU_LABEL_BYTES} characters: {text}")
                problems += 1
            elif jp["kind"] == "text":
                name = NAME_PLACEHOLDER if jp["speaker"] else ""
                rows, missing = sc.layout_rows(name + text, font)
                if rows > sc.WINDOW_ROWS:
                    print(f"{key}: takes {rows} rows, the window has {sc.WINDOW_ROWS}: {text}")
                    problems += 1
                if missing:
                    print(f"{key}: no glyph for {''.join(sorted(missing))}")
                    problems += 1
    print(f"{lang}: {problems} problems")
    return problems


TM_DIR = ROOT / "text-dump" / "tm"
TM_KINDS = ("text", "menu")
STRAIGHT_QUOTES = str.maketrans({"\u201c": '"', "\u201d": '"', "\u2018": "'", "\u2019": "'"})


def tm_rows():
    """Each unique string to translate, first appearance first, with where it occurs."""
    unique = {}
    for episode in EPISODES:
        for row in read_source(episode):
            if row["kind"] in TM_KINDS and row["text"].strip("\u3000 "):
                unique.setdefault(row["text"], {**row, "episode": episode})
    return list(unique.values())


def tm_export(batch_chars):
    """Write batches of unique strings for translation, split at scene boundaries."""
    TM_DIR.mkdir(parents=True, exist_ok=True)
    batches, current, size = [], [], 0
    previous_scene = None
    for row in tm_rows():
        scene = (row["episode"], row["scene"])
        if size >= batch_chars and scene != previous_scene:
            batches.append(current)
            current, size = [], 0
        current.append(row)
        size += len(row["text"])
        previous_scene = scene
    batches.append(current)
    for number, rows in enumerate(batches):
        with open(TM_DIR / f"in_{number:02d}.tsv", "w", encoding="utf-8", newline="\n") as f:
            f.write("id\tpart\tscene\tspeaker\tkind\tjapanese\n")
            for row in rows:
                f.write(f"{sc.source_hash(row['text'])}{row['key'][-6:]}\t{row['episode']}\t{row['scene']}\t"
                        f"{row['speaker']}\t{row['kind']}\t{row['text']}\n")
        print(f"in_{number:02d}.tsv: {len(rows)} strings, {rows[0]['episode']} scene {rows[0]['scene']} on")


def tm_problems(row, text, font):
    """Why a translation won't work in the game, if it won't."""
    problems = []
    if sc.kept_tags(row["text"]) != sc.kept_tags(text):
        problems.append(f"tags should be, in order: {' '.join(sc.kept_tags(row['text'])) or 'none'}")
    if row["kind"] == "menu":
        if len(text) > sc.MENU_LABEL_BYTES:
            problems.append(f"menu labels fit {sc.MENU_LABEL_BYTES} characters, this has {len(text)}")
    else:
        rows, missing = sc.layout_rows((NAME_PLACEHOLDER if row["speaker"] else "") + text, font)
        if rows > sc.WINDOW_ROWS:
            problems.append(f"takes {rows} rows of text, the window has {sc.WINDOW_ROWS}")
        if missing:
            problems.append(f"no glyph for: {''.join(sorted(missing))}")
    return problems


def tm_check(path):
    """Check a batch's output: every id present once, and everything fits."""
    font = sc.load_font(FONT_PATH)
    path = pathlib.Path(path)
    source = {}
    for line in (path.parent / path.name.replace("out_", "in_")).read_text(encoding="utf-8").splitlines()[1:]:
        fields = line.split("\t")
        source[fields[0]] = {"speaker": fields[3], "kind": fields[4], "text": fields[5]}
    seen, bad = set(), 0
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        fields = line.split("\t")
        if len(fields) < 2:
            print(f"bad line, needs id<TAB>english: {line}")
            bad += 1
            continue
        if fields[0] not in source:
            print(f"{fields[0]}: not in this batch")
            bad += 1
            continue
        seen.add(fields[0])
        for problem in tm_problems(source[fields[0]], fields[1], font):
            print(f"{fields[0]}: {problem}: {fields[1]}")
            bad += 1
    missing = sorted(set(source) - seen)
    for key in missing[:20]:
        print(f"{key}: missing")
    print(f"{path.name}: {len(seen)}/{len(source)} strings, {bad} problems, {len(missing)} missing")
    return bad + len(missing)


def tm_apply(lang):
    """Apply every batch's output to all the rows with that Japanese, then write them."""
    by_source, count = {}, 0
    for path in sorted(TM_DIR.glob("out_*.tsv")):
        source = {}
        for line in (TM_DIR / path.name.replace("out_", "in_")).read_text(encoding="utf-8").splitlines()[1:]:
            fields = line.split("\t")
            source[fields[0]] = fields[5]
        for line in path.read_text(encoding="utf-8").splitlines():
            fields = line.split("\t")
            if len(fields) >= 2 and fields[0] in source:
                note = fields[3] if len(fields) > 3 else ""
                if not note.isascii():
                    sys.exit(f"{path.name}: {fields[0]}: notes get committed, so no Japanese in them: {note}")
                by_source[source[fields[0]]] = {
                    "text": fields[1].translate(STRAIGHT_QUOTES),
                    "status": fields[2] if len(fields) > 2 and fields[2] else "draft",
                    "note": note,
                }
    for episode in EPISODES:
        merged = read_translation(lang, episode)
        for row in read_source(episode):
            done = by_source.get(row["text"])
            if done:
                merged[row["key"]] = {**done, "hash": sc.source_hash(row["text"])}
                count += 1
        write_translation(lang, episode, merged)
    print(f"applied {len(by_source)} strings to {count} rows")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="command", required=True)
    sheet = sub.add_parser("worksheet")
    sheet.add_argument("--lang", required=True)
    sheet.add_argument("--out")
    back = sub.add_parser("merge")
    back.add_argument("--lang", required=True)
    back.add_argument("sheet")
    report = sub.add_parser("status")
    report.add_argument("--lang")
    check = sub.add_parser("lint")
    check.add_argument("--lang", required=True)
    export = sub.add_parser("tm-export")
    export.add_argument("--batch-chars", type=int, default=9000)
    batch = sub.add_parser("tm-check")
    batch.add_argument("path")
    apply_tm = sub.add_parser("tm-apply")
    apply_tm.add_argument("--lang", required=True)
    dump = sub.add_parser("dump")
    dump.add_argument("--dir", default=INPUTS, help="folder holding the packs")
    dump.add_argument("--out", default="text-dump/ja")
    show = sub.add_parser("outline")
    show.add_argument("episode", choices=EPISODES)
    show.add_argument("--dir", default=INPUTS, help="folder holding the packs")
    args = ap.parse_args()

    if args.command == "outline":
        outline(load_pack(args.episode, args.dir))
        return
    if args.command == "worksheet":
        worksheet(args.lang, args.out)
        return
    if args.command == "merge":
        merge(args.lang, args.sheet)
        return
    if args.command == "status":
        status(args.lang)
        return
    if args.command == "lint":
        sys.exit(1 if lint(args.lang) else 0)
    if args.command == "tm-export":
        tm_export(args.batch_chars)
        return
    if args.command == "tm-check":
        sys.exit(1 if tm_check(args.path) else 0)
    if args.command == "tm-apply":
        tm_apply(args.lang)
        return
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    for episode in EPISODES:
        data = load_pack(episode, args.dir)
        rows = sc.messages(data) + sc.screen_rows(data, episode)
        with open(out / f"{episode}.tsv", "w", encoding="utf-8", newline="\n") as f:
            f.write("key\tscene\tspeaker\tkind\ttext\n")
            for row in rows:
                f.write(f"{episode}:{row.addr:06X}\t{row.scene}\t{row.speaker}\t{row.kind}\t{row.text}\n")
        print(f"{episode}: {len(rows)} messages, {sum(len(r.text) for r in rows)} characters with tags")


if __name__ == "__main__":
    main()
