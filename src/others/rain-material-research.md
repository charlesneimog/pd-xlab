Research audit of the seven `rain~` material profiles, 12 September 2026.

The existing coefficients are artistic presets. The research reviewed here does
not establish replacement frequency triples, gains, or filter settings for all
seven materials. Greater realism requires identifying the actual object or
surface, separating its response from the drop excitation, and fitting against
recordings. No new measured coefficients or listening validation are claimed.

This audit covers the current `RainMaterial` table and its use in `rain_trigger`
and `rain_background`. Recommendations below are engineering proposals, not
implementations or results of an acoustic calibration.

**What the evidence supports for each material**

| Preset | Evidence and scope | Recommended interpretation and implementation |
| --- | --- | --- |
| `ground` | The label does not identify a substrate. Beacham et al. measured dry masonry, dry/wet aluminum, and water; those cannot supply universal ground coefficients. [Surface experiments](https://doi.org/10.1016/j.expthermflusci.2020.110138). | Keep this as a legacy generic mix, or explicitly define a target such as rough masonry with a thin water film. Record that target before assigning resonances or band weights. Avoid describing `{470, 1130, 2190}` as measured ground modes. |
| `leaf` | Experiments show that drop impacts excite leaf movement and can eject secondary droplets. Leaf mechanics are not themselves an airborne audio spectrum. [Gilet and Tadrist, 2025](https://doi.org/10.1103/PhysRevFluids.10.053601). | Specify a leaf species/size and attachment. Separate the primary impact, leaf motion, and any subsequent droplet contacts. Fit audible response from microphone recordings; do not substitute measured mechanical oscillation rates for the current kHz resonances. |
| `roof` | Rainfall-noise models include drop forces and the vibrating panel response. The reference-panel work specifies dimensions and mounting and reports only moderate prediction/measurement agreement. [Schmid et al., 2021](https://perswww.kuleuven.be/~u0044091/ij-aa-schm-21a.postprint.pdf). | Define a particular metal panel, thickness, supports, and listener side. Use a stable set of panel modes with impact-position-dependent excitation. Fit decay separately for each mode; three arbitrarily retuned modes should not be treated as a roof model. |
| `plastic` | Deformed, crumpled Mylar emits discrete clicks. That experiment concerns strained film, not raindrops on arbitrary plastic. Rain on tensioned ETFE membranes is separately modeled as structural radiation. [Kramer and Lobkovsky, 1996](https://doi.org/10.1103/PhysRevE.53.1465), [Toyoda and Takahashi, 2013](https://doi.org/10.1016/j.apacoust.2013.05.013). | Keep `plastic-bag` explicitly a loose, creased-film target. Test occasional short click bursts rather than treating continuous flutter as physical crinkling. A taut tarp or rigid plastic sheet needs a different response; rainfall alone does not justify crinkle on every impact. |
| `wood` | Recorded impact experiments support both frequency-dependent decay and spectral structure as material cues; changing decay alone is insufficient. These are object-impact studies, not a raindrop coefficient table. [Aramaki et al., author demonstrations](https://kronland.fr/publications/controlling-the-perceived-material-in-an-impact-sound-synthesizer/). | Define a mounted board, including dimensions and species. Fit its modes and individual decays. Change modal excitation with drop position/size while retaining the board's identity. A hollow wooden box should be a separate target. |
| `dirt` | Soil experiments used 4.279 mm diameter drops falling 1.5 m. Sound varied with grain composition and moisture; sandier specimens were louder. The longest reported fading signal was 36 ms, and the 27–42 dB levels used a 40 ms averaging window at 1 m. [Ryżak et al., 2016](https://doi.org/10.1371/journal.pone.0158472). | Choose a defined soil texture and moisture state. Fit the transient and its decay; do not equate 36 ms with an exponential time constant. These measurements do not justify the present 1.33 kHz splash cutoff or the three low resonances. |
| `asphalt` | This search did not find a directly usable, calibrated airborne single-drop spectrum/decay dataset for asphalt. Wet concrete/masonry measurements offer an adjacent reference, not an asphalt calibration. [Surface experiments](https://doi.org/10.1016/j.expthermflusci.2020.110138). | Specify aggregate texture, porosity, and water-film state. Retain the existing preset as provisional until it is recorded. Do not import tire/pavement noise spectra or absorption coefficients as drop-emission spectra. |

Beacham et al. report surface-dependent airborne spectral features in the
1–20 kHz range. Their deep-water example contains an initial event around 10 kHz
and another around 4 kHz roughly 50 ms later. Those are condition-specific
observations, not frequencies or delays to apply to every wet material.
[Surface experiments](https://doi.org/10.1016/j.expthermflusci.2020.110138).

**What the current numbers actually produce**

The following values are calculated from the repository code, not literature.
They assume 48 kHz, a 1 mm drop radius, `brightness 0.45`, and `resonance 6`.
The resonance column evaluates both decay random factors at their midpoints;
individual events vary. Density 0 means a manually triggered drop.

| Material | Splash low-pass setting (Hz) | Splash decay constant at density ≥150 (ms) | Splash decay constant at density 0 (ms) | Representative lowest-mode decay constant (ms) |
| --- | ---: | ---: | ---: | ---: |
| ground | 3540 | 4.67 | 1.87 | 4.72 |
| leaf | 5310 | 4.67 | 1.87 | 3.30 |
| roof | 4867.5 | 3.11 | 1.24 | 11.79 |
| plastic | 6195 | 15.56 | 6.22 | 3.07 |
| wood | 2212.5 | 3.11 | 1.24 | 5.19 |
| dirt | 1327.5 | 6.22 | 2.49 | 1.65 |
| asphalt | 5088.75 | 7.78 | 3.11 | 1.42 |

An exponential amplitude envelope falls by 60 dB in approximately `6.908 * tau`.
Thus the dense plastic splash's envelope takes roughly 107 ms to fall by 60 dB
in this example. Its table entry `0.010` does not mean a 10 ms total sound.
The renderer allocates detail for seven times the longest splash/resonance time
constant, with a possible extension for plinks. Filtering, attack, masking, and
the other layers affect the audible duration.

For the default 0.5–2 mm radius range, `size_pitch`, global jitter, and mode
jitter together allow roughly 0.45–2.03 times each listed frequency, before the
sample-rate cap. Each drop can therefore excite a substantially different set
of pitches even on the same nominal roof or board. This is a code observation.
For a fixed object modeled with linear modes, the proposed correction is to
vary modal amplitudes with excitation while retaining the object's frequencies;
object-to-object variation should be chosen separately.

**How to calibrate every field**

| Current field | Research-informed treatment |
| --- | --- |
| `name` | Identify a reproducible target, not only a substance: mounted metal sheet, wooden board, loose film, specified soil, or specified leaf. Preserve existing names as aliases if new targets are added. |
| `frequencies[3]` | Estimate persistent spectral modes from repeated impacts on that target. Check whether three modes reproduce it adequately. Broadband surfaces may not warrant three audible pitched modes. |
| `damping` | Rename internally to `decay_scale` to reflect its actual direction. Prefer per-mode decay constants or a fitted frequency-dependent decay law. The current common division by `1 + 0.6*k` is an artistic assumption shared by all materials. |
| `tone_gain` | Fit modal amplitudes relative to the recorded transient. A single multiplier cannot fix incorrect relative modal strengths; allow separate mode gains if necessary. |
| `splash_gain` | Fit the nonmodal residual after accounting for the initial impact and identifiable structural modes. Jointly fit gain and decay rather than making every material louder. |
| `splash_seconds` | Fit the residual envelope, examining whether a single exponential is adequate. Keep delayed contacts distinct from a long noise tail. |
| `attack_seconds` | Estimate onset from aligned recordings. In this code it controls splash and surface-ring attack, not the initial paper transient. Account for microphone bandwidth and sample rate. |
| `cutoff` | Fit the splash spectral shape jointly with `highpass`. It is a multiplier of `1500 + 6500*brightness`, not a measured material cutoff in Hz. Calibrate with a fixed brightness setting. |
| `highpass` | Fit from the residual spectrum rather than assuming a dark material needs an arbitrary high-pass. It currently filters splash only. |
| `crinkle` | Treat as a film-specific event mechanism if the recording contains clicks; estimate click probability, delay, spectrum, and energy distribution. Current filtered-noise flutter is a texture approximation. |
| `impact_gain` | Fit relative pressure or playback amplitude at a common microphone position. The renderer's artistic body scaling means laboratory dB values cannot be inserted directly. |
| `bed_weights[6]` | Fit the output of the actual overlapping filter bank to a continuous-rain recording after accounting for individually rendered impacts. These are amplitude weights, not six independent measured band powers. Equal values do not guarantee a flat spectrum. |

This fitting approach is an engineering proposal. The material-perception work
supports considering spectral structure and frequency-dependent damping, but
does not supply the values of these custom fields.
[Author demonstrations and sound examples](https://kronland.fr/publications/controlling-the-perceived-material-in-an-impact-sound-synthesizer/).

**Changes with the strongest practical justification**

1. Separate drop excitation from object response. For roof and wood, represent
   the target with stable modal frequencies and fitted per-mode decay/gain.
   Preserve variation in strike position and strength; choose a different modal
   set only when representing a different object. A bounded modal bank can be
   evaluated before considering a more expensive structural solver.
2. Separate loose-film clicks, leaf/secondary-droplet events, and ordinary
   splashes instead of expressing all three as variations of one noise envelope.
   Their event rates and delays need reference recordings.
3. Introduce explicit surface-state targets: dry, water film, and puddled where
   appropriate. Do not infer water-film depth directly from `density`; drainage
   and exposure history are absent from the current model. The existing
   `surface water` cone calculation is not a generic wet-material control.
4. Calibrate individual events first, then the background. This makes sparse
   rain a diagnostic condition instead of hiding mismatches under overlapping
   events and a separately generated noise bed.
5. Keep the recent density-dependent timbre adjustment documented as artistic.
   It can improve a listening impression, but no reviewed source establishes a
   physical timbre transition at 150 drops/s. A calibrated path should reproduce
   isolated drops without depending on that threshold. Preserve the current
   hybrid path for comparison with the dense sound already preferred.

The original DAFx paper remains the source for the existing pressure-model
comparison. Its impact and water-source framework does not calibrate these seven
added material presets. Keep any new perceptual mechanism separately identified.
[Miklavcic, Zita and Arvidsson, 2004](https://huelights.com/docs/P_169.pdf).

**Proposed recording and acceptance procedure**

Use repeated isolated drops on each defined specimen, with fixed microphone
position and gain. Record drop diameter and fall conditions; do not confuse
drop diameter with this external's `radius`, or fall height with its listener
`height`. For panels, document mounting and which side the microphone hears.
Include dry and wet conditions separately, and several impact positions.

As a practical starting protocol, collect at least 30 isolated impacts per
condition and continuous-rain excerpts. That count is a proposed workflow, not
a literature-derived sufficiency threshold. Keep some recordings out of fitting
for validation. Record clean background noise and avoid recorder automatic gain
control. Check permissions before redistributing any external audio assets.

Fit transient rise, decay by frequency band, persistent modes, modal amplitudes,
and residual spectra. Compare both unnormalized levels (where recording
calibration allows) and loudness-matched listening examples. Evaluate multiple
events rather than fitting a single lucky drop. Public wood/metal impact demos
can help assess modal synthesis, but their different excitation does not make
them calibrated rainfall references.

Then compare current and candidate versions at densities 2, 20, 100, 150, and
800 with the same radius range and seeds. These are proposed audition points,
not physical boundaries. Ask listeners to judge resemblance to the recorded
target as well as overall preference. Check that the sparse correction is not
merely replacing a noise puff with a pitched knock.

Retain finite-output, reproducibility, sample-rate, capacity, CPU, and paper-mode
regression checks during implementation. Numerical tests can establish stable
rendering; acoustic measurements and listening comparisons establish whether a
candidate improves realism. No recordings were fitted or auditioned in this
research pass, and the existing material coefficients remain unchanged.
