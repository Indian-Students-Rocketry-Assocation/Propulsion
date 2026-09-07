import sys
import os
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker

# ── Configuration ──────────────────────────────────────────────────────────────
# Usage: python main.py <base_name> <ignition_s> <burnout_s> [propellant_mass_kg] [tare_samples]
#   e.g. python main.py T003 9.4 15.2 0.076
#
# Two input layouts are supported:
#   <base>R.csv + <base>O.csv  -> "repaired" (gaps filled) + "original" (gaps
#                                  flagged as 0.00) pair, both header "Time,Thrust"
#   <base>.csv                 -> single file, used for both panels (no missing
#                                  samples). Accepts a header ("Time,Thrust" or
#                                  "Time,RawADC,Thrust") or no header (2 or 3
#                                  columns, last column is Thrust).
#
# All inputs are assumed to be ALREADY calibration-corrected. This script only
# tares (zeros) them using the first TARE_SAMPLES readings.
if len(sys.argv) < 4:
    print("Usage: python main.py <base_name> <ignition_s> <burnout_s> [propellant_mass_kg] [tare_samples]")
    sys.exit(1)

BASE = sys.argv[1]
T_IGNITION = float(sys.argv[2])
T_BURNOUT = float(sys.argv[3])
PROPELLANT_MASS_KG = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0316
TARE_SAMPLES = int(sys.argv[5]) if len(sys.argv) > 5 else 40   # tare against the mean of the first N readings
G = 9.80665

# Resolve file paths — accept either "T003" or a full "T003R.csv" argument.
stem = os.path.splitext(os.path.basename(BASE))[0]
if stem.endswith("R") or stem.endswith("O"):
    stem = stem[:-1]
data_dir = os.path.dirname(os.path.abspath(BASE)) or "."
R_FILE = os.path.join(data_dir, f"{stem}R.csv")
O_FILE = os.path.join(data_dir, f"{stem}O.csv")
SINGLE_FILE = os.path.join(data_dir, f"{stem}.csv")
test_name = stem


def load_thrust_csv(path):
    """Load a Time/Thrust CSV, tolerating a missing header and an extra
    leading raw-ADC column (Time,RawADC,Thrust)."""
    probe = pd.read_csv(path, nrows=0)
    if "Time" in probe.columns and "Thrust" in probe.columns:
        return pd.read_csv(path)
    ncols = pd.read_csv(path, nrows=1, header=None).shape[1]
    names = ["Time", "Thrust"] if ncols == 2 else ["Time", "RawADC", "Thrust"]
    return pd.read_csv(path, header=None, names=names)


if os.path.exists(R_FILE) and os.path.exists(O_FILE):
    # ── Load repaired (R) data & tare ────────────────────────────────────────
    df = load_thrust_csv(R_FILE)
    df["Time_s"] = df["Time"] / 1000.0
    tare_offset = df["Thrust"].iloc[:TARE_SAMPLES].mean()
    df["Thrust_tared"] = df["Thrust"] - tare_offset

    # ── Load original (O) data — 0.00 flags missing samples ─────────────────
    odf = load_thrust_csv(O_FILE)
    odf["Time_s"] = odf["Time"] / 1000.0
    odf["missing"] = odf["Thrust"] == 0.0
    o_head = odf["Thrust"].iloc[:TARE_SAMPLES]
    o_tare_offset = o_head[o_head != 0.0].mean()
    odf["Thrust_valid"] = np.where(odf["missing"], np.nan, odf["Thrust"] - o_tare_offset)
elif os.path.exists(SINGLE_FILE):
    # ── Single-file mode: one clean recording, no missing samples ───────────
    df = load_thrust_csv(SINGLE_FILE)
    df["Time_s"] = df["Time"] / 1000.0
    tare_offset = df["Thrust"].iloc[:TARE_SAMPLES].mean()
    df["Thrust_tared"] = df["Thrust"] - tare_offset

    odf = df.copy()
    odf["missing"] = False
    o_tare_offset = tare_offset
    odf["Thrust_valid"] = odf["Thrust_tared"]
else:
    print(f"Could not find {R_FILE} + {O_FILE}, nor {SINGLE_FILE}")
    sys.exit(1)

# ── Ignition & burnout (manual) ────────────────────────────────────────────────
peak_thrust = df["Thrust_tared"].max()
t_ignition = T_IGNITION
t_burnout = T_BURNOUT
burn_time = t_burnout - t_ignition

ignition_idx = (df["Time_s"] - t_ignition).abs().idxmin()
burnout_idx = (df["Time_s"] - t_burnout).abs().idxmin()

# ── Burn rate ──────────────────────────────────────────────────────────────────
burn_rate = (PROPELLANT_MASS_KG * 1000) / burn_time  # g/s

# ── Weight correction ──────────────────────────────────────────────────────────
# The load cell reads: thrust − (lost propellant weight).
# Corrected thrust = tared reading + m_burned(t) · g
# Assume a constant burn rate, so m_burned grows linearly during the burn.
df["Thrust_corrected"] = df["Thrust_tared"].copy()

burn_mask = (df.index >= ignition_idx) & (df.index <= burnout_idx)
elapsed = df.loc[burn_mask, "Time_s"] - t_ignition
mass_burned = np.minimum(elapsed / burn_time, 1.0) * PROPELLANT_MASS_KG
df.loc[burn_mask, "Thrust_corrected"] += mass_burned.values * G

post_mask = df.index > burnout_idx
df.loc[post_mask, "Thrust_corrected"] += PROPELLANT_MASS_KG * G

