# M003 — HomiS II

**Goal:** an in-house KNSU motor that matches the E5 class of the commercial motor HomiS I flew. So far we've characterised a D3 (T002) and a C-class (T009).

## Test index

| Test | Tested | Propellant | Casing / nozzle | Changed from previous | Result |
|---|---|---|---|---|---|
| [T001](T001/) | 2026-06-26 | KNSU + Al + Fe₂O₃ | Reusable metal | First test with the thrust rig | No data: rig didn't record |
| [T002](T002/) | 2026-06-29 | KNSU + Al + Fe₂O₃ | Reusable metal, 4 mm | Improved rig | **D3**, first thrust curve |
| [T003](T003/) | 2026-07-03 | KNSU + Al + Fe₂O₃ | Reusable metal, 2 mm | Smaller nozzle | D2, partial data (gaps filled) |
| [T004](T004/) | 2026-07-06 | KNSU + Al + Fe₂O₃ | Reusable metal, M-Seal nozzle | Less Fe₂O₃ (3 → 2 g), fixed 130 °C | Nozzle failed, motor exploded |
| [T005](T005/) | 2026-07-10 | KNSU + Al + Fe₂O₃ | Reusable metal, CNC steel 3 mm | Steel nozzle | Casing failed, motor exploded |
| [T006](T006/) | 2026-08-09 | KNSU 60:40 | Cardboard, M-Seal nozzle | New formulation and casing | Nozzle failed, no data |
| [T007](T007/) | 2026-08-13 | KNSU 60:40 | Cardboard, thicker M-Seal nozzle | Thicker nozzle | Nozzle failed, no data |
| T008 | — | — | — | — | No record |
| [T009](T009/) | unrecorded | unrecorded | unrecorded | unrecorded | **C-class**: 7.37 N·s, 2.1 s burn |
| T010–T013 | — | — | — | — | No record |
| [T014](T014/) | unrecorded | unrecorded | unrecorded | unrecorded | Exploded at T+3.62 s, partial data |

## What we've learned

- **Metal casings, small nozzles, fast propellant don't mix.** T004 and T005 both exploded. After T005 we concluded that a 3 mm steel nozzle was too small, and that the combustion rate needs to come down (burn-rate modifiers) before going back to metal.
- **M-Seal can't hold chamber pressure as a nozzle.** It failed in T004, T006 and T007, even when made thicker.
- **Batch-to-batch repeatability isn't there yet.** T003 was meant to replicate T002 and underperformed; we put this down to procedural errors in the cook.
