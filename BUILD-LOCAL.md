# Local development build

Run from this project folder in Windows PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Build-Local.ps1
```

Requires Git, .NET 8 SDK and Visual Studio 2022 Community with Desktop development with C++ / Windows SDK. Native builds use the original `ScreenNative/build.cmd` and `SteamVR/driver/build.cmd` in an isolated staging folder so tracked upstream binaries remain untouched.

Outputs are under this repository's ignored `artifacts` folder:

- `app-vr/DLSS 5 MANAGER.exe`: Debug app with Desktop / SteamVR modes, Fast and normal Multi-pass.
- `vr-feasibility/driver-live-flow`: complete SteamVR driver with both eye workers and NR model.
- `build/ScreenNative`: rebuilt worker, forwarder and Fast GPU probe.
- `build/SteamVR/driver`: rebuilt driver and control/mock tests.
- `build-logs`: compiler and check results.

The tested Python/capture controller and release runtime payload are retained locally in `artifacts/runtime-seed/mod files`. This build uses no Codex workspace paths. These runtime files and model binaries are not uploaded to GitHub. On a fresh clone, import a complete tested runtime once:

```powershell
.\Build-Local.ps1 -RuntimeSource 'D:\your-tested-app\mod files'
```

By default, the build runs driver control/mock tests, Fast settings and D3D12 GPU checks, and headless VR UI checks. `-SkipChecks` skips the optional Fast/UI checks. A real headset comparison is still needed to judge visual quality.

The UI controls the packaged driver via its relative path. SteamVR must register that exact new package path; fully exit SteamVR before registering. When relocating from an older package, remove the old Live Flow registration as well to avoid registering two proxy drivers. Preserve Virtual Desktop's registration. Building alone does not alter registrations.

This is a Debug development build of the available upstream source plus the two feature commits, not a new official upstream release.
