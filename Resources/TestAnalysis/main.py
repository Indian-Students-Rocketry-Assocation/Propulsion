import sys
import os
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
from matplotlib.patches import Patch

# ── Configuration ──────────────────────────────────────────────────────────────
# Usage: python main.py <base_name> <ignition_s> <burnout_s> [propellant_mass_kg]
#   e.g. python main.py T003 9.4 15.2 0.076
#
# Loads two files:
#   <base>R.csv  -> "repaired" data (missing samples filled with generated readings)
#   <base>O.csv  -> "original"  data (missing samples flagged as 0.00)
#
# Both CSVs are assumed to be ALREADY calibration-corrected. This script only
# tares (zeros) them using the first TARE_SAMPLES readings.
if len(sys.argv) < 4:
    print("Usage: python main.py <base_name> <ignition_s> <burnout_s> [propellant_mass_kg]")
    sys.exit(1)

BASE = sys.argv[1]
T_IGNITION = float(sys.argv[2])
T_BURNOUT = float(sys.argv[3])
PROPELLANT_MASS_KG = float(sys.argv[4]) if len(sys.argv) > 4 else 0.076
TARE_SAMPLES = 40           # tare against the mean of the first 40 readings
G = 9.80665

# Resolve file paths — accept either "T003" or a full "T003R.csv" argument.
stem = os.path.splitext(os.path.basename(BASE))[0]
if stem.endswith("R") or stem.endswith("O"):
    stem = stem[:-1]
data_dir = os.path.dirname(os.path.abspath(BASE)) or "."
R_FILE = os.path.join(data_dir, f"{stem}R.csv")
O_FILE = os.path.join(data_dir, f"{stem}O.csv")
test_name = stem

# ── Load repaired (R) data & tare ────────────────────────────────────────────────
df = pd.read_csv(R_FILE)
df["Time_s"] = df["Time"] / 1000.0
tare_offset = df["Thrust"].iloc[:TARE_SAMPLES].mean()   # tare = mean of first 40 samples
df["Thrust_tared"] = df["Thrust"] - tare_offset

# ── Load original (O) data — 0.00 flags missing samples ──────────────────────────
odf = pd.read_csv(O_FILE)
odf["Time_s"] = odf["Time"] / 1000.0
odf["missing"] = odf["Thrust"] == 0.0
# Tare the original against the first 40 readings, ignoring any missing (0.00)
# flags that fall within that window.
o_head = odf["Thrust"].iloc[:TARE_SAMPLES]
o_tare_offset = o_head[o_head != 0.0].mean()
# Present missing samples as NaN so the trace breaks at every gap.
odf["Thrust_valid"] = np.where(odf["missing"], np.nan, odf["Thrust"] - o_tare_offset)

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

# ── Contiguous missing-data spans (for shading the original trace) ──────────────
missing_spans = []
in_gap = False
for _, row in odf.iterrows():
    if row["missing"] and not in_gap:
        start = row["Time_s"]
        in_gap = True
    elif not row["missing"] and in_gap:
        missing_spans.append((start, prev_t))
        in_gap = False
    prev_t = row["Time_s"]
if in_gap:
    missing_spans.append((start, prev_t))

# ── Plot ───────────────────────────────────────────────────────────────────────
fig, (ax, ax2) = plt.subplots(2, 1, figsize=(12, 10), sharex=True)

# ── Top panel: corrected (weight-compensated) thrust curve ─────────────────────
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

ax.set_ylabel("Thrust (N)", fontsize=11)
ax.set_title(f"{test_name} — Thrust Curve (calibration-corrected, weight-compensated)",
             fontsize=13, fontweight="bold")
ax.yaxis.set_minor_locator(ticker.AutoMinorLocator())
ax.grid(True, which="major", linestyle="--", alpha=0.5)
ax.grid(True, which="minor", linestyle=":", alpha=0.25)
ax.legend(fontsize=9, loc="upper left")

# ── Bottom panel: original readings with missing data indicated ────────────────
# Shade every contiguous missing-data span.
for i, (s, e) in enumerate(missing_spans):
    ax2.axvspan(s, e, color="#e84118", alpha=0.12,
                label="Missing data" if i == 0 else None)

# Original tared trace — line breaks at each gap; markers show recorded samples.
ax2.plot(odf["Time_s"], odf["Thrust_valid"], color="#0097e6", linewidth=1.3,
         marker="o", markersize=2.5, label="Original (tared)")
ax2.axhline(0, color="#999", linewidth=0.8, linestyle="--")
ax2.axvline(t_ignition, color="#2ecc71", linewidth=1.2, linestyle="-.")
ax2.axvline(t_burnout, color="#3498db", linewidth=1.2, linestyle="-.")

ax2.set_xlabel("Time (s)", fontsize=11)
ax2.set_ylabel("Thrust (N)", fontsize=11)
ax2.set_title(f"{test_name} — Original Readings ({n_missing} missing samples flagged)",
              fontsize=12, fontweight="bold")
ax2.xaxis.set_minor_locator(ticker.AutoMinorLocator())
ax2.yaxis.set_minor_locator(ticker.AutoMinorLocator())
ax2.grid(True, which="major", linestyle="--", alpha=0.5)
ax2.grid(True, which="minor", linestyle=":", alpha=0.25)

# Legend for bottom panel (include a proxy for the missing-data shading).
handles, labels = ax2.get_legend_handles_labels()
if "Missing data" not in labels:
    handles.append(Patch(facecolor="#e84118", alpha=0.12, label="Missing data"))
ax2.legend(handles=handles, fontsize=9, loc="upper left")

plt.tight_layout()
out_png = f"{test_name}.png"
plt.savefig(out_png, dpi=150)
plt.show()
print(f"Saved plot to {out_png}")
