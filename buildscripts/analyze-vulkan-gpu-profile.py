"""Rank real GPU shader work; invocation share is NOT GPU time share."""
import argparse
import collections
import csv
import re
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", type=Path)
    parser.add_argument("--materials", type=Path)
    args = parser.parse_args()
    names = {}
    if args.materials:
        for line in args.materials.read_text(encoding="utf-8", errors="replace").splitlines():
            match = re.search(r"\btex=(\d+).*?\bimage=(.*?)\s+diffuse=", line)
            if match:
                names[int(match[1])] = match[2]
    with args.profile.open(encoding="utf-8-sig", newline="") as source:
        rows = list(csv.DictReader(source))
    if not rows:
        raise SystemExit("No completed GPU profile samples")
    totals = [row for row in rows if row["category"] == "scene_total"]
    rows = [row for row in rows if row["category"] != "scene_total"]
    if not rows:
        raise SystemExit("Only whole-scene samples available; waiting for per-draw samples")
    incomplete = [row for row in rows if int(row.get("available_views", "2")) != 2]
    if incomplete:
        availability = collections.Counter(row["available_views"] for row in incomplete)
        draw_frames = {row["frame"] for row in rows}
        whole_frames = {row["frame"] for row in totals}
        draw_fs = sum(int(row["fs_invocations"]) for row in rows) / len(draw_frames)
        whole_fs = sum(int(row["fs_invocations"]) for row in totals) / max(len(whole_frames), 1)
        ratio = draw_fs / max(whole_fs, 1)
        if (not totals or set(availability) != {"1"} or
                len(incomplete) != len(rows) or not 0.95 <= ratio <= 1.05 or
                any(int(row["available_views"]) != 1 for row in totals)):
            raise SystemExit(f"Incomplete multiview GPU results: {dict(availability)}; "
                             f"draw/whole-scene fragment count ratio={ratio:.3f}; cannot rank reliably")
        print("Driver exposes only the first multiview query slot. Combined counts are "
              f"corroborated by independent whole-scene queries: draw/whole ratio={ratio:.3f}. "
              "This comparison assumes a stationary scene.")
    frames = {row["frame"] for row in rows}
    categories = collections.defaultdict(lambda: [0, 0, 0, 0])
    materials = collections.defaultdict(lambda: [0, 0, 0, 0])
    total_fs = 0
    for row in rows:
        fs = int(row["fs_invocations"])
        vs = int(row["vs_invocations"])
        primitives = int(row["ia_primitives"])
        total_fs += fs
        material = (row["category"], row["texture0"], row["texture1"],
                    row["textures"], row["render_state"], row["lighting_mode"])
        for aggregate in (categories[row["category"]], materials[material]):
            aggregate[0] += 1
            aggregate[1] += fs
            aggregate[2] += vs
            aggregate[3] += primitives
    count = len(frames)
    print(f"Samples: {count}, average fragment invocations: {total_fs / count:,.0f}")
    print("Fragment invocation shares measure work counts, not elapsed GPU time.")
    print("\nCategory                 FS share    FS/frame    VS/frame   primitives/frame draws/frame")
    for name, (draws, fs, vs, primitives) in sorted(categories.items(), key=lambda item: -item[1][1]):
        print(f"{name:24} {100 * fs / max(total_fs, 1):7.2f}% {fs / count:11,.0f} "
              f"{vs / count:11,.0f} {primitives / count:17,.0f} {draws / count:11.1f}")
    print("\nTop materials/passes by fragment invocations:")
    for key, (draws, fs, vs, primitives) in sorted(materials.items(), key=lambda item: -item[1][1])[:25]:
        category, texture0, texture1, textures, state, lighting = key
        name = names.get(int(texture0), "<texture name unavailable>")
        print(f"{100 * fs / max(total_fs, 1):6.2f}% FS/frame={fs / count:11,.0f} "
              f"VS/frame={vs / count:9,.0f} draws/frame={draws / count:6.1f} {category} "
              f"tex={texture0},{texture1} stages={textures} state=0x{int(state):08x} "
              f"lighting={lighting} {name}")


if __name__ == "__main__":
    main()
