# pd-xlab

**pd-xlab** is a collection of objects, tools, and integrations for **Pure Data (Pd)** that I use in my work. It focus on Signal Manipulation, Statistics, Music Information Retrieval (MIR), Python/Lua scripting, and include all my libraries: 
* `pd-partialtrack`, 
* `pd-saf`, 
* `pd-vamp`, 
* `pd-server`,
* `pd-ambi`,
* `pd-upic`,

and also some libraries I fork, update or simply use:
* `kalman-pd`
* `SOFAlizer-for-pd`
* `earplug~`

---

## 📦 Features

* **Statistics objects** (`statistics`)
* **Array and signal manipulation** (`arrays`, `manipulations`)
* **MIR and onset detection** (`mir`, `onsetsds~`)
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

---

## 📜 License

This project follows the licensing of its integrated components.
Check the individual components (`SAF`, `py4pd`, `FFTW3`) for detailed license information.
