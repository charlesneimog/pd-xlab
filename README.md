# pd-xlab

**pd-xlab** is a collection of objects, tools, and integrations for **Pure Data (Pd)** that I use in my work. It focus on Signal Manipulation, Statistics, Music Information Retrieval (MIR), Python/Lua scripting, and include all my libraries: 

* `pd-ambi`,
* `pd-saf`, 
* `o.scofo~`,
* `pd-partialtrack`, 
* `pd-onnx`,

* `py4pd`
* `pd-lua`

* `pd-orchidea`
* `pd-upic`,

---

## Object names

Objects maintained in this repository use the `x.` prefix across compiled externals,
Lua/Python objects, abstractions, and help patches:

| Group | Examples |
| --- | --- |
| Utilities | `x.click`, `x.cputime`, `x.curve~`, `x.darray~` |
| Statistics | `x.entropy`, `x.euclidean`, `x.kalman`, `x.kl` |
| Audio | `x.tsf~`, `x.fdn~`, `x.freeze~`, `x.gain~` |
| Arrays | `x.array.rotate`, `x.array.invert`, `x.array.sum` |
| Sets | `x.set.union`, `x.set.intersection` |
| Just intonation | `x.ji.hexany`, `x.ji.mos` |
| GUI | `x.gui.keyboard`, `x.gui.plot`, `x.gui.granulator` |

The library loader remains `[xlab]` / `[declare -lib xlab]`. Third-party libraries
keep their upstream names. `x.click` retains its signal output despite having no
tilde in its requested name.

Existing patches must use the new object names; see the complete
[old-to-new mapping](resources/object-renames.json). Source files with object
registrations use the object name as their basename, and help files use
`<object>-help.pd`. Internal support modules retain their existing filenames.
Experimental sources outside the CMake build remain outside the build.

---

## 📦 Features

* **Statistics objects** (`statistics`)
* **Array and signal manipulation** (`arrays`, `manipulations`)
* **MIR and onset detection** (`mir`, `x.onsetsds~`)
* **Python and Lua integration** via [py4pd](https://github.com/py4pd) and `pd_lua`
* **External plugins** such as `patcherize`
* **Spatial Audio Framework (SAF)** support:
  * `saf.encoder~`, `saf.decoder~`, `saf.binaural~`, `saf.roomsim~`, `saf.pitchshifter~`, `saf.binauraliser~`
* **Included abstractions and help patches** (`Abstractions/`, `Help-Patches/`)

---

## 🔨 Build Instructions

### 1. Clone the repository

```bash
git clone https://github.com/user/pd-xlab.git
cd pd-xlab
```

### 2. Create a build directory

```bash
cmake . -B build
cmake --build build
```

Compiled Pd objects and binaries will be available in the output directory (`build/xlab`).

### CloudSeed reverb

`[x.cloudseed.rev~]` uses the MIT-licensed DSP from
[Ghost Note Audio's CloudSeedCore](https://github.com/GhostNoteAudio/CloudSeedCore).
CMake fetches a pinned source revision and builds it with `pd_add_external`:

```bash
cmake -S . -B build
cmake --build build --target x.cloudseed.rev_tilde
```

Connect stereo audio to its two signal inputs; the two outputs are left/right.
The optional creation argument sets `mix` (0 = dry, 1 = wet, default 1).
It starts with Dark Plate and `DryOut` muted. Send `LateLineDecay 0.63` or
`param LateLineDecay 0.63` to change a parameter on CloudSeedCore's normalized
0–1 scale. `param 28 0.63` uses the matching upstream index. `parameters` prints
all 45 names, indices, current values, and scaled values in the Pd console.
`clear` erases the tail; `reset` restores the starting preset and `mix 1`.
See `x.cloudseed.rev~-help.pd` for a playable example. Supported sample rates
are 8–192 kHz; core processing uses 32-bit floats in either Pd sample precision.
This independent Pd integration is not a Ghost Note Audio product. Its MIT
notice is included as `CloudSeedCore-LICENSE.txt` in the built package.

### Publish to Deken

Set the repository's `DEKEN_PASSWORD` Actions secret for the `charlesneimog`
Deken account. Push a version tag such as `v0.1.1`; the workflow sets
`LIBVERSION` to that exact tag name. After all three platform builds succeed, it
merges their artifacts, uploads the package to Deken, and publishes a GitHub
Release containing the `.dek` package and the all-platforms ZIP. Normal pushes
only build and upload Actions artifacts.

---

## 📜 License

This project follows the licensing of its integrated components.
Check the individual components (`SAF`, `py4pd`, `FFTW3`) for detailed license information.
