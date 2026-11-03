"""Headless integration checks: python3 tests/freezer_pd.py [build/xlab]."""

from pathlib import Path
import cmath
import math
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
PACKAGE = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build/xlab"


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

    def array(self, name, length):
        self.add("obj", f"array define {name} {length}")

    def freezer(self, label, name, record_ms=20):
        receiver = self.add("obj", f"r {label}-control")
        freezer = self.add("obj", f"vb.freezer~ {name} {record_ms}")
        self.connect(receiver, 0, freezer)
        size = self.add("obj", f"print SIZE_{label}")
        done = self.add("obj", f"print DONE_{label}")
        get = self.add("obj", f"array get {name}")
        data = self.add("obj", f"print DATA_{label}")
        inspect = self.add("obj", f"r {label}-inspect")
        self.connect(freezer, 0, size)
        self.connect(freezer, 1, done)
        self.connect(freezer, 1, get)
        self.connect(inspect, 0, get)
        self.connect(get, 0, data)
        return freezer

    def deletion_subpatch(self):
        # clear deletes the receiving object immediately after it started a
        # background job; its destructor must join before freeing worker state.
        self.lines.extend([
            "#N canvas 0 0 300 200 victim 0;",
            "#X obj 10 10 r delete-control;",
            "#X obj 10 40 vb.freezer~ delete-array;",
            "#X connect 0 0 1 0;",
            "#X restore 20 20 pd victim;",
        ])
        self.count += 1

    def write(self, path):
        path.write_text("\n".join(self.lines + self.connections) + "\n")


def values(output, label):
    matches = re.findall(rf"^DATA_{re.escape(label)}: (.*)$", output, re.M)
    assert len(matches) == 1, f"Expected one array for {label}:\n{output}"
    return [float(value) for value in matches[0].split()]


def assert_silent(samples, tolerance=1e-8):
    assert max(map(abs, samples), default=0) <= tolerance, samples


def magnitude(samples, bin):
    return abs(sum(value * cmath.exp(-2j * math.pi * bin * i / len(samples))
                   for i, value in enumerate(samples)))


def main():
    patch = Patch()
    definitions = {
        "odd": 17,
        "two": 2,
        "one": 1,
        "preserve": 5,
        "lower": 7,
        "threshold": 13,
        "clear": 5,
        "set": 257,
        "new": 257,
        "cancel": 257,
        "resize": 257,
        "capture": 257,
        "latest": 127,
    }
    objects = {}
    for label, length in definitions.items():
        patch.array(f"{label}-array", length)
        objects[label] = patch.freezer(label, f"{label}-array")
    patch.array("delete-array", 65536)
    patch.deletion_subpatch()

    oscillator = patch.add("obj", "osc~ 220")
    gate = patch.add("obj", "*~ 1")
    gate_control = patch.add("obj", "r gate-control")
    patch.connect(oscillator, 0, gate)
    patch.connect(gate_control, 0, gate, 1)
    patch.connect(gate, 0, objects["capture"])
    patch.connect(gate, 0, objects["latest"])
    patch.timed(0, [("pd", "dsp 1")])

    setup = [(f"{label}-array", "0 1") for label in definitions]
    setup += [
        ("preserve-array", "0 1 -2 3 -4 5"),
        ("new-array", "0 0.25"),
        ("delete-array", "0 1"),
        # Both upper gain clamps: FFT magnitudes must remain exactly one when
        # fgain=2 is clamped to 1; the separate preserve case exercises bgain.
        ("odd-control", "bgain 0"),
        ("odd-control", "fgain 2"),
        ("odd-control", "thresh -10"),
        ("odd-control", "freeze_buf"),
        ("two-control", "bgain 0"),
        ("two-control", "freeze_buf"),
        ("one-control", "freeze_buf"),
        ("preserve-control", "bgain 2"),
        ("preserve-control", "fgain -1"),
        ("preserve-control", "freeze_buf"),
        ("lower-control", "bgain -1"),
        ("lower-control", "fgain -1"),
        ("lower-control", "freeze_buf"),
        ("threshold-control", "bgain 0"),
        ("threshold-control", "thresh 1e+06"),
        ("threshold-control", "freeze_buf"),
        ("clear-control", "clear"),
        # All cancellation changes happen in this same message dispatch, before
        # any completion clock can publish a worker's output.
        ("set-control", "freeze_buf"),
        ("set-control", "set new-array"),
        ("cancel-control", "freeze_buf"),
        ("cancel-control", "clear"),
        ("resize-control", "freeze_buf"),
        ("resize-array", "resize 129"),
        ("delete-control", "freeze_buf"),
        ("pd-victim", "clear"),
    ]
    patch.timed(50, setup)
    patch.timed(250, [("capture-control", "bgain 0"), ("capture-control", "bang")])
    patch.timed(400, [("gate-control", "0")])
    # The complete recording ring is now recent silence. This must replace the
    # array's initial impulse, including when the array is shorter than the ring.
    patch.timed(750, [("latest-control", "bgain 0"), ("latest-control", "bang")])
    patch.timed(1300, [(f"{label}-inspect", "bang")
                       for label in ("one", "clear", "set", "new", "cancel", "resize")])
    patch.timed(1500, [("pd", "quit")])

    with tempfile.TemporaryDirectory(prefix="vb-freezer-pd-") as directory:
        path = Path(directory) / "freezer.pd"
        patch.write(path)
        completed = subprocess.run(
            ["pd", "-noprefs", "-nogui", "-noaudio", "-stderr", "-r", "48000",
             "-path", str(PACKAGE), str(path)],
            capture_output=True, text=True, timeout=20,
        )
    output = completed.stdout + completed.stderr
    assert completed.returncode == 0, output
    assert not re.search(r"couldn't create|can't load|undefined symbol|connection failed", output), output
    errors = re.findall(r"^error: (.*)$", output, re.M)
    expected_errors = (
        "[vb.freezer~] array size must be between 2 and 16777216 samples",
        "[vb.freezer~] array resized during processing; result discarded",
    )
    assert sorted(errors) == sorted(expected_errors), output

    successful = {"odd", "two", "preserve", "lower", "threshold", "capture", "latest"}
    for label, length in definitions.items():
        done = re.findall(rf"^DONE_{label}: bang$", output, re.M)
        sizes = re.findall(rf"^SIZE_{label}: (\S+)$", output, re.M)
        assert len(done) == int(label in successful), output
        assert sizes == ([str(length)] if label in successful else []), output

    odd = values(output, "odd")
    assert len(odd) == 17
    assert magnitude(odd, 0) < 1e-5
    for bin in range(1, 9):
        assert abs(magnitude(odd, bin) - 1) < 1e-5, (bin, odd)
    assert values(output, "preserve") == [1, -2, 3, -4, 5]
    for label in ("two", "lower", "threshold", "clear", "cancel", "latest"):
        assert_silent(values(output, label))
    assert values(output, "one") == [1]
    assert values(output, "set") == [1] + [0] * 256
    assert values(output, "new") == [0.25] + [0] * 256
    assert values(output, "resize") == [1] + [0] * 128
    capture = values(output, "capture")
    assert len(capture) == 257
    assert sum(sample * sample for sample in capture) > 0.1, output
    assert abs(sum(capture)) < 1e-5, output
    print("vb.freezer~ Pd: async arrays/outlets, gains, threshold, capture, cancellation, resize and deletion passed")


if __name__ == "__main__":
    main()