# ── Impulse (trapezoidal integration of corrected thrust during burn) ──────────
burn_df = df.loc[burn_mask]
impulse = np.trapezoid(burn_df["Thrust_corrected"].values, burn_df["Time_s"].values)

# ── Specific impulse ──────────────────────────────────────────────────────────
isp = impulse / (PROPELLANT_MASS_KG * G)

# ── Average thrust ─────────────────────────────────────────────────────────────
avg_thrust = impulse / burn_time
avg_thrust_uncorrected = np.trapezoid(burn_df["Thrust_tared"].values, burn_df["Time_s"].values) / burn_time

# ── Print results ──────────────────────────────────────────────────────────────
n_missing = int(odf["missing"].sum())
print(f"""
{'='*60}
  Static Fire Analysis — {test_name}
{'='*60}

  Propellant mass       :  {PROPELLANT_MASS_KG*1000:.1f} g
  Tare offset (R)       :  {tare_offset:.4f} (mean of first {TARE_SAMPLES} samples)
  Tare offset (O)       :  {o_tare_offset:.4f} (mean of first {TARE_SAMPLES} valid samples)
  Missing samples (O)   :  {n_missing} of {len(odf)}

  ── Timing ──
  Ignition              :  {t_ignition:.2f} s
  Burnout               :  {t_burnout:.2f} s
  Burn time             :  {burn_time:.2f} s

  ── Thrust ──
  Peak thrust (raw)     :  {peak_thrust:.3f} N
  Peak thrust (corrected): {df.loc[burn_mask, 'Thrust_corrected'].max():.3f} N
  Avg thrust (raw)      :  {avg_thrust_uncorrected:.3f} N
  Avg thrust (corrected):  {avg_thrust:.3f} N

  ── Performance ──
  Total impulse         :  {impulse:.3f} N·s
  Specific impulse      :  {isp:.2f} s
  Burn rate             :  {burn_rate:.2f} g/s

{'='*60}
""")

# ── Plot ───────────────────────────────────────────────────────────────────────
fig, ax = plt.subplots(figsize=(12, 6))

# ── Corrected (weight-compensated) thrust curve ─────────────────────────────────
ax.plot(df["Time_s"], df["Thrust_tared"], color="#aaa", linewidth=1.0,
        linestyle="--", label="Raw (tared)")
ax.plot(df["Time_s"], df["Thrust_corrected"], color="#e84118", linewidth=1.8,
        label="Corrected (weight-compensated)")
ax.axhline(0, color="#999", linewidth=0.8, linestyle="--")

ax.fill_between(df["Time_s"], df["Thrust_corrected"], 0,
                where=(df["Thrust_corrected"] > 0),
                alpha=0.12, color="#e84118")

# Mark ignition & burnout
ax.axvline(t_ignition, color="#2ecc71", linewidth=1.2, linestyle="-.",
           label=f"Ignition ({t_ignition:.2f} s)")
ax.axvline(t_burnout, color="#3498db", linewidth=1.2, linestyle="-.",
           label=f"Burnout ({t_burnout:.2f} s)")

# Annotate peak
peak_corr_idx = df.loc[burn_mask, "Thrust_corrected"].idxmax()
peak_t = df.loc[peak_corr_idx, "Time_s"]
peak_v = df.loc[peak_corr_idx, "Thrust_corrected"]
ax.annotate(f"Peak: {peak_v:.2f} N\n@ T+{peak_t - t_ignition:.2f} s",
            xy=(peak_t, peak_v),
            xytext=(peak_t + 0.5, peak_v - 1.5),
            arrowprops=dict(arrowstyle="->", color="#333"),
            fontsize=9, color="#333")

# Info box — all static fire analysis results
info = (f"Propellant mass:  {PROPELLANT_MASS_KG*1000:.1f} g\n"
        f"Ignition:  {t_ignition:.2f} s\n"
        f"Burnout:  {t_burnout:.2f} s\n"
        f"Burn time:  {burn_time:.2f} s\n"
        f"Burn rate:  {burn_rate:.2f} g/s\n"
        f"\n"
        f"Peak thrust (raw):  {peak_thrust:.3f} N\n"
        f"Peak thrust (corrected):  {df.loc[burn_mask, 'Thrust_corrected'].max():.3f} N\n"
        f"Avg thrust (raw):  {avg_thrust_uncorrected:.3f} N\n"
        f"Avg thrust (corrected):  {avg_thrust:.3f} N\n"
        f"\n"
        f"Total impulse:  {impulse:.3f} N·s\n"
        f"Specific impulse:  {isp:.2f} s")
ax.text(0.97, 0.97, info, transform=ax.transAxes, fontsize=8.5,
        fontfamily="monospace", verticalalignment="top", horizontalalignment="right",
        bbox=dict(boxstyle="round,pad=0.5", facecolor="white", edgecolor="#ccc", alpha=0.9))

ax.set_xlabel("Time (s)", fontsize=11)
ax.set_ylabel("Thrust (N)", fontsize=11)
ax.set_title(f"{test_name} — Thrust Curve (calibration-corrected, weight-compensated)",
             fontsize=13, fontweight="bold")
ax.xaxis.set_minor_locator(ticker.AutoMinorLocator())
ax.yaxis.set_minor_locator(ticker.AutoMinorLocator())
ax.grid(True, which="major", linestyle="--", alpha=0.5)
ax.grid(True, which="minor", linestyle=":", alpha=0.25)
ax.legend(fontsize=9, loc="upper left")

plt.tight_layout()
out_png = f"{test_name}.png"
plt.savefig(out_png, dpi=150)
plt.show()
print(f"Saved plot to {out_png}")
