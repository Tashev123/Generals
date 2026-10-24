"""Looks at a recording of what the game's mixer put out (gen_test.ps1 -Record file.wav), since a test run
can't be listened to:

  audio_check.py rec.wav                      loudness, silent / sounding stretches, clipping
  audio_check.py rec.wav --sides [seconds]    loudness of the left and the right side over time (panning)
  audio_check.py rec.wav name[:pitch] ...     where each of those sounds of the game is in the recording, how
                                              well it matches (1.0 = the same sound), and at what level per side
  audio_check.py --dumps <folder>             compares the game's decoder (gen_test.ps1 -DumpWav "a,b" writes
                                              Saved/AudioTest/a.wav) with the decoder in this file, sample by sample

A sound's name is its file name without folder or extension, as the [audio f..] trace lines print it in
brackets; the pitch is the one on that line. Expected level of a sound: its trace "vol" x 0.5 for one in
the middle (0.71 for the pan, 0.71 Unreal's output headroom), as long as the limiter isn't working.
"""
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_vfs

STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
        157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
        1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
        10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]
INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]
FOLDERS = ("data/audio/sounds/english/", "data/audio/sounds/", "data/audio/speech/english/", "data/audio/speech/")


def chunks(b):
    pos, out = 12, {}
    while pos + 8 <= len(b):
        cid, size = b[pos:pos + 4], struct.unpack("<I", b[pos + 4:pos + 8])[0]
        out.setdefault(cid, b[pos + 8:pos + 8 + size])
        pos += 8 + size + (size & 1)
    return out


