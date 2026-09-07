import os
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker

# ── T014 — special case ──────────────────────────────────────────────────────
# The data acquisition cut out mid-burn (motor exploded), so the recording
# only covers T+0 -> T+0.8s of a 3.62s burn. Everything past the last real
# sample (t=5.4s) is a PREDICTED continuation, not measured data. The shape
# of that continuation is borrowed from T002's normalized decay curve.

TARE_SAMPLES = 49
T_IGNITION = 4.6          # s
BURN_TIME = 3.62          # s (given: burned 3.62 s before exploding)
T_EXPLOSION = T_IGNITION + BURN_TIME
PROPELLANT_MASS_KG = 0.035
MOTOR_MASS_KG = 0.072
G = 9.80665

data_dir = os.path.dirname(os.path.abspath(__file__))
df = pd.read_csv(os.path.join(data_dir, "T014.csv"))
df["Time_s"] = df["Time"] / 1000.0

tare_offset = df["Thrust"].iloc[:TARE_SAMPLES].mean()
df["Thrust_tared"] = df["Thrust"] - tare_offset

t_last_real = df["Time_s"].iloc[-1]          # 5.4 s
v_last_real = df["Thrust_tared"].iloc[-1]    # last measured (tared) sample

# ── Build the predicted continuation from T002's normalized decay curve ────
t002 = pd.read_csv(os.path.join(data_dir, "..", "..",
                                 "Missions", "M003-HomiSII", "T002", "T002.csv"))
t002["Time_s"] = t002["Time"] / 1000.0
t002_tare = t002["Thrust"].iloc[:40].mean()
t002["Thrust_t"] = t002["Thrust"] - t002_tare

peak_idx_002 = t002["Thrust_t"].idxmax()
t_peak_002 = t002.loc[peak_idx_002, "Time_s"]
v_peak_002 = t002.loc[peak_idx_002, "Thrust_t"]

decay_002 = t002.loc[t002["Time_s"] >= t_peak_002, ["Time_s", "Thrust_t"]].copy()
decay_002["dt"] = decay_002["Time_s"] - t_peak_002
decay_002["ratio"] = decay_002["Thrust_t"] / v_peak_002
decay_002 = decay_002[decay_002["ratio"] > 0]  # stop once T002 crosses zero

# T014's own peak: the last two real samples (10.417 -> 10.337) show thrust has
# just turned over, so the true peak sits a touch before t_last_real.
t_peak_014 = 5.35
v_peak_014 = 10.45

t_pred = np.arange(t_last_real + 0.1, T_EXPLOSION + 1e-9, 0.1)
dt_pred = t_pred - t_peak_014
ratio_pred = np.interp(dt_pred, decay_002["dt"].values, decay_002["ratio"].values,
                        left=1.0, right=decay_002["ratio"].values[-1])
v_pred = v_peak_014 * ratio_pred

pred_df = pd.DataFrame({"Time_s": t_pred, "Thrust_tared": v_pred})

print(f"Tare offset (first {TARE_SAMPLES} samples): {tare_offset:.4f}")
print(f"Last real sample:  t={t_last_real:.2f}s  thrust={v_last_real:.3f} N")
print(f"Assumed peak:      t={t_peak_014:.2f}s  thrust={v_peak_014:.2f} N")
print(f"Predicted thrust just before explosion (t={T_EXPLOSION:.2f}s): "
      f"{pred_df['Thrust_tared'].iloc[-1]:.2f} N")

# ── Weight-compensation correction (real + predicted, up to explosion) ─────
def corrected(time_s, thrust_tared):
    elapsed = time_s - T_IGNITION
    mass_burned = np.clip(elapsed / BURN_TIME, 0, 1) * PROPELLANT_MASS_KG
    return thrust_tared + mass_burned * G

df["Thrust_corrected"] = corrected(df["Time_s"], df["Thrust_tared"])
pred_df["Thrust_corrected"] = corrected(pred_df["Time_s"], pred_df["Thrust_tared"])

burn_mask = df["Time_s"] >= T_IGNITION
impulse_real = np.trapezoid(df.loc[burn_mask, "Thrust_corrected"],
                             df.loc[burn_mask, "Time_s"])
all_t = np.concatenate([df.loc[burn_mask, "Time_s"], pred_df["Time_s"]])
all_v = np.concatenate([df.loc[burn_mask, "Thrust_corrected"], pred_df["Thrust_corrected"]])
impulse_total_est = np.trapezoid(all_v, all_t)

# ── Plot ─────────────────────────────────────────────────────────────────────
fig, ax = plt.subplots(figsize=(12, 6))

ax.plot(df["Time_s"], df["Thrust_tared"], color="#aaa", linewidth=1.0,
        linestyle="--", label="Raw (tared)")
