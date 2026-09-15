"""Check naming and load renamed objects in Pd: python3 tests/object_names.py build/xlab."""

import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
PACKAGE = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build/xlab"
RENAMES = json.loads((ROOT / "resources/object-renames.json").read_text())
LEGACY = {old for old, new in RENAMES.items() if old != new}


def run_pd(arguments):
    result = subprocess.run(
        ["pd", "-noprefs", "-nogui", "-noaudio", "-stderr", "-path", str(PACKAGE), *arguments],
        capture_output=True,
        text=True,
        timeout=30,
    )
    output = result.stdout + result.stderr
    assert result.returncode == 0, output
    assert not re.search(r"error:|couldn't create|can't load|undefined symbol", output), output


native = []
for cmake in [ROOT / "CMakeLists.txt", *ROOT.glob("src/**/CMakeLists.txt")]:
    native.extend(re.findall(r"pd_add_external\(\s*(x\.[^\s)]+)", cmake.read_text()))

# Loading by the public name exercises Pd's filename and setup-symbol lookup.
run_pd([arg for name in native for arg in ("-lib", name)] + ["-send", "pd quit"])

for source in ROOT.glob("src/**/*"):
    if source.suffix not in (".c", ".cpp") or source.name == "xlab.cpp":
        continue
    text = source.read_text()
    registration = re.search(r'class_new\s*\(gensym\("([^"]+)"\)', text)
    registration = registration or re.search(r'#define OBJECT_NAME "([^"]+)"', text)
    if registration:
        name = registration[1]
        assert name.startswith("x.") and source.stem == name, source
        setup = "setup_" + name.replace(".", "0x2e").replace("~", "_tilde")
        assert re.search(r"\bvoid " + setup + r"\(", text), source
        if source.suffix == ".cpp":
            assert 'extern "C" void ' + setup in text, source

lua = sorted(ROOT.glob("src/**/*.pd_lua"))
for source in lua:
    name = re.search(r'pd.Class:new\(\):register\("([^"]+)"\)', source.read_text())[1]
    assert name == source.stem and name.startswith("x."), source
    assert (PACKAGE / source.name).read_bytes() == source.read_bytes(), source
    subprocess.run(["luac", "-p", str(source)], check=True)

# Construct all Lua objects with their required inputs to check module dispatch
# as well as filenames. The GUI granulator needs an existing array.
with tempfile.TemporaryDirectory(prefix="xlab-names-") as directory:
    patch = Path(directory) / "objects.pd"
    lines = ["#N canvas 0 0 450 300 12;", "#X obj 20 20 array define test-array 1024;"]
    examples = ["x.click", "x.cputime", "x.curve~", "x.tsf~", "x.entropy", "x.euclidean", "x.kalman"]
    for name in examples:
        lines.append(f"#X obj 20 30 {name};")
    for source in lua:
        args = " test-array" if source.stem == "x.gui.granulator" else ""
        lines.append(f"#X obj 20 40 {source.stem}{args};")
    count = len(lua) + len(examples) + 1
    lines.extend([
        "#X obj 20 80 loadbang;",
        r"#X msg 20 110 \; pd quit;",
        f"#X connect {count} 0 {count + 1} 0;",
    ])
    patch.write_text("\n".join(lines) + "\n")
    run_pd(["-lib", "lua", str(patch)])

for folder in ("src", "resources", "patches"):
    for source in (ROOT / folder).rglob("*.pd"):
        for name in re.findall(r"^#X obj \S+ \S+ (\S+)", source.read_text(), re.M):
            assert name.rstrip(";,").removeprefix("xlab/") not in LEGACY, (source, name)

for source in [*ROOT.glob("src/**/*.pd_py"), *ROOT.glob("resources/**/*.pd_py")]:
    text = source.read_text()
    name = re.search(r'^\s*name = "([^"]+)"', text, re.M)[1]
    assert name == source.stem and name.startswith("x."), source
    assert (PACKAGE / source.name).read_bytes() == source.read_bytes(), source
    compile(text, str(source), "exec")

for source in [*ROOT.glob("src/**/x.*-help.pd"), *ROOT.glob("resources/**/x.*-help.pd")]:
    # Packaging may rewrite references to third-party objects in help patches.
    assert (PACKAGE / source.name).is_file(), source

print(f"Passed: {len(native)} native loads, {len(lua)} Lua creations, Python syntax, and naming/help checks.")
