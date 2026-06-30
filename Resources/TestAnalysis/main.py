import sys
import os
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker

# ── Configuration ──────────────────────────────────────────────────────────────
# Usage: python Plotter.py <csv> <ignition_time_s> <burnout_time_s> [propellant_mass_kg]
#   e.g. python Plotter.py T002.csv 5.7 10.2 0.085
if len(sys.argv) < 4:
    print("Usage: python Plotter.py <csv> <ignition_s> <burnout_s> [propellant_mass_kg]")
    sys.exit(1)

CSV_FILE = sys.argv[1]
T_IGNITION = float(sys.argv[2])
T_BURNOUT = float(sys.argv[3])
PROPELLANT_MASS_KG = float(sys.argv[4]) if len(sys.argv) > 4 else 0.085
TARE_SAMPLES = 50
G = 9.80665

test_name = os.path.splitext(os.path.basename(CSV_FILE))[0]

# ── Load & tare ────────────────────────────────────────────────────────────────
df = pd.read_csv(CSV_FILE)
df["Time_s"] = df["Time"] / 1000.0
tare_offset = df["Thrust"].iloc[:TARE_SAMPLES].mean()
df["Thrust_tared"] = df["Thrust"] - tare_offset

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
# Assume constant burn rate, so m_burned grows linearly during the burn.
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
print(f"""
{'='*60}
  Static Fire Analysis — {test_name}
{'='*60}

  Propellant mass       :  {PROPELLANT_MASS_KG*1000:.1f} g
  Tare offset           :  {tare_offset:.4f} (mean of first {TARE_SAMPLES} samples)

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
ax.set_title(f"{test_name} — Thrust Curve (weight-compensated)", fontsize=13, fontweight="bold")
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