ax.plot(df["Time_s"], df["Thrust_corrected"], color="#e84118", linewidth=1.8,
        label="Measured (weight-compensated)")

bridge_t = [t_last_real, pred_df["Time_s"].iloc[0]]
bridge_v = [df["Thrust_corrected"].iloc[-1], pred_df["Thrust_corrected"].iloc[0]]
ax.plot(bridge_t, bridge_v, color="#e84118", linewidth=1.8)
ax.plot(pred_df["Time_s"], pred_df["Thrust_corrected"], color="#e84118",
        linewidth=1.8, linestyle=(0, (5, 3)), alpha=0.65,
        label="Predicted continuation (extrapolated from T002)")

ax.fill_between(df["Time_s"], df["Thrust_corrected"], 0,
                where=(df["Thrust_corrected"] > 0), alpha=0.12, color="#e84118")
ax.fill_between(pred_df["Time_s"], pred_df["Thrust_corrected"], 0,
                alpha=0.06, color="#e84118")

ax.axvline(T_IGNITION, color="#2ecc71", linewidth=1.2, linestyle="-.",
           label=f"Ignition ({T_IGNITION:.2f} s)")
ax.axvline(t_last_real, color="#95a5a6", linewidth=1.0, linestyle=":",
           label=f"Data cutoff ({t_last_real:.2f} s)")
ax.axvline(T_EXPLOSION, color="#c0392b", linewidth=1.4, linestyle="-.",
           label=f"Explosion ({T_EXPLOSION:.2f} s)")

# Explosion marker: sharp drop to 0
explosion_v = pred_df["Thrust_corrected"].iloc[-1]
ax.plot([T_EXPLOSION, T_EXPLOSION], [explosion_v, 0], color="#c0392b",
        linewidth=1.4, linestyle=(0, (5, 3)), alpha=0.65)
ax.scatter([T_EXPLOSION], [explosion_v], color="#c0392b", marker="x", s=70, zorder=5)
ax.annotate("Motor exploded\n(T+3.62 s)",
            xy=(T_EXPLOSION, explosion_v), xytext=(T_EXPLOSION - 1.3, explosion_v + 2.5),
            arrowprops=dict(arrowstyle="->", color="#c0392b"),
            fontsize=9, color="#c0392b", fontweight="bold")

peak_t_annot = df["Time_s"].iloc[-2]
peak_v_annot = df["Thrust_corrected"].iloc[-2]
ax.annotate(f"Peak (measured): {peak_v_annot:.2f} N\n@ T+{peak_t_annot - T_IGNITION:.2f} s",
            xy=(peak_t_annot, peak_v_annot), xytext=(peak_t_annot - 1.9, peak_v_annot - 1.5),
            arrowprops=dict(arrowstyle="->", color="#333"), fontsize=9, color="#333",
            bbox=dict(boxstyle="round,pad=0.3", facecolor="white", edgecolor="none", alpha=0.8))

info = (f"Propellant mass:  {PROPELLANT_MASS_KG*1000:.0f} g\n"
        f"Motor mass:  {MOTOR_MASS_KG*1000:.0f} g\n"
        f"Ignition:  {T_IGNITION:.2f} s\n"
        f"Burn time (to explosion):  {BURN_TIME:.2f} s\n"
        f"\n"
        f"Measured up to:  {t_last_real:.2f} s\n"
        f"Peak thrust (measured):  {peak_v_annot:.2f} N\n"
        f"\n"
        f"Est. total impulse*:  {impulse_total_est:.1f} N·s\n"
        f"*includes predicted segment")
ax.text(0.97, 0.97, info, transform=ax.transAxes, fontsize=8.5,
        fontfamily="monospace", verticalalignment="top", horizontalalignment="right",
        bbox=dict(boxstyle="round,pad=0.5", facecolor="white", edgecolor="#ccc", alpha=0.9))

ax.set_xlabel("Time (s)", fontsize=11)
ax.set_ylabel("Thrust (N)", fontsize=11)
ax.set_title("T014 — Thrust Curve (data cuts out at T+0.8s; remainder predicted)",
             fontsize=13, fontweight="bold")
ax.xaxis.set_minor_locator(ticker.AutoMinorLocator())
ax.yaxis.set_minor_locator(ticker.AutoMinorLocator())
ax.grid(True, which="major", linestyle="--", alpha=0.5)
ax.grid(True, which="minor", linestyle=":", alpha=0.25)
ax.legend(fontsize=9, loc="upper left")

plt.tight_layout()
out_png = os.path.join(data_dir, "T014.png")
plt.savefig(out_png, dpi=150)
print(f"Saved plot to {out_png}")
