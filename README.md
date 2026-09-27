# Propulsion

![License: MIT](https://img.shields.io/badge/License-MIT-orange.svg)
![Safety](https://img.shields.io/badge/⚠%20read-safety%20note-red.svg)

Motor test records, thrust data, analysis scripts and test-rig firmware from the propulsion team of the Indian Students' Rocketry Association. Published in the spirit of open science: rocketry propulsion knowledge should be accessible to student teams everywhere, not locked behind expensive courses or institutional walls.

We're a student rocketry association from Anand, Gujarat, building towards 100 km, one mission at a time.

---

> ### ⚠️ Safety Note
> All propellant work documented here was conducted under direct supervision of qualified advisors, with proper safety protocols and institutional clearances in place. Propellant chemistry carries serious risk of fire, explosion, and legal consequence if mishandled. Several motors in this repository exploded.
>
> We share this for educational understanding, **not as instructions to replicate.** Do not attempt any experimental propulsion work without the appropriate expertise, facilities, supervision, and legal clearances in your region.
>
> Every cook we run follows our [Propellant Safety Protocol](Resources/Safety/ISRA-Propulsion-Safety-Protocol.pdf) (PS-01).
>
> **We strongly urge against potassium dichromate.** It was used in M001, and we have discontinued it. It's a hexavalent chromium compound: toxic, carcinogenic, and not worth it.

---

## Missions

A **mission** is one rocket. A **test** is one static fire (or attempt) of a motor built for it. Test numbers are global and never reused.

| Mission | Rocket | Motor | Status |
|---|---|---|---|
| [M001-SRM](Missions/M001-SRM/) | SRM | In-house KNSU | Flown 2024-10-20 · unrecorded |
| [M002-HomiSI](Missions/M002-HomiSI/) | HomiS I | Commercial E5 (Aerosports, Vadodara) | Launched 2025-03-11 · failed |
| [M003-HomiSII](Missions/M003-HomiSII/) | HomiS II | In-house KNSU, target E5 | Static testing: T001–T014 |

## Layout

```
Missions/
  M00N-<Rocket>/
    README.md            mission overview + test index
    T0NN/
      README.md          what changed, why, what happened
      T0NN.pdf           scanned log sheet (the primary record)
      T0NN.csv           thrust data, Time (ms), Thrust (N)
      T0NN.png           thrust-curve analysis
      T0NN.mov / .mp4    test video
Resources/
  Safety/                Propellant Safety Protocol (PS-01)
  Templates/             blank motor log sheet, printed for every test
  TestAnalysis/          main.py: thrust-curve analysis
  TestingRig/            load-cell rig firmware (transmitter + receiver)
```

## Recording a test

1. Print [the log sheet](Resources/Templates/Motor-Log-Sheet.pdf) and fill it in during the cook and the test.
2. Create `Missions/<mission>/T0NN/` using the next unused number.
3. Add the scanned sheet as `T0NN.pdf`, the receiver's serial log as `T0NN.csv`, and any video.
4. Run the analysis (see [TestAnalysis](Resources/TestAnalysis/)) and commit the `T0NN.png` it produces.
5. Write `README.md` from the previous test's README, and add a row to the mission's test index.

---

## Contributing

Propulsion is the domain where outside input matters most. If you have experience with motor design, burn-rate modelling, or data that contextualises ours, we'd genuinely like to hear from you.

Please open a Discussion before submitting pull requests that relate to propellant formulations or safety protocols. Those aren't changes we merge lightly.

[💬 Discussions](../../discussions) · [🔗 All ISRA repos](https://github.com/Indian-Students-Rocketry-Assocation)

## License

MIT, see [LICENSE](LICENSE).
