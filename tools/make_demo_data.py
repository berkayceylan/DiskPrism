#!/usr/bin/env python3
"""Generates the synthetic file tree used by the website's live demo and its screenshots.

Nothing here is read from a real disk: every folder, file name and size is made up, and
the generator is seeded, so it always produces the same tree.

    tools/make_demo_data.py --json website/assets/demo-tree.json
        write the tree for the browser demo

    tools/make_demo_data.py --fixture /tmp/demo
        create the same tree as sparse files below /tmp/demo/Studio, so that
        `DiskPrism --snapshot /tmp/demo/Studio --logical` renders it. Each file's length is
        its size on disk in the tree, and no data blocks are written.

JSON format (compact on purpose)
    folder  ["name", [children...]]
    file    ["name", logical]                     on-disk size = logical rounded up to 4 KiB
            ["name", logical, physical]           sparse or compressed file
            ["name", logical, physical, flags]    flags: 1 cloud-only (SF_DATALESS)
                                                         2 hard-linked, carries the on-disk size
                                                         4 further name of a hard-linked file
                                                         8 sparse
"""

import argparse
import json
import os
import random
import sys
import time

KB, MB, GB = 10**3, 10**6, 10**9
CLOUD, LINK, LINK_DUP, SPARSE = 1, 2, 4, 8

rng = random.Random(20261125)


def around(size, spread=0.35):
    """A size near `size`, never a suspiciously round number."""
    return max(1, int(size * rng.uniform(1 - spread, 1 + spread)) + rng.randrange(997))


def f(name, size, spread=0.12):
    return [name, around(size, spread)]


def cloud(name, size):
    return [name, around(size, 0.06), 0, CLOUD]


