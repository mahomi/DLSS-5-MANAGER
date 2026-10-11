# Live Flow SteamVR implementation

This branch depends on the preceding Fast Multi-pass feature commit. It is not presented as the upstream 1.3.5 source release.

The Manager supports separate Desktop and SteamVR modes. VR supports normal 1–6-pass NR, or single-pass NR with the existing measured Fast matching and color stabilization. Fast OFF restores the selected normal pass count. Controls apply through the Manager's live control file and heartbeat; environment variables are not required.

The driver proxies the Virtual Desktop DirectMode HMD without installing a DLL in VRChat. Each eye uses a separate native worker and shared D3D11/D3D12 textures and fences on the same GPU. Replacement requires both results from the current stereo frame. Model initialization and feature warmup happen outside render callbacks.

Short input and processing deadlines have been removed. Completion events wake the waiting driver, while control polling remains responsive to OFF. A two-second recovery watchdog remains for prolonged stalls/errors. Source ownership is retained until outstanding source reads finish or device removal is confirmed; this safety wait can exceed the recovery watchdog during a GPU stall. This does not guarantee a particular headset frame rate.

## Build and runtime layout

Requirements: Visual Studio 2022 Community with Desktop development with C++, Windows SDK, and .NET 8 SDK. Build scripts currently use the Community `vcvars64.bat` path.

From the repository root:

```powershell
cmd /c ScreenNative\build.cmd
cmd /c SteamVR\driver\build.cmd
dotnet build "DLSS 5 MANAGER.fsproj" -c Debug -o artifacts/app-vr
```

The driver build also runs its control and mock passthrough checks. For a standalone deployment, copy `SteamVR/driver` to `artifacts/vr-feasibility/driver-live-flow`, including `bin/win64/driver_codex_live_flow.dll`. Populate its `engine/left` and `engine/right` directories with `ScreenNative/nvngx.dll`, `nvngx.dll_ns-forwarder.dll`, `Spout.dll`, `SpoutDX.dll`, and `libraries/nvngx_dlssnr.dll`. The NVIDIA NR runtime model and the Manager's Python/capture/NR distribution assets must be supplied from the release payload; they are not added by this commit. The Manager resolves this sibling driver layout from its application directory.

Registration scripts are deployment tools: run them from the completed runtime directory with SteamVR closed. They preserve other driver registrations. `restore-baseline.cmd` requires the separately preserved sibling `driver-frame-output-sync` payload. No registration change is needed for the already installed working deployment.

For GPU worker checks, build `ScreenNative/build-vr-gpu-test.cmd`. For isolated stereo route checks, populate the driver's engine directories first, then run `SteamVR/driver/build-live-route-test.cmd -FullResolution`. This copies the payload into `tests/latest` so test control files do not overwrite a running Manager's control file.

The headless UI test is `Tests/VrUi/vr-ui-check.csproj`; it references the application built into `artifacts/app-vr`. Run it from the repository root with `dotnet run --project Tests/VrUi/vr-ui-check.csproj`.

## Verification checkpoint — 2026-10-11

Native worker/forwarder, SteamVR driver, and .NET Debug builds passed. UI control checks passed. The mock HMD route used real GPU/NR processing, including full 2496×2688 stereo input, normal 2/6 passes, Fast ON/OFF, delayed input/worker completion, OFF cancellation, stall recovery, and prevention of stale stereo output. Mock HMD results are separate from headset observations; generated test logs are not versioned.

After the short deadlines were removed, the user confirmed the updated feature works in VRChat through Virtual Desktop on Quest 3. This is a user-confirmed functional checkpoint, not a measured frame-rate result.

Generated native binaries, test outputs, runtime state, and NVIDIA model payloads are excluded from new commits. Existing upstream SDK/import libraries and binaries are left unchanged.
