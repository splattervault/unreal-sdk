# Fab submission checklist (code plugin)

Fab takes one packaged plugin per engine version. For each supported version (5.3, 5.4, 5.5, 5.6, 5.7, 5.8...):

1. Copy the plugin to a clean folder and set `"EngineVersion": "5.X.0"` in `SplatterVault.uplugin` (Fab checks it
   against the engine the package is for). Keep `"Installed": false`, `"CanContainContent": false`, and fill
   `"MarketplaceURL"` with the Fab listing URL once it exists (`"FabURL"` on newer engines).
2. Build it with that engine, warning-free:

   ```bat
   "C:\Program Files\Epic Games\UE_5.X\Engine\Build\BatchFiles\RunUAT.bat" BuildPlugin ^
     -Plugin="C:\build\src-5.X\SplatterVault\SplatterVault.uplugin" ^
     -Package="C:\build\out-5.X" -TargetPlatforms=Win64+Android -Rocket
   ```

3. Zip the package **without** `Binaries/` and `Intermediate/` (Fab rebuilds them). The zip holds
   `SplatterVault.uplugin`, `Source/`, `Resources/Icon128.png`, `Config/FilterPlugin.ini`, `README.md`, `CHANGELOG.md`,
   `LICENSE`, `Docs/`.
4. Listing: category Code Plugins; supported platforms Win64 and Android (Meta Quest) only: `PlatformAllowList` in
   `SplatterVault.uplugin` names just those two (the platforms built and tested). Add Mac, iOS or Linux to it only after
   building them (they would use the obfuscated token store). State that the plugin needs a SplatterVault account and calls `https://splattervault.com/rest`
   (network access), that it stores sign-in secrets on the device, and link the documentation (README) and support
   page. Screenshots: the Project Settings page and a Blueprint graph with the async nodes.
5. `DocsURL` and `SupportURL` in `SplatterVault.uplugin` point at `https://github.com/splattervault/unreal-sdk`: that
   public repository (README and issues) must exist before submitting.
6. Every `.cpp`/`.h`/`.cs` starts with the copyright line; the license is MIT (Fab also accepts its standard license:
   the MIT text ships in `LICENSE`).

Known engine-version notes:

- UE 5.3 with Visual Studio 17.10+ (MSVC 14.40+): engine headers fail on `__has_feature`. `SplatterVault.Build.cs`
  works around it for 5.3 only (`PLATFORM_HAS_ASAN_INCLUDE=0`). Epic's own 5.3 toolchain (MSVC 14.38) does not need it.
- UE 5.3-5.5: the module compiles without the shared engine PCH, whose `Engine.h` brings an engine deprecation
  warning (`FBoneMirrorInfo`, SkeletalMesh.h) into an otherwise warning-free build log.
- `-Rocket` is accepted and ignored by newer engines; it marks the build as "installed engine".