def linked(name, size):
    size = around(size, 0.02)
    return [name, size, (size + 4095) // 4096 * 4096, LINK]


def link_of(name, original):
    return [name, original[1], 0, LINK_DUP]


def sparse(name, logical, physical):
    return [name, logical, physical, SPARSE]


def d(name, *children):
    flat = []
    for child in children:
        flat.extend(child) if child and isinstance(child[0], list) else flat.append(child)
    return [name, flat]


def series(pattern, count, size, spread=0.35, start=1):
    return [[pattern.format(i), around(size, spread)] for i in range(start, start + count)]


def tail(stems, extension, count, size):
    """Many small files with a heavy-tailed size distribution, like a real source tree."""
    out, seen = [], set()
    while len(out) < count:
        name = f"{rng.choice(stems)}{rng.choice(['', '_util', '_impl', '_core', '_view', '_test', '2', '_v2'])}.{extension}"
        if name in seen:
            name = f"{rng.choice(stems)}_{len(out)}.{extension}"
        if name in seen:
            continue
        seen.add(name)
        out.append([name, max(180, int(size * rng.paretovariate(1.25) * 0.4))])
    return out


LANGUAGES = ["ar", "ca", "cs", "da", "de", "el", "en", "es", "fi", "fr", "he", "hi", "hr", "hu", "id", "it", "ja", "ko", "ms", "nb", "nl",
             "pl", "pt", "ro", "ru", "sk", "sv", "th", "tr", "uk", "vi", "zh_CN", "zh_TW"]
NOUNS = ["arena", "atlas", "batch", "buffer", "camera", "canvas", "cluster", "codec", "color", "curve", "cursor", "decoder", "device",
         "effect", "encoder", "event", "filter", "frame", "glyph", "graph", "grid", "image", "index", "input", "kernel", "layer", "layout",
         "light", "loader", "matrix", "mesh", "model", "node", "noise", "packet", "panel", "parser", "path", "pipeline", "pixel", "pool",
         "query", "queue", "ray", "region", "render", "sampler", "scanner", "scene", "shader", "signal", "stream", "surface", "table",
         "texture", "timeline", "token", "tracer", "vector", "view", "volume", "window", "worker"]


def localizations(size):
    return [d(f"{code}.lproj", f("Localizable.strings", size, 0.4), f("MainMenu.nib", size * 0.6, 0.4)) for code in LANGUAGES]


def framework(name, binary, resources=6):
    return d(f"{name}.framework", f(name, binary), d("Resources", series("Asset{:02d}.car", resources, binary * 0.03, 0.7),
                                                      f("Info.plist", 2 * KB)), d("Headers", tail(NOUNS, "h", 10, 6 * KB)))


def applications():
    codec = linked("libcodec.dylib", 412 * MB)
    runtime = linked("libruntime.dylib", 186 * MB)
    models = linked("denoise.mlmodel", 640 * MB)
    return d(
        "Applications",
        d("Lumen Editor.app", d(
            "Contents",
            d("MacOS", f("Lumen Editor", 318 * MB)),
            d("Frameworks", codec, runtime, framework("LumenCore", 540 * MB), framework("ColorKit", 96 * MB), framework("Timeline", 148 * MB)),
            d("Resources", models, f("Assets.car", 284 * MB), series("LUT-{:03d}.cube", 64, 1.1 * MB, 0.1), series("Title {:02d}.template", 40, 3.2 * MB, 0.8),
              localizations(86 * KB)))),
        d("Lumen Helper.app", d(
            "Contents",
            d("MacOS", f("Lumen Helper", 22 * MB)),
            d("Frameworks", link_of("libcodec.dylib", codec), link_of("libruntime.dylib", runtime)),
            d("Resources", link_of("denoise.mlmodel", models), f("Assets.car", 18 * MB)))),
        d("Orbit Browser.app", d(
            "Contents",
            d("MacOS", f("Orbit Browser", 9 * MB)),
            d("Frameworks", d("Orbit Framework.framework", f("Orbit Framework", 246 * MB), d("Resources", series("locale-{:02d}.pak", 54, 1.4 * MB, 0.3),
                                                                                          f("resources.pak", 38 * MB), f("icudtl.dat", 10.7 * MB))),
              d("Helpers", series("Orbit Helper {:d}", 4, 2.2 * MB))),
            d("Resources", f("app.asar", 212 * MB), f("app.icns", 1.8 * MB), localizations(34 * KB)))),
        d("Kiln 3D.app", d(
            "Contents",
            d("MacOS", f("Kiln", 402 * MB)),
            d("Frameworks", framework("KilnRender", 388 * MB), framework("OpenGraph", 74 * MB), framework("Physics", 112 * MB)),
            d("Resources", series("matcap-{:02d}.exr", 24, 8.4 * MB, 0.2), series("studio-{:02d}.hdr", 16, 22 * MB, 0.3), f("default.metallib", 96 * MB),
              d("python", tail(NOUNS, "py", 90, 18 * KB), tail(NOUNS, "pyc", 90, 22 * KB))))),
        d("Waveform.app", d(
            "Contents",
            d("MacOS", f("Waveform", 126 * MB)),
            d("Resources", series("IR-{:03d}.wav", 48, 5.6 * MB, 0.5), f("Assets.car", 64 * MB), localizations(22 * KB)))),
        d("Draft Notes.app", d("Contents", d("MacOS", f("Draft Notes", 31 * MB)), d("Resources", f("Assets.car", 27 * MB), localizations(14 * KB)))),
        d("Sketchbook.app", d("Contents", d("MacOS", f("Sketchbook", 88 * MB)), d("Resources", f("Assets.car", 142 * MB), series("brush-{:03d}.png", 120, 320 * KB, 0.8)))),
    )


def projects():
    return d(
        "Projects",
        d("renderer",
          d("src", tail(NOUNS, "cpp", 120, 26 * KB), tail(NOUNS, "hpp", 110, 9 * KB), tail(NOUNS, "metal", 24, 12 * KB)),
          d("build", tail(NOUNS, "o", 170, 1.9 * MB), f("librenderer.a", 486 * MB), f("librenderer.dylib", 214 * MB), f("renderer_tests", 168 * MB)),
          d("third_party", d("zstd", tail(NOUNS, "c", 50, 30 * KB), tail(NOUNS, "h", 30, 9 * KB)), d("fmt", tail(NOUNS, "h", 20, 24 * KB))),
          d(".git", d("objects", d("pack", f("pack-7c41.pack", 1.34 * GB), f("pack-7c41.idx", 18 * MB), f("pack-e09b.pack", 212 * MB))), f("index", 1.2 * MB)),
          f("CMakeLists.txt", 14 * KB), f("README.md", 9 * KB)),
        d("web-console",
          d("src", tail(NOUNS, "ts", 140, 11 * KB), tail(NOUNS, "css", 40, 6 * KB)),
          d("dist", series("chunk-{:03d}.js", 60, 240 * KB, 0.7), series("chunk-{:03d}.map", 60, 690 * KB, 0.7), f("app.wasm", 22 * MB)),
          d("node_modules", [d(n, tail(NOUNS, "js", 12, 34 * KB), tail(NOUNS, "ts", 5, 12 * KB), f("package.json", 2 * KB)) for n in NOUNS[:26]]),
          f("package.json", 4 * KB)),
        d("perception-model",
          d("checkpoints", series("step-{:05d}.safetensors", 7, 3.9 * GB, 0.02, 1000)),
          d("data", series("train-{:03d}.parquet", 36, 246 * MB, 0.08), series("eval-{:02d}.parquet", 6, 118 * MB, 0.1)),
          d("notebooks", series("experiment-{:02d}.ipynb", 18, 5.2 * MB, 0.9)),
          d("src", tail(NOUNS, "py", 60, 15 * KB))),
        d("firmware", d("src", tail(NOUNS, "c", 60, 18 * KB), tail(NOUNS, "h", 60, 6 * KB)), d("out", series("board-{:02d}.bin", 12, 8.2 * MB, 0.2), f("firmware.elf", 64 * MB))),
    )


def media():
    return d(
        "Media",
        d("Footage",
          d("Coastline", series("A001_C{:03d}.mov", 14, 4.1 * GB, 0.4), series("A001_C{:03d}.proxy.mp4", 14, 210 * MB, 0.4)),
          d("Night Market", series("B002_C{:03d}.mov", 10, 3.2 * GB, 0.45), series("B002_C{:03d}.proxy.mp4", 10, 160 * MB, 0.4)),
          d("Interviews", series("INT_{:02d}.mp4", 9, 1.9 * GB, 0.3), series("INT_{:02d}.wav", 9, 540 * MB, 0.2))),
        d("Renders",
          d("Glacier", series("glacier.{:04d}.exr", 180, 46 * MB, 0.06, 1001)),
          d("Orbit Titles", series("titles.{:04d}.exr", 90, 28 * MB, 0.08, 1001), f("titles-final.mov", 3.6 * GB))),
        d("Photos",
          d("Raw", series("DSC_{:04d}.dng", 130, 46 * MB, 0.18, 2200)),
          d("Edited", series("DSC_{:04d}.psd", 22, 320 * MB, 0.5, 2200), series("DSC_{:04d}.jpg", 130, 8.4 * MB, 0.4, 2200)),
          d("Phone", series("IMG_{:04d}.heic", 210, 2.3 * MB, 0.45, 4100), series("IMG_{:04d}.mov", 26, 118 * MB, 0.6, 4100))),
        d("Audio",
          d("Sessions", [d(f"Session {n}", series("Track {:02d}.wav", 16, 182 * MB, 0.25), f(f"Session {n}.wfproj", 12 * MB)) for n in ("North", "Harbor", "Slow Light")]),
          d("Library", series("Loop {:03d}.aif", 110, 9.4 * MB, 0.6), series("Album {:02d}.flac", 48, 34 * MB, 0.3))),
    )


def cloud_drive():
    return d(
        "Cloud Drive",
        d("Archive", cloud("Footage 2023.zip", 21.4 * GB), cloud("Footage 2024.zip", 17.8 * GB), cloud("Project Backups.dmg", 12.6 * GB)),
        d("Talks", cloud("Keynote Recording.mov", 9.2 * GB), cloud("Workshop Day 1.mov", 6.1 * GB), cloud("Workshop Day 2.mov", 5.7 * GB),
          f("Keynote.key", 412 * MB), f("Workshop.key", 188 * MB)),
        d("Field Recordings", [cloud(f"Forest {i:02d}.wav", 760 * MB) for i in range(1, 9)], series("Harbor {:02d}.wav", 4, 640 * MB, 0.15)),
        d("Documents", series("Report {:02d}.pdf", 34, 9.6 * MB, 0.8), series("Sheet {:02d}.numbers", 18, 4.2 * MB, 0.7),
          [cloud(f"Scan {i:03d}.pdf", 86 * MB) for i in range(1, 22)], series("Notes {:02d}.md", 60, 14 * KB, 0.8)),
    )


def library():
    return d(
        "Library",
        d("Caches",
          d("Orbit Browser", series("data_{:d}", 5, 310 * MB, 0.3, 0), series("f_{:06x}", 190, 620 * KB, 0.9, 4096)),
          d("Lumen Editor", series("render-{:03d}.cache", 46, 162 * MB, 0.5)),
          d("pip", series("wheel-{:02d}.whl", 26, 38 * MB, 0.8)),
          d("Homebrew", series("bottle-{:02d}.tar.gz", 34, 41 * MB, 0.9))),
        d("Developer",
          d("DerivedData", [d(p, d("Build", tail(NOUNS, "o", 60, 1.2 * MB), tail(NOUNS, "swiftmodule", 14, 1.8 * MB)), d("Index", series("store-{:03d}.db", 14, 26 * MB, 0.6)))
                           for p in ("renderer", "Kiln", "DraftNotes")]),
          d("Simulators", f("Runtime 27.dmg", 7.4 * GB), f("Runtime 26.dmg", 6.9 * GB))),
        d("Virtual Machines", sparse("Linux.sparseimage", 68719476736, around(21.3 * GB, 0.01)), sparse("Build Agent.sparseimage", 42949672960, around(9.8 * GB, 0.01))),
        d("Containers", [d(n, f("store.sqlite", 148 * MB, 0.8), f("store.sqlite-wal", 14 * MB, 0.8), series("thumb-{:03d}.jpg", 30, 160 * KB, 0.6))
                         for n in ("Mailbox", "Calendar", "Photos Agent", "Messages")]),
        d("Logs", series("system-{:02d}.log", 30, 6.4 * MB, 0.9), series("crash-{:02d}.ips", 24, 240 * KB, 0.8)),
        d("Fonts", series("Family-{:02d}.otf", 70, 280 * KB, 0.8), series("Collection-{:02d}.ttc", 12, 14 * MB, 0.6)),
    )


def downloads():
    return d(
        "Downloads", f("kiln-4.2-arm64.dmg", 1.24 * GB), f("dataset-sample.tar.gz", 2.86 * GB), f("linux-arm64.iso", 2.1 * GB), f("fonts.zip", 184 * MB),
        series("Invoice {:02d}.pdf", 22, 420 * KB, 0.7), series("Screenshot {:02d}.png", 40, 2.1 * MB, 0.5), f("installer.pkg", 618 * MB), f("sample-pack.zip", 940 * MB))


def build():
    return ["Studio", [applications(), projects(), media(), cloud_drive(), library(), downloads()]]


def is_folder(node):
    return isinstance(node[1], list)


def physical(node):
    return node[2] if len(node) > 2 else (node[1] + 4095) // 4096 * 4096


def summarize(node):
    if is_folder(node):
        totals = [summarize(child) for child in node[1]]
        return tuple(map(sum, zip(*totals))) if totals else (0, 0, 0, 0)
    return (1, node[1], physical(node), 1 if len(node) > 3 and node[3] & CLOUD else 0)


def write_fixture(node, parent, stamp):
    path = os.path.join(parent, node[0])
    if is_folder(node):
        os.makedirs(path, exist_ok=True)
        for child in node[1]:
            write_fixture(child, path, stamp)
    else:
        with open(path, "wb") as handle:
            handle.truncate(physical(node))
    when = stamp - rng.randrange(0, 420 * 86400)
    os.utime(path, (when, when))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--json", metavar="FILE", help="write the tree as JSON")
    parser.add_argument("--fixture", metavar="DIR", help="create the tree as sparse files below DIR")
    options = parser.parse_args()
    if not options.json and not options.fixture:
        parser.error("nothing to do: pass --json and/or --fixture")

    tree = build()
    files, logical, on_disk, clouds = summarize(tree)
    print(f"{files} files, {logical / GB:.1f} GB logical, {on_disk / GB:.1f} GB on disk, {clouds} cloud-only")
    if options.json:
        with open(options.json, "w", encoding="utf-8") as handle:
            json.dump(tree, handle, ensure_ascii=False, separators=(",", ":"))
        print(f"{options.json}: {os.path.getsize(options.json) / 1024:.0f} KiB")
    if options.fixture:
        write_fixture(tree, options.fixture, time.mktime((2026, 11, 20, 12, 0, 0, 0, 0, 0)))
        print(f"fixture: {os.path.join(options.fixture, tree[0])}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
