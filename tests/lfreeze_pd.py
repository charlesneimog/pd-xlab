"""Headless integration checks: python3 tests/lfreeze_pd.py [build/xlab]."""

from pathlib import Path
import math
import re
import struct
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
PACKAGE = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build/xlab"
SAMPLE_RATE = 48000


def cache_period(frame_count):
    spectral_period = frame_count * 2048
    return math.ceil(4 * SAMPLE_RATE / spectral_period) * spectral_period


class Patch:
    def __init__(self):
        self.lines = ["#N canvas 0 0 1000 700 12;"]
        self.connections = []
        self.count = 0
        self.loadbang = self.add("obj", "loadbang")

    def add(self, kind, text):
        index = self.count
        self.count += 1
        self.lines.append(f"#X {kind} 20 20 {text};")
        return index

    def connect(self, source, outlet, target, inlet=0):
        self.connections.append(f"#X connect {source} {outlet} {target} {inlet};")

    def timed(self, milliseconds, messages):
        delay = self.add("obj", f"delay {milliseconds}")
        contents = " ".join(rf"\; {receiver} {message}" for receiver, message in messages)
        message = self.add("msg", contents)
        self.connect(self.loadbang, 0, delay)
        self.connect(delay, 0, message)

    def freezer(self, label, source):
        receiver = self.add("obj", f"r {label}-control")
        freezer = self.add("obj", "x.lfreeze~ -f 2048 -fr 2")
        self.connect(receiver, 0, freezer)
        self.connect(source, 0, freezer)
        snapshot = self.add("obj", "snapshot~")
        inspect = self.add("obj", f"r {label}-snapshot")
        output = self.add("obj", f"print SNAPSHOT_{label}")
        self.connect(freezer, 0, snapshot)
        self.connect(inspect, 0, snapshot)
        self.connect(snapshot, 0, output)
        return freezer

    def recording(self, label, source, length):
        self.add("obj", f"array define {label}-array {length}")
        writer = self.add("obj", f"tabwrite~ {label}-array")
        record = self.add("obj", f"r {label}-record")
        inspect = self.add("obj", f"r {label}-inspect")
        save = self.add("msg", f"write -wave -bytes 4 {label}.wav {label}-array")
        soundfiler = self.add("obj", "soundfiler")
        output = self.add("obj", f"print SAVED_{label}")
        self.connect(source, 0, writer)
        self.connect(record, 0, writer)
        self.connect(inspect, 0, save)
        self.connect(save, 0, soundfiler)
        self.connect(soundfiler, 0, output)

    def write(self, path):
        path.write_text("\n".join(self.lines + self.connections) + "\n")


def wav_values(path):
    data = path.read_bytes()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE", path
    chunks = {}
    offset = 12
    while offset + 8 <= len(data):
        name, length = struct.unpack_from("<4sI", data, offset)
        chunks[name] = data[offset + 8:offset + 8 + length]
        offset += 8 + length + (length & 1)
    fmt = chunks[b"fmt "]
    encoding, channels, sample_rate, _, _, bits = struct.unpack_from("<HHIIHH", fmt)
    if encoding == 65534:  # WAVE_FORMAT_EXTENSIBLE: the GUID starts with the subtype.
        encoding = struct.unpack_from("<I", fmt, 24)[0]
    assert (encoding, channels, sample_rate, bits) == (3, 1, SAMPLE_RATE, 32), path
    samples = chunks[b"data"]
    return struct.unpack(f"<{len(samples) // 4}f", samples)


def assert_repeats(samples, period, spectral_period, label):
    assert len(samples) == period * 2, (len(samples), period)
    assert all(map(math.isfinite, samples)), "Nonfinite frozen output"
    assert sum(sample * sample for sample in samples) > 0.01, "Frozen output is silent"
    # Float WAV files retain the cached samples exactly. Include noisy input so
    # this also catches ongoing random synthesis after the one-time render.
    assert samples[:period] == samples[period:], (
        label, "Loop wrap changed samples",
        "maximum error", max(abs(a - b) for a, b in zip(samples[:period], samples[period:])),
        "first nonzero", next((i for i, value in enumerate(samples) if value), None),
    )
    assert samples[:spectral_period] != samples[spectral_period:2 * spectral_period], \
        "Output still repeats the short spectral cycle"


