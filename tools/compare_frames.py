#!/usr/bin/env python3
"""Compare recompiled game frames against Mesen-generated reference frames.

Converts both to a common NES palette representation before pixel-by-pixel comparison.

Usage:
    python3 compare_frames.py --ref DIR --recomp DIR --frames N [--output FILE]
"""
import argparse
import os
import sys
from PIL import Image
import numpy as np

# NES system palette, MUST match g_nes_palette in
# external/nesrecomp/runner/src/ppu_renderer.c (ARGB8888 -> RGB rows).
# Recomp screenshots map to exact indices; Mesen frames map nearest.
# (The old table was mostly [0,0,0] rows, collapsing all dark colors to one
# index and inflating cross-renderer matches — see TAS harness notes.)
NES_PALETTE = np.array([
    [0x54, 0x54, 0x54], [0x00, 0x1E, 0x74], [0x08, 0x10, 0x90], [0x30, 0x00, 0x88],
    [0x44, 0x00, 0x64], [0x5C, 0x00, 0x30], [0x54, 0x04, 0x00], [0x3C, 0x18, 0x00],
    [0x20, 0x2A, 0x00], [0x08, 0x3A, 0x00], [0x00, 0x40, 0x00], [0x00, 0x3C, 0x00],
    [0x00, 0x32, 0x3C], [0x00, 0x00, 0x00], [0x00, 0x00, 0x00], [0x00, 0x00, 0x00],
    [0x98, 0x96, 0x98], [0x08, 0x4C, 0xC4], [0x30, 0x32, 0xEC], [0x5C, 0x1E, 0xE4],
    [0x88, 0x14, 0xB0], [0xA0, 0x14, 0x64], [0x98, 0x22, 0x20], [0x78, 0x3C, 0x00],
    [0x54, 0x5A, 0x00], [0x28, 0x72, 0x00], [0x08, 0x7C, 0x00], [0x00, 0x76, 0x28],
    [0x00, 0x66, 0x78], [0x00, 0x00, 0x00], [0x00, 0x00, 0x00], [0x00, 0x00, 0x00],
    [0xEC, 0xEE, 0xEC], [0x4C, 0x9A, 0xEC], [0x78, 0x7C, 0xEC], [0xB0, 0x62, 0xEC],
    [0xE4, 0x54, 0xEC], [0xEC, 0x58, 0xB4], [0xEC, 0x6A, 0x64], [0xD4, 0x88, 0x20],
    [0xA0, 0xAA, 0x00], [0x74, 0xC4, 0x00], [0x4C, 0xD0, 0x20], [0x38, 0xCC, 0x6C],
    [0x38, 0xB4, 0xCC], [0x3C, 0x3C, 0x3C], [0x00, 0x00, 0x00], [0x00, 0x00, 0x00],
    [0xEC, 0xEE, 0xEC], [0xA8, 0xCC, 0xEC], [0xBC, 0xBC, 0xEC], [0xD4, 0xB2, 0xEC],
    [0xEC, 0xAE, 0xEC], [0xEC, 0xAE, 0xD4], [0xEC, 0xB4, 0xB0], [0xE4, 0xC4, 0x90],
    [0xCC, 0xD2, 0x78], [0xB4, 0xDE, 0x78], [0xA8, 0xE2, 0x90], [0x98, 0xE2, 0xB4],
    [0xA0, 0xD6, 0xE4], [0xA0, 0xA2, 0xA0], [0x00, 0x00, 0x00], [0x00, 0x00, 0x00],
], dtype=np.uint8)


def image_to_palette_indices(img_array):
    """Convert an RGB image array to NES palette indices."""
    h, w, c = img_array.shape
    pixels = img_array.reshape(-1, 3).astype(np.int32)
    diff = pixels[:, np.newaxis, :] - NES_PALETTE[np.newaxis, :, :]
    dists = np.sum(diff * diff, axis=2)
    indices = np.argmin(dists, axis=1).reshape(h, w)
    closest = NES_PALETTE[indices]
    return indices, closest.reshape(h, w, 3).astype(np.uint8)


def compare_frames(ref_dir, recomp_dir, num_frames, output_file=None):
    """Compare reference and recompiled frames."""
    results = []

    for i in range(num_frames):
        ref_path = os.path.join(ref_dir, f"frame_{i:04d}.png")
        recomp_path = os.path.join(recomp_dir, f"frame_{i:04d}.png")

        if not os.path.exists(ref_path):
            print(f"Missing reference: {ref_path}", file=sys.stderr)
            continue
        if not os.path.exists(recomp_path):
            print(f"Missing recompiled: {recomp_path}", file=sys.stderr)
            continue

        ref_img = Image.open(ref_path).convert('RGB')
        recomp_img = Image.open(recomp_path).convert('RGB')

        ref_arr = np.array(ref_img)
        recomp_arr = np.array(recomp_img)

        # Convert both to palette indices
        ref_indices, ref_closest = image_to_palette_indices(ref_arr)
        recomp_indices, recomp_closest = image_to_palette_indices(recomp_arr)

        # Compare palette indices
        idx_match = np.sum(ref_indices == recomp_indices)
        total = 256 * 240
        idx_pct = 100.0 * idx_match / total

        # Also compare raw RGB
        raw_match = np.sum(np.all(ref_arr == recomp_arr, axis=2))
        raw_pct = 100.0 * raw_match / total

        # Find which tiles differ (8x8 blocks)
        tile_diff_count = 0
        tile_total = (256 // 8) * (240 // 8)  # 32 * 30 = 960 tiles
        for y in range(0, 240, 8):
            for x in range(0, 256, 8):
                ref_block = ref_indices[y:y+8, x:x+8]
                recomp_block = recomp_indices[y:y+8, x:x+8]
                if not np.array_equal(ref_block, recomp_block):
                    tile_diff_count += 1

        results.append({
            'frame': i,
            'idx_match_pct': idx_pct,
            'raw_match_pct': raw_pct,
            'tiles_diff': tile_diff_count,
            'tiles_total': tile_total,
        })

        print(f"Frame {i:4d}: palette_idx {idx_pct:5.1f}%  raw_rgb {raw_pct:5.1f}%  tiles {tile_diff_count}/{tile_total} differ")

    # Summary
    print(f"\n--- Summary ({len(results)} frames) ---")
    if results:
        avg_idx = np.mean([r['idx_match_pct'] for r in results])
        avg_tiles = np.mean([r['tiles_diff'] for r in results])
        print(f"Average palette index match: {avg_idx:.1f}%")
        print(f"Average tiles differing: {avg_tiles:.1f}/{tile_total}")

    if output_file:
        with open(output_file, 'w') as f:
            f.write("frame,palette_idx_pct,raw_rgb_pct,tiles_diff,tiles_total\n")
            for r in results:
                f.write(f"{r['frame']},{r['idx_match_pct']:.1f},{r['raw_match_pct']:.1f},{r['tiles_diff']},{r['tiles_total']}\n")
        print(f"\nResults saved to {output_file}")


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Compare NES frames (Mesen reference vs recompiled)')
    parser.add_argument('--ref', default='nes_reference', help='Reference frame directory (from Mesen)')
    parser.add_argument('--recomp', default='cyc_shots', help='Cycle screenshot directory (from --shot-every)')
    parser.add_argument('--frames', type=int, default=10, help='Number of frames to compare')
    parser.add_argument('--output', default=None, help='CSV output file')
    args = parser.parse_args()

    compare_frames(args.ref, args.recomp, args.frames, args.output)
