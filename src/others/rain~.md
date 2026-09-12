# rain~ — hybrid rain with a DAFx 2004 impact core

Reference: Stanley J. Miklavcic, Andreas Zita and Per Arvidsson,
*Computational Real-Time Sound Synthesis of Rain*, DAFx 2004, pp. 169–172.
[Paper](https://huelights.com/docs/P_169.pdf). Equation numbers in the code refer
only to this paper. The default hybrid mode adds clearly annotated perceptual
layers; `model paper` retains the equation-only comparison after its 30 ms
control smoothing settles.

## Hybrid sound-design layers (not equations from the paper)

The paper-derived impulse remains the initial impact. These additions give drops
body and continuous rain texture without making occasional pitched notes dominate:

| Control | Default | Effect |
| --- | --- | --- |
| `model hybrid` / `model paper` | hybrid | Blend into the perceptual mix or restore unaltered paper pressure. |
| `impact 0.25` | 0.25 | Level of the paper signal in the hybrid mix. |
| `splash 0.8` | 0.8 | Short filtered-noise splash with a soft attack and size-dependent tail. |
| `tone 0.06` | 0.06 | Level of three normalized, strongly damped surface resonances. Set 0 to remove them. |
| `resonance 6` | 6 ms | Reference decay time, scaled by material and varied per drop and mode; range 1–30 ms. Changes new events. |
| `material ground` | ground | Material response: plastic, roof, wood, dirt, asphalt (plus legacy ground and leaf). Independent of the paper's hard/water selection. |
| `plinks 0` | 0 | Optional quiet water-like tones on about 8% of detailed events. They are a sound-design effect, NOT Equation 3. |
| `bed 0.12` | 0.12 | Diffuse background made from six independently modulated noise bands. |
| `brightness 0.45` | 0.45 | Splash cutoff for new events and ongoing background spectral tilt. |

Choose a surface with one message (in hybrid mode):

| Message | Intended character |
| --- | --- |
| `material plastic` | Thin plastic bag: bright, irregular crinkle with a longer noisy tail. |
| `material roof` | Metal roof: sharp ticks with a longer, hollow metallic ring. |
| `material wood` | Wooden board: low, short knocks with a subdued splash. |
| `material dirt` | Dirt floor: soft, dull impacts with very little ringing. |
| `material asphalt` | Asphalt floor: crisp, grainy splatter with little tonal sustain. |

`plastic-bag`, `dirt-floor`, and `asphalt-floor` are also accepted names.
`ground` retains the original default profile; `leaf` remains available.
Materials shape impact level, splash attack, duration and filtering, resonance
strength and damping, and the background spectrum. They do not overwrite your
layer controls. New drops use the selected profile; existing tails finish with
their original profile, and background color blends over 30 ms. Material has no
effect in settled `model paper`. Use `tone` to bring out roof/wood resonance and
`splash` to emphasize plastic/asphalt texture.

All layer levels and brightness range from 0 to 1; layer gains smooth with a
30 ms time constant and affect active voices. Plinks must be enabled before an
event is created; their frequencies stay in 650–1600 Hz and decay time is 4 ms.
They do not require a cone waveform. The actual paper water model still does.

Hybrid arrivals use exponential waiting times: `density` is the average drops
per second, with natural clusters and gaps. A fixed `metro` sending bangs still
produces fixed timing; use automatic density for irregular rain.

Here the added resonances vary independently in tuning, damping and strength.
Larger drops excite lower, shorter resonances. Above a 1 mm radius, a smooth
per-event gain reduces the quadratic size growth toward twice the reference
excitation; smaller drops retain their original gain. This prevents occasional
large drops from dominating the mix. These are sound-design choices; `model
paper` retains the original size response and buffer-based arrival scheduling.

Every automatic paper impact is retained. Above 800 drops/s, a random subset
(about 800/s) gets longer splash/resonance tails to bound computation. Manual
bangs always get detail in hybrid mode. Capacity accounting still applies.
Texture randomness is separate from the paper event generator, and per-voice
noise generators keep splash evolution independent of voice ordering.

The background follows density and fades out at density 0. For isolated drops,
send `density 0`, allow the background to fade, then bang the object.
The external and `rain.pd` wrapper each have one mono signal outlet.

Suggested starting point:

```text
model hybrid
surface hard
material ground
radius 0.0005 0.003
impact 0.25
splash 0.8
tone 0.06
resonance 6
plinks 0
bed 0.12
brightness 0.45
density 800
```

Code comments explicitly say “Here I implement…” or “Here I add…” for these
perceptual layers. Their constants are tunable sound-design choices, not measured
material parameters or equations attributed to the paper. The hybrid output is
not calibrated pressure. Gain remains linear, so extreme settings can exceed
full scale. A listening comparison against reference recordings is still needed
to assess realism; passing DSP tests alone does not establish it.

## Equation-to-code map

| Paper | Implementation | Notes |
| --- | --- | --- |
| Section 2.1, Equation 1 | `rain_eq2`, `rain_prepare_water`, `rain_perform` | For a circular uniform step velocity, Equation 2 evaluates the boundary integral analytically. For water, differences of sampled surface velocity excite that same step response, implementing the integral by linear superposition. |
| Section 2.1, Equation 2 | `rain_eq2` | Direct pressure pulse with support from Rs/c to Rl/c. Its argument is time relative to Rs/c for numerical accuracy. |
| Section 2.2, Equation 3 | `rain_eq3`, `rain_prepare_water` | Uses supplied pinch-off velocity v0(t), cone length l, opening radius, and splay lambda. No bubble oscillator is substituted. |
| Section 3.1, Figure 3 | `rain_trigger`, `rain_perform` | Random arrival times in each Pd buffer; uniform source positions in an annulus; linear superposition of pressure. |
| Section 3.2 | Not implemented | Speaker spatialization is omitted from this mono external. |

Equation 2 is evaluated as

```text
p(t) = rho*c/pi * A(vterm)
       * acos((c²t² - H² + x0² - a²) / (2*x0*sqrt(c²t²-H²)))
Rs = sqrt((x0-a)²+H²)
Rl = sqrt((x0+a)²+H²)
p(t) = 0 outside [Rs/c, Rl/c]
```

The internal unit-step kernel uses A(vterm)=1; each hard impact multiplies it by
the explicit `amplitude` input. Source positions are restricted to x0>a. The
printed equation has a division by x0, and its stated Rs does not handle a disk
containing the listener axis. The implementation rejects such geometry instead
of inventing a special case.

For Equation 3, let s=opening_radius/lambda and q=t-l/c. The code computes

```text
z(q) = integral_0^q v0(q-tau) * exp(-c*tau/(s-l)) d tau
u(l,t) = (1-l/s)*v0(q) - c*l/s²*z(q)
```

A and a should not be confused: in Section 2.1, A(vterm) is a surface velocity
amplitude; in Section 2.2, A is the cone opening area. The interface accepts an
opening radius, so sqrt(A/pi)/lambda = opening_radius/lambda.

## What this paper leaves unspecified

- It names A(vterm) and relates it to impact energy, but supplies no formula to
  calculate it from terminal velocity. `amplitude` supplies A directly, in m/s.
  No terminal-velocity, kinetic-energy scaling or drop-size/loudness law is added.
- The water calculation depends on the pinch-off function v0(t); examples are
  referred to another work. `pinch` therefore requires a user-supplied velocity
  waveform. There is no invented default oscillation, delay, glide or damping.
- Quantitative conditions for bubble entrainment are not given. `surface water`
  assumes each triggered event satisfies those conditions. `surface hard` does
  not use the cone response. No random entrainment-probability law is inserted into the paper water model.
- Section 3.1 permits randomized sizes but gives no size distribution. The
  default radius range is 0.0005–0.002 m. Radius ranges are sampled uniformly,
  an explicit sampling choice rather than a claimed empirical rain distribution.
- Section 4 treats vibrating roofs and rustling leaves as additional future
  sound sources. Hybrid material presets are separate sound-design additions.

## Controls and units

Creation: `[rain~ 800]`; omitted, zero or negative creation arguments use 800.
Send `density 0` to disable automatic events. Bangs still work.

| Message | Meaning and initial value |
| --- | --- |
| `bang` | One event at the next DSP block. |
| `density 800` | Arrival count per second; range 0–10000. No 600/s cap or noise-bed substitution. |
| `radius 0.0005 0.002` | Default minimum/maximum drop **radius in meters**, not diameter or millimeters. Equal values give fixed size; maximum permitted radius is 0.02 m. |
| `area 0.5 5` | Inner/outer horizontal radii of the source annulus, meters. Inner must exceed both drop and opening radii. Equal limits specify a circular ring. |
| `height 1.7` | Listener height above the source plane, meters. |
| `amplitude 1` | A(vterm), the step surface velocity in m/s; applies to the initial impact. |
| `air 1.2 343` | Air density rho in kg/m³ and sound speed c in m/s. c is also the symbol used in Equation 3. |
| `gain 1` | Linear conversion from pressure to Pd sample amplitude. At gain 1, sample values numerically equal pressure in pascals. |
| `surface hard` | Initial hard-surface impact only; default. |
| `cone 0.002 0.002 0.5` | Cone length l (m), opening radius (m), and dimensionless splay lambda. Must satisfy s=radius/lambda>l. These example values are illustrative inputs, not paper estimates. |
| `pinch velocity-array 48000` | Copy array samples as v0(t) in m/s, at the stated Hz. Omitted/zero rate means current DSP rate. Requires 1–2048 finite samples. |
| `surface water` | Initial impact plus cone radiation; requires valid cone and pinch inputs first. |
| `seed 1234` | Reseed the random generator; zero maps to 1. |
| `status` | Print active voices and events dropped because of numerical/storage capacity. |

Scene defaults are explicit chosen input values, not numbers established by the
paper. `pitch`, `drops`, `distance`, and `saturation` remain unsupported. Hybrid
`tone`, `brightness` and `bed` are documented above. Use `gain` for output level.

For one drop every five seconds, send `density 0` and connect `[metro 5000]`
to `[rain~]`. No `bed 0` is needed. See `patches/rain-paper.pd` for a working patch.
Restart Pd after rebuilding to load the new class with its single mono signal outlet.

## Numerical and implementation notes — not additional physics

- Pd receives averages of pressure over sample intervals. Millimeter-scale
  pulses can be shorter than one sample. Eight-point Gauss-Legendre quadrature
  integrates Equation 2 over its overlap with each interval, preserving brief
  impacts instead of missing them. This is a numerical sampling approximation,
  not a claim of exact band-limited reconstruction. Peak/RMS values may change
  with sample rate even when time-integrated pressure is preserved.
- v0 is represented by held samples. The Equation 3 exponential convolution
  state is integrated exactly for each held input interval. Its resulting u is
  then represented by held samples for Equation 1. This second approximation
  converges as time resolution increases; it is not the exact continuous-time
  water waveform. Changing rate resamples the original supplied v0 by holding
  its input values. Prepare reference waveforms at the intended DSP rate.
- The geometric propagation times and l/c cone delay are retained relative to
  the first arriving component. The first arrival is placed in the current
  buffer, following Section 3.1's arrival-time scheduling.
- In `model paper`, buffer event counts use density * buffer_duration with fractional carry;
  arrival times within a buffer are uniform. The paper does not specify a law
  for random buffer counts. At sparse densities this produces roughly periodic
  intervals with buffer-scale jitter, not exponential waiting times.
- Uniform annular placement uses the inverse area CDF. PRNG, quadrature nodes,
  floating-point guards, array resampling and buffer bookkeeping are numerical
  tools, and are annotated as such in the source.
- Capacity: 512 simultaneous voices, 128 samples per geometric kernel, and
  16384 samples for the cone velocity response. A cone tail below 1e-9 of input
  peak is terminated with a final step to zero. Oversize responses are rejected;
  voice exhaustion is counted by `status`. No random voice stealing occurs.
- Supported DSP rates: 1000–384000 Hz. Updating air, cone or pinch configuration,
  or changing DSP sample rate, clears existing voices; these are scene setup
  controls. Gain changes are immediate. Other scene controls affect new events.
- In model paper there is no added noise bed, oscillator, saturation, compressor,
  EQ or pitch shift. Hybrid additions are described above. Output is mono.

## Validation

Build with `ninja -C build rain_tilde`.

```sh
c++ -std=c++17 -O2 -ffunction-sections -fdata-sections -I/usr/include/pd \
    tests/rain_dsp.cpp -Wl,--gc-sections -o /tmp/rain_dsp_test
/tmp/rain_dsp_test
```

Tests compare Equation 2 against its literal formula, compare integrated pressure
against an independent numerical disk integral of Equation 1, test sub-sample
pulse area, compare Equation 3 against a closed-form constant-input solution,
and check silence, bangs, linear amplitude scaling,
configuration errors, finite output and explicit capacity accounting. They do
not establish perceptual equivalence to the authors' demonstration recordings.
