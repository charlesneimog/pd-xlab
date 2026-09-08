#!/usr/bin/env python3
import argparse
from pathlib import Path

import numpy as np
import matplotlib.pyplot as plt
import skrf as rf


def air_properties(temp_c: float):
    # Same simple approximation used by the external is sufficient here.
    c = 331.3 + 0.606 * temp_c
    # Ideal-gas approximation around standard atmospheric pressure.
    rho = 1.2041 * (293.15 / (273.15 + temp_c))
    return rho, c


def load_impedance_csv(path: str):
    data = np.genfromtxt(path, delimiter=",", names=True, dtype=float, encoding=None)
    if data.dtype.names is None:
        raise ValueError("CSV must have a header row")

    names = {name.lower(): name for name in data.dtype.names}

    def col(*candidates):
        for candidate in candidates:
            if candidate.lower() in names:
                return np.asarray(data[names[candidate.lower()]], dtype=float)
        return None

    f = col("frequency_hz", "freq_hz", "frequency", "freq", "f")
    if f is None:
        raise ValueError("Missing frequency column (e.g. frequency_hz)")

    zr = col("z_real", "real", "re")
    zi = col("z_imag", "imag", "im")

    if zr is not None and zi is not None:
        z = zr + 1j * zi
    else:
        mag = col("z_mag", "magnitude", "mag", "abs")
        phase_deg = col("z_phase_deg", "phase_deg", "phase")
        if mag is None or phase_deg is None:
            raise ValueError(
                "Need either z_real,z_imag or magnitude,phase_deg columns"
            )
        z = mag * np.exp(1j * np.deg2rad(phase_deg))

    valid = np.isfinite(f) & np.isfinite(z.real) & np.isfinite(z.imag) & (f > 0)
    f = f[valid]
    z = z[valid]

    order = np.argsort(f)
    f = f[order]
    z = z[order]

    # Remove duplicate frequency samples.
    f_unique, idx = np.unique(f, return_index=True)
    return f_unique, z[idx]


def build_exact_modal_matrix(freqs_hz, poles):
    """Matrix for exactly the model implemented by clarinet~.

    Zhat(s) = sum_k [C_k/(s-p_k) + conj(C_k)/(s-conj(p_k))]

    Unknown vector is [Re(C1), Im(C1), Re(C2), Im(C2), ...].
    """
    s = 1j * 2.0 * np.pi * freqs_hz
    A = np.empty((len(freqs_hz), 2 * len(poles)), dtype=complex)

    for k, p in enumerate(poles):
        g = 1.0 / (s - p)
        gc = 1.0 / (s - np.conj(p))
        A[:, 2 * k] = g + gc
        A[:, 2 * k + 1] = 1j * (g - gc)

    return A


def fit_residues_exact(freqs_hz, zhat, poles, weight_floor=0.05):
    A = build_exact_modal_matrix(freqs_hz, poles)

    # Relative-ish weighting: prevents the tall impedance peaks from completely
    # dominating the anti-resonances, while avoiding huge weights near zeros.
    scale = np.max(np.abs(zhat))
    floor = max(weight_floor * scale, 1e-12)
    weights = 1.0 / np.sqrt(np.abs(zhat) ** 2 + floor ** 2)

    Aw = A * weights[:, None]
    bw = zhat * weights

    Ar = np.vstack((Aw.real, Aw.imag))
    br = np.concatenate((bw.real, bw.imag))

    x, *_ = np.linalg.lstsq(Ar, br, rcond=None)
    residues = x[0::2] + 1j * x[1::2]
    fit = A @ x
    return residues, fit


