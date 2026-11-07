#!/usr/bin/env python3
"""Run every floppy image in a software library through the headless TK2000 sim.

For each unique image (deduplicated by SHA-1) it boots the image, takes
screenshots, records a framebuffer hash every frame, and classifies the run:

  ok        screen still changing near the end of the run (or settled late on a
            screen that is not the no-disk BASIC prompt)
  static    screen stopped changing early (title screen waiting for a key, or a hang)
  no-boot   final screen is the plain TK2000 BASIC screen seen with no disk
  blank     final screen is entirely black
  crash     simulator exited abnormally / timed out

Outputs go to <out>/: one folder per image (log, PNGs, contact sheet), plus
report.md and results.json summarising everything.

Example:
  ./run_library.py ../software/mister14/games/TK2000 --frames 1800 -j 8
  ./run_library.py ../software --filter -i karateka --frames 3000 --keys 900:\\n
"""
import argparse, hashlib, json, os, re, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor, as_completed

HERE = os.path.dirname(os.path.abspath(__file__))
EXTS = ('.dsk', '.do', '.po', '.nib', '.ct2')


def sha1(path):
    h = hashlib.sha1()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def find_images(roots, pattern, ignore_case):
    flags = re.I if ignore_case else 0
    seen = {}
    for root in roots:
        if os.path.isfile(root):
            cands = [root]
        else:
            cands = []
            for d, dirs, files in os.walk(root):
                dirs[:] = [x for x in dirs if x != 'by-format']
                cands += [os.path.join(d, f) for f in files]
        for p in sorted(cands):
            if not p.lower().endswith(EXTS):
                continue
            if pattern and not re.search(pattern, p, flags):
                continue
            try:
                h = sha1(p)
            except OSError:
                continue
            seen.setdefault(h, []).append(p)
    return seen


def safe_name(path, h):
    base = os.path.splitext(os.path.basename(path))[0]
    base = re.sub(r'[^A-Za-z0-9._-]+', '_', base).strip('_') or 'image'
    return f"{base}_{os.path.splitext(path)[1][1:].lower()}_{h[:8]}"


