# Ports

`vb.freezer~` is a Pure Data port of Volker Boehm's Max external
(2011–2020, version 1.1.2), adapted from the supplied source.

Create `[vb.freezer~ array-name 250]` with a mono Pd array. The optional second
argument sets the rolling input recording duration in milliseconds (default 250).
Send a bang to freeze the recent input, or `freeze_buf` to process the current
array contents. Its length sets the loop/FFT size, from 2 to 16777216 samples;
arbitrary odd and even lengths work. Shorter arrays retain the latest input
samples that fit. Live captures use a short fade at each end before analysis.

The object writes a randomized-phase loop to the array; it has no signal output.
The left outlet reports the sample count, then the right outlet bangs when the
array is ready. Play it using `tabplay~` or another array reader. FFT work runs
on a background thread on desktop builds. Single-threaded WebAssembly builds
defer that work to the scheduler instead.

Messages:

- `set name`: change the destination array.
- `thresh n`: reject FFT magnitudes below this raw, unnormalized threshold.
- `bgain n`, `fgain n`: clamp the old-array and frozen-signal gains to 0–1.
  Both default to 1, matching the original. Use `bgain 0` to replace the loop.
- `clear`: zero the destination array.
- `info`: print the recording settings and processing status.

Requests received while processing are rejected. `set` and `clear` cancel a
pending write; resizing or deleting the array also prevents committing an
incompatible result. Only successful writes produce completion messages.

The port uses the project's double-precision FFTW build, exact sine/cosine,
per-instance random state, and Pd arrays in place of Max's multichannel buffers.
DC and the even-length Nyquist bin are removed, as in the original algorithm.
See `vb.freezer~-help.pd` for a complete example.
