#!/usr/bin/env python3

"""Real pictures for measuring the video scalers: crops of Wikimedia Commons
photographs and film stills, kept in corpus/ as JPEG XL with their sources
in corpus/manifest.json.

fetch lists the pictures of CATEGORIES and downloads the originals into the
cache; make decodes each original to sRGB, scales it in linear light to a
4K frame's density, keeps its most detailed 960x540 crop and writes it to
corpus/NAME.jxl.

  python3 dev/video_bench/corpus.py --cache DIR fetch
  ix run bin/jxl -- python3 dev/video_bench/corpus.py --cache DIR make
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
CORPUS = HERE / "corpus"
MANIFEST = CORPUS / "manifest.json"
API = "https://commons.wikimedia.org/w/api.php"
AGENT = "suite-video-bench/1.0 (https://github.com/impulse-desktop/suite)"
CROP = (960, 540)

CATEGORIES = [
    ("landscape", "Featured pictures of landscapes", 16),
    ("city", "Featured pictures of cityscapes", 14),
    ("night", "Featured night photography", 12),
    ("village", "Featured pictures of villages", 8),
    ("bird", "Featured pictures of birds", 14),
    ("mammal", "Featured pictures of mammals", 14),
    ("insect", "Featured pictures of Arthropoda", 10),
    ("fish", "Featured pictures of fish", 6),
    ("reptile", "Featured pictures of reptiles", 6),
    ("sport", "Featured pictures of sports", 12),
    ("musician", "Featured pictures of musicians", 8),
    ("actor", "Featured pictures of actors", 8),
    ("still", "Featured still-life photographs", 10),
    ("object", "Featured pictures of objects", 10),
    ("transport", "Featured pictures of transport", 12),
    ("art", "Featured pictures of the arts", 10),
    ("satellite", "Featured pictures taken from satellites", 6),
    ("astronomy", "Featured pictures of astronomy", 6),
    ("bunny", "Big Buck Bunny", 7),
    ("sintel", "Sintel", 8),
]


def request(params):
    query = urllib.parse.urlencode({**params, "format": "json"})
    with urllib.request.urlopen(urllib.request.Request(f"{API}?{query}", headers={"User-Agent": AGENT}), timeout=60) as reply:
        return json.load(reply)


def subcategories(category):
    params = {"action": "query", "list": "categorymembers", "cmtitle": f"Category:{category}", "cmtype": "subcat", "cmlimit": "200"}
    return [member["title"].removeprefix("Category:") for member in request(params)["query"]["categorymembers"]]


def members(category, depth=2):
    """The files of the category and of its subcategories, depth levels
    down, with their originals' sizes and addresses."""
    found = files(category)
    if depth:
        for child in subcategories(category):
            found += members(child, depth - 1)
    return found


def files(category):
    params = {
        "action": "query",
        "generator": "categorymembers",
        "gcmtitle": f"Category:{category}",
        "gcmtype": "file",
        "gcmlimit": "200",
        "prop": "imageinfo",
        "iiprop": "url|size|mime",
    }
    found = []
    while True:
        reply = request(params)
        for page in reply.get("query", {}).get("pages", {}).values():
            if page.get("imageinfo"):
                info = page["imageinfo"][0]
                found.append({"title": page["title"], "url": info["url"], "width": info["width"], "height": info["height"], "mime": info["mime"]})
        if "continue" not in reply:
            return found
        params.update(reply["continue"])


def usable(entry):
    width, height = entry["width"], entry["height"]
    return entry["mime"] in ("image/jpeg", "image/png") and width >= CROP[0] * 2 and height >= CROP[1] * 2 and 1.2 <= width / height <= 2.0 and width * height <= 80_000_000


def original(cache, entry):
    return cache / (hashlib.sha1(entry["url"].encode()).hexdigest() + (".png" if entry["mime"] == "image/png" else ".jpg"))