def main():
    parser = argparse.ArgumentParser(
        description="Fit measured clarinet input impedance to clarinet~ modal poles/residues"
    )
    parser.add_argument("csv", help="CSV with frequency_hz,z_real,z_imag")
    parser.add_argument("--modes", type=int, default=16, help="complex pole pairs (max 32)")
    parser.add_argument("--fmin", type=float, default=50.0)
    parser.add_argument("--fmax", type=float, default=5000.0)
    parser.add_argument("--radius", type=float, default=0.0075, help="bore radius at input [m]")
    parser.add_argument("--temperature", type=float, default=20.0, help="deg C")
    parser.add_argument(
        "--weight-floor",
        type=float,
        default=0.05,
        help="relative weighting floor; try 0.02..0.2",
    )
    parser.add_argument("--prefix", default="clarinet_fit")
    args = parser.parse_args()

    if not (1 <= args.modes <= 32):
        raise SystemExit("--modes must be between 1 and 32")

    f, zin = load_impedance_csv(args.csv)
    select = (f >= args.fmin) & (f <= args.fmax)
    f = f[select]
    zin = zin[select]
    if len(f) < 20:
        raise SystemExit("Too few samples in selected fitting band")

    rho, c = air_properties(args.temperature)
    area = np.pi * args.radius**2
    zc = rho * c / area

    # Fit a dimensionless impedance. This is numerically much better and maps
    # directly onto the normalization used inside clarinet~.
    zhat = zin / zc

    freq = rf.Frequency.from_f(f, unit="hz")
    zmat = zhat[:, None, None]
    ntw = rf.Network(frequency=freq, z=zmat, z0=1.0)

    vf = rf.VectorFitting(ntw)
    vf.vector_fit(
        n_poles_real=0,
        n_poles_cmplx=args.modes,
        init_pole_spacing="log",
        parameter_type="z",
        enforce_dc=False,
    )

    # scikit-rf stores only one pole of each conjugate pair. Keep physically
    # useful, stable resonant poles in the fitting band.
    poles = np.asarray(vf.poles, dtype=complex)
    poles = poles[(poles.imag > 0.0) & (poles.real < 0.0)]
    pole_freqs = poles.imag / (2.0 * np.pi)
    keep = (pole_freqs >= args.fmin * 0.5) & (pole_freqs <= args.fmax * 1.2)
    poles = poles[keep]
    poles = poles[np.argsort(poles.imag)]

    if len(poles) == 0:
        raise SystemExit("Vector fitting returned no usable complex poles")
    if len(poles) > 32:
        poles = poles[:32]

    # Important: ignore scikit-rf's d + e*s residues and re-fit C using exactly
    # the modal form implemented by clarinet~.
    residues, zhat_fit = fit_residues_exact(
        f, zhat, poles, weight_floor=args.weight_floor
    )

    zfit = zhat_fit * zc

    f_modes = poles.imag / (2.0 * np.pi)
    q_modes = poles.imag / (-2.0 * poles.real)
    f0 = f_modes[0]
    ratios = f_modes / f0

    # clarinet~ uses actual normalized modal residue:
    #   R_n = Zc * (4*f0) * (Cre + j*Cim)
    # Since we fitted Z/Zc, our residues are R_n/Zc.
    c_external = residues / (4.0 * f0)

    numerator = np.sqrt(np.mean(np.abs(zfit - zin) ** 2))
    denominator = max(np.sqrt(np.mean(np.abs(zin) ** 2)), 1e-30)
    nrms = numerator / denominator

    prefix = Path(args.prefix)
    txt_path = prefix.with_suffix(".pd.txt")
    npz_path = prefix.with_suffix(".npz")
    png_path = prefix.with_suffix(".png")

    with open(txt_path, "w", encoding="utf-8") as fp:
        fp.write(f"# fitted from {args.csv}\n")
        fp.write(f"# Zc = {zc:.12g} Pa*s/m^3\n")
        fp.write(f"# normalized RMS error = {nrms:.6g}\n")
        fp.write("resetmodes\n")
        fp.write(f"boreradius {args.radius:.12g}\n")
        fp.write(f"temperature {args.temperature:.12g}\n")
        fp.write(f"freq {f0:.12g}\n")
        fp.write(f"modes {len(poles)}\n")
        for i, (ratio, q, ce) in enumerate(zip(ratios, q_modes, c_external), start=1):
            fp.write(
                f"mode {i} {ratio:.12g} {q:.12g} "
                f"{ce.real:.12g} {ce.imag:.12g}\n"
            )

    np.savez(
        npz_path,
        frequency_hz=f,
        zin=zin,
        zfit=zfit,
        zc=zc,
        poles=poles,
        residues_normalized=residues,
        external_coefficients=c_external,
        f0=f0,
        q=q_modes,
        ratios=ratios,
        nrms=nrms,
    )

    fig, ax = plt.subplots(2, 1, figsize=(9, 7), sharex=True)
    ax[0].plot(f, np.abs(zin), label="measured")
    ax[0].plot(f, np.abs(zfit), "--", label="fit")
    ax[0].set_ylabel("|Zin| [Pa s / m^3]")
    ax[0].set_yscale("log")
    ax[0].legend()
    ax[0].grid(True, alpha=0.25)

    ax[1].plot(f, np.unwrap(np.angle(zin)), label="measured")
    ax[1].plot(f, np.unwrap(np.angle(zfit)), "--", label="fit")
    ax[1].set_xlabel("Frequency [Hz]")
    ax[1].set_ylabel("phase [rad]")
    ax[1].grid(True, alpha=0.25)
    fig.tight_layout()
    fig.savefig(png_path, dpi=160)

    print(f"Zc: {zc:.6g} Pa*s/m^3")
    print(f"f0: {f0:.6f} Hz")
    print(f"complex modes: {len(poles)}")
    print(f"normalized RMS error: {nrms:.6g}")
    print()
    print(f"Pd messages written to: {txt_path}")
    print(f"fit data written to:    {npz_path}")
    print(f"comparison plot:        {png_path}")
    print()
    print("First fitted modes:")
    for i, (fm, ratio, q, ce) in enumerate(
        zip(f_modes, ratios, q_modes, c_external), start=1
    ):
        print(
            f"{i:2d}: f={fm:9.3f} Hz  ratio={ratio:9.6f}  "
            f"Q={q:8.3f}  C=({ce.real:+.6g},{ce.imag:+.6g})"
        )


if __name__ == "__main__":
    main()
