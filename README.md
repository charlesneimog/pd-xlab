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

### Publish to Deken

Set the repository's `DEKEN_PASSWORD` Actions secret for the `charlesneimog`
Deken account and update `LIBVERSION` in
`.github/workflows/cmake-multi-platform.yml` for the release. Run **Build and
publish Pure Data package** manually with `publish_release` enabled. After all
three platform builds succeed, the workflow merges their artifacts, uploads the
package to Deken, and creates a draft GitHub Release containing the `.dek` package
and the all-platforms ZIP. Normal pushes only build and upload Actions artifacts.

---

## 📜 License

This project follows the licensing of its integrated components.
Check the individual components (`SAF`, `py4pd`, `FFTW3`) for detailed license information.