def download(cache, entry):
    """The original, once; Commons answers a fast client with 429 and the
    pause it wants."""
    path = original(cache, entry)
    while not path.exists():
        try:
            with urllib.request.urlopen(urllib.request.Request(entry["url"], headers={"User-Agent": AGENT}), timeout=300) as reply:
                data = reply.read()
        except urllib.error.HTTPError as error:
            if error.code != 429:
                raise
            time.sleep(int(error.headers.get("Retry-After") or 30))
            continue
        path.with_suffix(".part").write_bytes(data)
        path.with_suffix(".part").rename(path)
        time.sleep(1)
    return path


def fetch(cache):
    chosen = []
    seen = set()
    for tag, category, count in CATEGORIES:
        unique = {entry["url"]: entry for entry in members(category) if usable(entry) and entry["url"] not in seen}
        candidates = sorted(unique.values(), key=lambda entry: hashlib.sha1(entry["title"].encode()).hexdigest())
        for k, entry in enumerate(candidates[:count]):
            seen.add(entry["url"])
            chosen.append({"name": f"{tag}{k:02d}", "title": entry["title"], "url": entry["url"], "mime": entry["mime"]})
        print(f"{category}: {min(count, len(candidates))} of {len(candidates)}", flush=True)
    CORPUS.mkdir(exist_ok=True)
    MANIFEST.write_text(json.dumps(chosen, indent=1, ensure_ascii=False) + "\n")
    cache.mkdir(parents=True, exist_ok=True)
    for entry in chosen:
        print(f"{entry['name']} {download(cache, entry).stat().st_size}", flush=True)


def tool(work):
    binary = work / "corpus"
    flags = " ".join(os.environ.get(name, "") for name in ("CPPFLAGS", "CFLAGS"))
    libraries = " ".join(os.environ.get(name, "") for name in ("CTRFLAGS", "LDFLAGS"))
    subprocess.run(f"cc -O2 {flags} -o {binary} {HERE / 'corpus.c'} {libraries} -lm", shell=True, check=True)
    return binary


def prepare(binary, cache, work, entry):
    source = original(cache, entry)
    staged = work / f"{entry['name']}.jxl"
    pixels = work / f"{entry['name']}.pfm"
    cropped = work / f"{entry['name']}.ppm"
    lossless = ["--lossless_jpeg=1"] if entry["mime"] == "image/jpeg" else ["-d", "0"]
    subprocess.run(["cjxl", str(source), str(staged), "-e", "1", *lossless], check=True, capture_output=True)
    subprocess.run(["djxl", "--color_space=RGB_D65_SRG_Rel_SRG", str(staged), str(pixels)], check=True, capture_output=True)
    placed = subprocess.run([str(binary), "crop", str(pixels), str(cropped), str(CROP[0]), str(CROP[1])], check=True, capture_output=True, text=True).stdout.split()
    subprocess.run(["cjxl", str(cropped), str(CORPUS / f"{entry['name']}.jxl"), "-d", "1", "-e", "7"], check=True, capture_output=True)
    for path in (staged, pixels, cropped):
        path.unlink()
    return {**entry, "crop": [int(value) for value in placed[1:5]], "scale": float(placed[6])}


def make(cache, work):
    work.mkdir(parents=True, exist_ok=True)
    binary = tool(work)
    entries = json.loads(MANIFEST.read_text())
    with ThreadPoolExecutor(os.cpu_count() or 1) as pool:
        made = list(pool.map(lambda entry: prepare(binary, cache, work, entry), entries))
    for entry in made:
        print(f"{entry['name']} crop {entry['crop']} scale {entry['scale']:.3f} {(CORPUS / (entry['name'] + '.jxl')).stat().st_size}", flush=True)
    MANIFEST.write_text(json.dumps(made, indent=1, ensure_ascii=False) + "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cache", required=True)
    parser.add_argument("--work", default="")
    parser.add_argument("step", choices=("fetch", "make"))
    args = parser.parse_args()
    cache = Path(args.cache)
    if args.step == "fetch":
        fetch(cache)
    else:
        make(cache, Path(args.work) if args.work else cache / "work")


if __name__ == "__main__":
    sys.exit(main())
