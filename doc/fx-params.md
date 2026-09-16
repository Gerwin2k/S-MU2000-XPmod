# Effect parameters (1–16) — SysEx addresses

`doc/effects.md` established, by measurement, the addresses for effect **type /
return / part / connection**, and `doc/params.md` lists the per-effect
**parameters 1–16** as not-yet-mapped. This fills that gap.

The addresses below were verified the same way as the rest — **firmware
round-trip**: write a value, request a dump of the block, and check that the
value reads back. The probe is `src/fx_probe.cpp` (same style as `xgtest`; boots
`mu2000`, writes, dump-requests `F0 43 20 4C <ah> <am> <al> F7`, inspects the
block payload). Confirmed audible for the variation block (tremolo AM Depth
P2=0 → ~5 %, P2=127 → ~95 %).

## Addresses

| Block | Type | Param N (1..10) | Param N (11..16) |
|---|---|---|---|
| Reverb `02 01` | `00` (2B) | `02 + (N-1)` (1B) | — |
| Chorus `02 01` | `20` (2B) | `22 + (N-1)` (1B) | — |
| **Variation** `02 01` | `40` (2B) | **`42 + 2·(N-1)`** (2B) | (within the 2B region) |
| **Insertion 1** `03 00` | `00` (2B) | **`02 + (N-1)`** (1B) | **`0D + (N-11)`** (1B) |
| Insertion 2/3/4 | `03 01/02/03 00` | (same layout as Ins 1) | (same) |

`2B` = two data bytes (MSB, LSB); `1B` = one data byte.

## The one that bites: variation params are 2-byte and must be written atomically

The variation effect parameters are stored as **2-byte** values (MSB at the even
address, LSB at the odd). The firmware only accepts them when **both bytes are
sent in a single Parameter Change**:

```
F0 43 10 4C 02 01 <al> <MSB> <LSB> F7      # e.g. P1 = 0x0055:  ... 42 00 55 F7
```

A single-byte write to the odd (LSB) address, or the MSB and LSB sent as two
separate messages, is **silently ignored** — the effect keeps running with its
default parameters. (This is why "set a variation FX param" appears to do
nothing.) Insertion params are plain 1-byte writes and take individually.

Reverb / chorus params are 1-byte and were already working, which is why only the
variation block showed the problem.

## Example — set Variation = Tremolo, LFO Freq = 0x40, AM Depth = 0x7F

```
F0 43 10 4C 02 01 40 46 00 F7      # type = TREMOLO (2-byte, atomic)
F0 43 10 4C 02 01 42 00 40 F7      # P1  LFO Frequency
F0 43 10 4C 02 01 44 00 7F F7      # P2  AM Depth
```

Undefined param numbers for a given effect (e.g. Tremolo has no P4/P5) accept the
write into storage but the effect ignores them — expected.

## Verify

`build/fx_probe.exe <rom dir>` prints the variation + insertion param maps
(which addresses store, at which dump offset). Add to `make test` alongside
`xgtest` if wanted; it needs ROMs and takes a few seconds (no audio).