def decode(b):
    """A WAV file (PCM 16 or IMA ADPCM, as the game's sounds are): rate, channels, interleaved samples."""
    c = chunks(b)
    tag, ch, rate, _, align, bits = struct.unpack("<HHIIHH", c[b"fmt "][:16])
    data = c[b"data"]
    if tag == 1:
        return rate, ch, np.frombuffer(data[:len(data) // 2 * 2], dtype="<i2").astype(np.int32)
    assert tag == 0x11, "format tag %d" % tag
    fact = struct.unpack("<I", c[b"fact"][:4])[0] if b"fact" in c else None
    out = [[] for _ in range(ch)]
    for off in range(0, len(data), align):
        block = data[off:off + align]
        if len(block) < 4 * ch:
            break
        pred, idx = [], []
        for k in range(ch):
            p, i, _ = struct.unpack("<hBB", block[k * 4:k * 4 + 4])
            pred.append(p)
            idx.append(min(max(i, 0), 88))
            out[k].append(p)
        body = block[4 * ch:]
        for g in range(len(body) // (4 * ch)):          # four bytes per channel, the channels in turn
            for k in range(ch):
                for byte in body[(g * ch + k) * 4:(g * ch + k) * 4 + 4]:
                    for nib in (byte & 15, byte >> 4):
                        step = STEP[idx[k]]
                        diff = (step >> 3) + (step if nib & 4 else 0) + (step >> 1 if nib & 2 else 0) + (step >> 2 if nib & 1 else 0)
                        pred[k] = max(-32768, min(32767, pred[k] - diff if nib & 8 else pred[k] + diff))
                        idx[k] = max(0, min(88, idx[k] + INDEX[nib & 7]))
                        out[k].append(pred[k])
    n = min(len(o) for o in out)
    n = min(n, fact) if fact else n
    inter = np.empty(n * ch, dtype=np.int32)
    for k in range(ch):
        inter[k::ch] = out[k][:n]
    return rate, ch, inter


def find(vfs, name):
    for folder in FOLDERS:
        if vfs.exists(folder + name + ".wav"):
            return folder + name + ".wav"
    return None


def compare_dumps(folder):
    vfs = gen_vfs.GeneralsVFS()
    ok = True
    for f in sorted(os.listdir(folder)):
        src = find(vfs, f[:-4])
        if not src:
            continue
        raw = vfs.read(src)
        rate, ch, ref = decode(raw)
        rate2, ch2, got = decode(open(os.path.join(folder, f), "rb").read())
        same = rate == rate2 and ch == ch2 and len(ref) == len(got) and bool(np.array_equal(ref, got))
        ok &= same
        kind = "ADPCM" if struct.unpack("<H", chunks(raw)[b"fmt "][:2])[0] == 0x11 else "PCM"
        print("%-10s %-5s %5d Hz %d ch %7d samples  %s" % (f[:-4], kind, rate, ch, len(ref), "identical" if same else "DIFFERENT"))
    print("ALL IDENTICAL" if ok else "MISMATCH")


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "--dumps":
        return compare_dumps(sys.argv[2])
    rate, ch, x = decode(open(sys.argv[1], "rb").read())
    rec = x.astype(np.float64).reshape(-1, ch) / 32768.0
    mono = rec.mean(axis=1)
    dur = len(mono) / rate
    args = sys.argv[2:]

    if args and args[0] == "--sides":
        win = int(rate * (float(args[1]) if len(args) > 1 else 1.0))
        print(" time    left   right  share on the right")
        for i in range(len(rec) // win):
            seg = rec[i * win:(i + 1) * win]
            l, r = np.sqrt((seg[:, 0] ** 2).mean()), np.sqrt((seg[:, -1] ** 2).mean())
            print("%5.1f  %.4f  %.4f  %s" % (i * win / rate, l, r, "%.2f" % (r * r / (l * l + r * r)) if l + r > 1e-5 else "-"))
        return

    if not args:
        peak = np.abs(rec).max(axis=1)
        clipped = peak >= 0.999
        print("%.2f s, %d Hz, %d channels; peak %.3f, rms %.4f; samples at full scale: %d (%.3f%%)" % (dur, rate, ch, peak.max(),
              np.sqrt((rec ** 2).mean()), clipped.sum(), 100.0 * clipped.mean()))
        for q in (90, 99, 99.9):
            print("  %5.1f%% of samples below %.3f" % (q, np.percentile(peak, q)))
        win = rate // 10
        n = len(mono) // win
        env = np.sqrt((mono[:n * win].reshape(n, win) ** 2).mean(axis=1))
        runs, start = [], None
        for i, a in enumerate(np.append(env > 1e-4, False)):
            if a and start is None:
                start = i
            if not a and start is not None:
                runs.append((start, i))
                start = None
        print("sound in %d stretches, %.1f s in all:" % (len(runs), sum(b - a for a, b in runs) / 10.0))
        for a, b in runs[:30]:
            print("   %6.1f - %6.1f s   rms %.4f" % (a / 10.0, b / 10.0, np.sqrt((env[a:b] ** 2).mean())))
        return

    vfs = gen_vfs.GeneralsVFS()
    for spec in args:
        name, _, pitch = spec.partition(":")
        pitch = float(pitch) if pitch else 1.0
        path = find(vfs, name)
        if not path:
            print(name, "not found")
            continue
        srate, sch, s = decode(vfs.read(path))
        s = s.astype(np.float64).reshape(-1, sch).mean(axis=1) / 32768.0
        # the source as the mixer has it: at the recording's rate, and at its pitch
        t = np.arange(int(len(s) * rate / (srate * pitch))) * (srate * pitch / rate)
        ref = np.interp(t, np.arange(len(s)), s)
        nfft = 1 << int(np.ceil(np.log2(len(mono) + len(ref))))
        corr = np.fft.irfft(np.fft.rfft(mono, nfft) * np.conj(np.fft.rfft(ref, nfft)), nfft)[:len(mono)]
        k = int(np.argmax(corr))
        m = min(len(mono) - k, len(ref))
        seg, ref = mono[k:k + m], ref[:m]
        power = (ref ** 2).sum()
        denom = np.sqrt((seg ** 2).sum() * power)
        gains = [float((rec[k:k + m, c] * ref).sum() / power) if power > 0 else 0.0 for c in range(ch)]
        print("%-10s (%.2f s%s): at %.2f s, match %.3f, level left %.3f right %.3f" % (name, len(s) / srate,
              ", pitch %.2f" % pitch if pitch != 1.0 else "", k / rate, float((seg * ref).sum() / denom) if denom > 0 else 0.0, gains[0], gains[-1]))


if __name__ == "__main__":
    main()