def contact_sheet(pngs, out):
    try:
        from PIL import Image
    except ImportError:
        return False
    if not pngs:
        return False
    ims = [Image.open(p) for p in pngs]
    w, h = ims[0].size
    cols = min(4, len(ims))
    rows = (len(ims) + cols - 1) // cols
    sheet = Image.new('RGB', (cols * (w + 4), rows * (h + 4)), 'white')
    for i, im in enumerate(ims):
        sheet.paste(im, ((i % cols) * (w + 4), (i // cols) * (h + 4)))
    sheet.save(out)
    return True


def run_one(args, h, paths, nodisk_hashes):
    img = paths[0]
    name = safe_name(img, h)
    d = os.path.join(args.out, name)
    os.makedirs(d, exist_ok=True)
    if img.lower().endswith('.ct2'):
        # Tape: boot to BASIC with the disk ROM off, type LOADT, then insert
        # the tape (it starts playing when loaded, like pressing PLAY).
        media = ['--no-disk-rom', '--send-keys', '100:LOADT\\n',
                 '--tape', os.path.abspath(img), '--tape-frame', '250']
    else:
        media = ['--floppy', os.path.abspath(img)]
    cmd = [args.sim, '--headless'] + media + [
           '--stop-at-frame', str(args.frames), '--hash',
           '--screenshot-every', str(args.shot_every),
           '--screenshot-dir', d, '--screenshot-prefix', 'f',
           '--sd-ack-delay', str(args.sd_ack_delay)]
    if args.legacy_main_interleave:
        cmd.append('--legacy-main-interleave')
    for k in args.keys or []:
        cmd += ['--send-keys', k]
    for j in args.joy or []:
        cmd += ['--joy', j]
    t0 = time.time()
    status = 'crash'
    try:
        with open(os.path.join(d, 'run.log'), 'w') as log:
            # cwd must be the verilator dir: the ROM .hex paths are relative.
            r = subprocess.run(cmd, cwd=HERE, stdout=log, stderr=subprocess.STDOUT,
                               timeout=args.timeout)
        rc = r.returncode
    except subprocess.TimeoutExpired:
        rc = -999
    secs = time.time() - t0

    hashes = {}
    stats = ''
    with open(os.path.join(d, 'run.log'), errors='replace') as f:
        for line in f:
            if line.startswith('FBHASH frame'):
                _, _, fr, hv = line.split()
                hashes[int(fr)] = hv
            elif line.startswith('DISKSTATS'):
                stats = line.strip()
    frames = sorted(hashes)
    last = frames[-1] if frames else 0
    final = hashes.get(last)
    # frame at which the screen last changed
    last_change = 0
    for fr in frames[1:]:
        if hashes[fr] != hashes.get(fr - 1):
            last_change = fr
    distinct = len(set(hashes.values()))

    if rc != 0 or last < args.frames:
        status = 'crash'
    elif final in nodisk_hashes and last_change < args.frames - args.settle:
        status = 'no-boot'
    elif final == args.black_hash:
        status = 'blank'
    elif last_change >= args.frames - args.settle:
        status = 'ok'
    elif last_change >= args.frames * 0.5:
        status = 'ok'
    else:
        status = 'static'

    pngs = sorted(os.path.join(d, p) for p in os.listdir(d) if p.startswith('f_') and p.endswith('.png'))
    sheet = os.path.join(d, 'sheet.png')
    contact_sheet(pngs, sheet)
    return {
        'name': name, 'sha1': h, 'image': img, 'aliases': paths[1:], 'status': status,
        'rc': rc, 'seconds': round(secs, 1), 'frames': last, 'last_change_frame': last_change,
        'distinct_screens': distinct, 'diskstats': stats,
        'sheet': os.path.relpath(sheet, args.out) if os.path.exists(sheet) else None,
    }


def nodisk_baseline(args):
    """Hashes of the screens the machine shows with no disk (BASIC prompt etc.)."""
    d = os.path.join(args.out, '_nodisk')
    os.makedirs(d, exist_ok=True)
    cmd = [args.sim, '--headless', '--stop-at-frame', str(min(args.frames, 600)), '--hash']
    out = subprocess.run(cmd, cwd=HERE, capture_output=True, text=True).stdout
    hs = [l.split()[3] for l in out.splitlines() if l.startswith('FBHASH frame')]
    # all screens after the first ~30 frames of reset noise
    return set(hs[30:]) if len(hs) > 30 else set(hs)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('roots', nargs='+', help='image files or directories to scan')
    ap.add_argument('--out', default=os.path.join(HERE, 'library_results'))
    ap.add_argument('--sim', default=os.path.join(HERE, 'obj_dir', 'Vemu'))
    ap.add_argument('--frames', type=int, default=1800, help='frames to run per image (60/s)')
    ap.add_argument('--shot-every', type=int, default=150)
    ap.add_argument('--settle', type=int, default=300, help='frames of no change that count as static')
    ap.add_argument('--sd-ack-delay', type=int, default=1200)
    ap.add_argument('--legacy-main-interleave', action='store_true', help='old Main_MiSTer .dsk layout (A/B)')
    ap.add_argument('--keys', action='append', help='--send-keys value passed to every run (frame:text)')
    ap.add_argument('--joy', action='append', help='--joy value passed to every run (frame:hexmask)')
    ap.add_argument('--filter', help='regex on the image path')
    ap.add_argument('-i', action='store_true', help='case-insensitive --filter')
    ap.add_argument('-j', '--jobs', type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument('--timeout', type=int, default=3600)
    ap.add_argument('--black-hash', default=None)
    args = ap.parse_args()

    if not os.path.exists(args.sim):
        sys.exit(f"sim binary not found: {args.sim} (run make)")
    os.makedirs(args.out, exist_ok=True)
    images = find_images(args.roots, args.filter, args.i)
    print(f"{len(images)} unique images")
    nodisk = nodisk_baseline(args)

    results = []
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(run_one, args, h, p, nodisk): h for h, p in images.items()}
        for n, fut in enumerate(as_completed(futs), 1):
            r = fut.result()
            results.append(r)
            print(f"[{n}/{len(images)}] {r['status']:8s} {r['seconds']:7.1f}s  {r['image']}", flush=True)

    results.sort(key=lambda r: (r['status'] == 'ok', r['image'].lower()))
    with open(os.path.join(args.out, 'results.json'), 'w') as f:
        json.dump(results, f, indent=1)
    counts = {}
    for r in results:
        counts[r['status']] = counts.get(r['status'], 0) + 1
    with open(os.path.join(args.out, 'report.md'), 'w') as f:
        f.write(f"# TK2000 library run\n\n{len(results)} images, {args.frames} frames each, "
                f"sd-ack-delay {args.sd_ack_delay}\n\n")
        f.write(' | '.join(f"**{k}**: {v}" for k, v in sorted(counts.items())) + '\n\n')
        f.write('| status | image | last change | screens | disk |\n|---|---|---|---|---|\n')
        for r in results:
            link = f"[{os.path.basename(r['image'])}]({r['sheet']})" if r['sheet'] else os.path.basename(r['image'])
            f.write(f"| {r['status']} | {link} | {r['last_change_frame']} | {r['distinct_screens']} | "
                    f"{r['diskstats'].replace('DISKSTATS ', '')} |\n")
    print(counts)
    print(f"report: {os.path.join(args.out, 'report.md')}")


if __name__ == '__main__':
    main()
