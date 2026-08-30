# GC16 black-hold compensation experiment

## Symptom

After a Quality refresh, a target-black area can retain a faint gray/white
outline where the previous frame was also black. A black-to-white diagnostic
is nearly clean, so this experiment does not add a global erase pass. It only
balances the GC16 `old 0 -> new 0` transition.

## Experimental change

`MYPAPER_EPD_2D_GC16_00_BLACK_COMP_FRAMES` controls the correction and is `2`
by default. The engine changes `0 -> 0` to black drive during the two idle
phases immediately before the final idle phase. The last phase remains idle for panel
settling. Set the macro to `0` to restore the supplied GC16 data exactly.

The correction is applied after decoding a GC16 phase into the 256-entry
old/new transition table. It therefore does not edit the waveform source file
and cannot affect the legacy `Panel_EPD` implementation.

## DU / Quality switching semantics

- `epd_quality` selects GC16 and is always expanded to a full-screen batch.
- `epd_fast` and `epd_fastest` select DU. The GC16 correction helper rejects
  DU before changing the transition table.
- When queued refreshes are merged, the numerically smaller mode wins. Since
  Quality is `1`, it cannot be downgraded to DU by a later Fast request.
- After DU, only old/new transitions with at least one real DU drive pulse are
  committed to the physical-state buffer. Undriven gray targets retain their
  previous physical value, so a following Quality refresh receives a truthful
  old-gray input.
- After a successful Quality batch, all driven pixels are committed. A scan or
  power failure invalidates the state, and the next request is promoted to
  Quality.

At boot, RAM is initialized as white but physical state is marked unknown. The
first request is promoted to Quality. This establishes the software tracking
baseline; it cannot infer arbitrary panel history left by an earlier firmware
run.

## Hardware validation

Use two pages with overlapping black regions and refresh both with Quality.
Compare the gray/white outline inside pixels that remain black. The log should
contain:

```text
Panel_EPD_2D: GC16 0->0 black compensation enabled: frames=2 final_idle=1
```

Also alternate Quality and Fast once in each direction. The batch log should
show waveform type `2` for Quality and type `1` for Fast/Fastest. If the black
area becomes overdriven or darker-edged, set the compensation macro to `0`.
