# Test Analysis

`main.py` turns a thrust log into a thrust curve and the headline numbers: burn time, peak and average thrust, total impulse, specific impulse, and mass burn rate.

```sh
pip install pandas numpy matplotlib
cd Missions/M003-HomiSII/T009
python ../../../Resources/TestAnalysis/main.py T009 <ignition_s> <burnout_s> [propellant_kg] [tare_samples]
```

It writes `<test>.png` to the current directory.

- **Ignition and burnout are set by hand.** Read them off the raw curve first.
- **Input:** either `T0NN.csv`, or a pair `T0NNO.csv` (original, gaps as `0.00`) and `T0NNR.csv` (repaired). Header `Time,Thrust`, with time in ms and thrust in N, already calibration-corrected by the rig.
- **Tare:** the mean of the first `tare_samples` readings (default 40) is subtracted.
- **Weight correction:** the load cell also sees the motor getting lighter as it burns. The script adds back the burned propellant weight, assuming a constant burn rate.
- `propellant_kg` defaults to 0.0316 (T009's load). Always pass the real value.

One-off variants live beside their test, e.g. [`T014_plot.py`](../../Missions/M003-HomiSII/T014/).