def main():
    patch = Patch()
    oscillator = patch.add("obj", "osc~ 330")
    tone_gain = patch.add("obj", "*~ 0.2")
    noise = patch.add("obj", "noise~")
    noise_gain = patch.add("obj", "*~ 0.002")
    source = patch.add("obj", "+~")
    patch.connect(oscillator, 0, tone_gain)
    patch.connect(noise, 0, noise_gain)
    patch.connect(tone_gain, 0, source)
    patch.connect(noise_gain, 0, source, 1)

    main_freezer = patch.freezer("main", source)
    cancel_freezer = patch.freezer("cancel", source)
    patch.recording("first", main_freezer, cache_period(2) * 2)
    patch.recording("resized", main_freezer, cache_period(3) * 2)
    patch.recording("recovered", cancel_freezer, cache_period(3) * 2)

    patch.timed(0, [("pd", "dsp 1"), ("cancel-control", "freeze 1")])
    patch.timed(10, [("main-control", "freeze 1")])
    # A 4096-sample capture completes after about 85 ms. Cancel before the
    # renderer's five-millisecond completion poll can publish its output.
    patch.timed(87, [("cancel-control", "freeze 0")])
    patch.timed(300, [("cancel-snapshot", "bang")])
    patch.timed(350, [("cancel-control", "freeze 1")])
    patch.timed(351, [("cancel-control", "frames 3")])
    patch.timed(600, [("cancel-snapshot", "bang")])
    patch.timed(650, [("cancel-control", "freeze 1")])
    # The phase-closed cache spans at least four seconds. Record complete pairs
    # after capture/render/fade-in, including the resized and cancelled cases.
    patch.timed(4000, [("first-record", "bang"), ("recovered-record", "bang")])
    patch.timed(12500, [
        ("first-inspect", "bang"), ("recovered-inspect", "bang"),
        ("main-control", "freeze 0"),
    ])
    patch.timed(12750, [("main-snapshot", "bang")])
    patch.timed(13000, [("main-control", "frames 3"), ("main-control", "freeze 1")])
    patch.timed(15500, [("resized-record", "bang")])
    patch.timed(24000, [("resized-inspect", "bang")])
    patch.timed(24200, [("pd", "quit")])

    with tempfile.TemporaryDirectory(prefix="lfreeze-pd-") as directory:
        path = Path(directory) / "lfreeze.pd"
        patch.write(path)
        completed = subprocess.run(
            ["pd", "-noprefs", "-nogui", "-noaudio", "-stderr", "-r", str(SAMPLE_RATE),
             "-path", str(PACKAGE), str(path)],
            cwd=directory, capture_output=True, text=True, timeout=45,
        )
        output = completed.stdout + completed.stderr
        assert completed.returncode == 0, output
        assert not re.search(
            r"couldn't create|can't load|undefined symbol|connection failed|^error:", output, re.M
        ), output
        recordings = {label: wav_values(Path(directory) / f"{label}.wav")
                      for label in ("first", "resized", "recovered")}
    for label, frames in (("first", 2), ("resized", 3), ("recovered", 3)):
        saved = re.findall(rf"^SAVED_{label}: (\S+)$", output, re.M)
        assert saved == [str(cache_period(frames) * 2)], (label, saved, output)
        assert_repeats(recordings[label], cache_period(frames), frames * 2048, label)
    for label, count in (("main", 1), ("cancel", 2)):
        snapshots = re.findall(rf"^SNAPSHOT_{label}: (\S+)$", output, re.M)
        assert len(snapshots) == count, output
        assert all(float(sample) == 0 for sample in snapshots), (label, snapshots)
    print("x.lfreeze~ Pd: exact noisy loop repetition, fade-out, frame changes, cancellation and recapture passed")


if __name__ == "__main__":
    main()
