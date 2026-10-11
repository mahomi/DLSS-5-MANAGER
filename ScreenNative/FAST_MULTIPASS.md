# Fast Multi-pass: measured matching only

Fast ON runs **one NR evaluation**, then applies the measured Multi-pass profile
and NR/Fast color stabilization. Fast OFF restores the existing upstream
Multi-pass selection. UI changes apply during Live Flow without restarting.

## Controls and settings

- Fast OFF/ON; default OFF.
- **Target Multi-pass**: integer 1–6, default 2. Target 1 adds no spatial gain,
  but still stabilizes NR colors on subsequent frames when the amount is nonzero.
- **NR/Fast color stabilization**: 0–90% in 1% steps, default 90%. 0% disables
  Fast history blending, not the NR model's internal history. Larger values
  smooth changes but may lag or leave trails. Previous saved amounts are retained.

There is no separate Measured checkbox, heuristic Effect strength, texture
correction or added-lighting slider. They are removed from UI and processing.
The Manager atomically writes only `enabled`, `target`, `stability` to
`%LOCALAPPDATA%/DLSS5Manager/Screen/fast-multipass.json`. The worker polls at most
once every 100 ms. Old saved files remain readable: retired keys `strength`,
`texture`, `tone`, `reference`, `temporal`, `full_stability`, `final_output`
are type-checked and ignored. Target and amount retain their previous values;
missing values default to 2 and 0.9. Invalid updates keep the last valid worker
configuration. Missing files default to Fast OFF. No Fast environment variables
are read. Other upstream controller environment variables remain unchanged.

Fast settings are separate from the external capture controller's presets.
Turning Fast OFF/ON drains GPU work before changing extra NR resources; the
controller and primary NR feature keep running. Effective target/amount edits
reset Fast history and refresh a stationary source. Deprecated-key-only edits
do not invalidate history. Fast OFF disables both sliders without losing values.

## Measured spatial correction

At raw NR resolution, with immutable source X and first-pass result Y1:

```
R = Y1 - X
fineLow = Gaussian3x3(R)
low = SparseGaussian9x9(R)
fine = R - fineLow
structure = fineLow - low
nrFineLow = Gaussian3x3(Y1)
nrLow = SparseGaussian9x9(Y1)
matched = Cfine*fine + Cstructure*structure + Clow*low
        + CnrFine*(Y1-nrFineLow) + CnrStructure*(nrFineLow-nrLow) + Cbias
output = Y1 + bounded(matched * motionConfidence)
```

Each style has separate signed RGB coefficients for targets 1–6. Coefficients,
filter sizes, bounds and processing placement are unchanged from measured-ON
at commit `dc23ee3`; only the unused heuristic branches have been removed.
Target 1 has zero coefficients. RGB correction is limited to ±(0.04*target)
and 75% of available shadow/highlight headroom. Alpha is preserved. Border
samples clamp. A copy of Y1 prevents neighboring read/write aliasing.
Motion confidence uses source correspondence while history resources are ready;
on history-resource failure, speed fading between 2 and 24 work pixels remains
the fallback. Invalid motion rejects the added correction.

The dispatch stays **immediately after raw NR, before Enhance/Halo and resizing**.
NR bypass does not execute Fast. Fast results are never fed into NR history.

Profiles were fitted against actual sequential NR 1–6 output on nine packaged
scenes per model at 512×288, intensity 100%, tone/local/skin structure 150%,
automatic mask enabled, Enhance/Halo off, and reset for each frame. Six scenes
were used for fitting and three held out. Different settings and games can
produce a different match. This approximates further nonlinear AI passes and
cannot create all their new textures. No coefficients were refitted in cleanup.

## NR/Fast color stabilization

The filter stores `current output - current source`. Motion reprojects this
history with bilinear sampling. Source RGB correspondence validates it, also
rejecting equal-luminance hue replacements. A 3×3 refinement at ±1.5 NR pixels
runs when source error exceeds 0.025; confidence fades between 0.025 and 0.12.
Speed alone does not fade gain or history. Nonfinite vectors and off-image
samples reject history. The history clamp radius stays ±0.20; the final
correction still obeys the target and headroom limits relative to raw NR.
Reset, settings/NR/style edits, bypass and recreation invalidate history. The
first frame seeds exactly the spatial correction.

90% is the maximum weight at a matching sample, not constant blending of whole
screens. 100% can retain old effects indefinitely when correspondence stays
high, so the slider maximum remains 90%. Two RGBA16F and two RGBA8 history
textures cost about 24 bytes per NR pixel. They and the dispatch remain at 0%
to preserve the same spatial motion confidence. Resources are released at worker
cleanup. The retired added-effect-only scope is removed.

## Build and verification

```
ScreenNative/build.cmd
ScreenNative/build-fast-probe.cmd
dotnet build "DLSS 5 MANAGER.fsproj" -c Debug
verify_fast_multipass.exe --settings-check isolated-settings.json
verify_fast_multipass.exe isolated-settings.json
```

`build.cmd` uses Visual Studio 2022 Community, MSVC C++17 and the Windows SDK.
The repository includes NGX headers/libraries. CUDA is not required. The whole
Manager uses .NET 8. `nvngx.dll` is a worker executable with a DLL suffix;
`nvngx.dll_ns-forwarder.dll` is its accompanying forwarder.

Run the Python checks with the prepared app's bundled Python. They use packaged
images and restore the user's Fast JSON in `finally`; run them sequentially with
Manager idle/closed because they share that settings file.

- `verify_fast_multipass_live.py APP OUTPUT --worker CURRENT --baseline-worker OLD`
  compares every byte with dc23ee3 measured-ON: all three models, targets 1–6,
  stability 0/30/60/90%, reset/live history, small NR + Enhance/Halo, full NR,
  migration and hot OFF. Synthetic full NR + Halo is excluded because that
  combination also failed on the previous worker; it is not claimed fixed.
- `verify_fast_scroll_live.py APP OUTPUT --worker CURRENT --baseline-worker OLD`
  compares 40 moving frames at each of two fractional pan speeds. Reports
  motion-compensated bottom-half RGB/chroma changes; this is a diagnostic,
  not a game quality metric.
- `verify_fast_reference_live.py APP CALIBRATION_DIR --worker CURRENT` compares
  production output against actual sequential references and the independent
  fit report. Legacy MAE is read from the archived fit, not a removed mode.
- The GPU probe exercises all profiles/targets, bounds, source/alpha preservation,
  odd sizes, resize, live settings, 0/30/60/90%, invalid motion, reset and RGB
  rejection with D3D12 debug validation. An isolated Avalonia headless harness
  checks actual UI bindings, migration, recording-time edits and rendering.

Packaged-image comparisons cannot establish game perceptual quality or FPS.
Use the updated complete `outputs/app/DLSS 5 MANAGER.exe` for game testing.
