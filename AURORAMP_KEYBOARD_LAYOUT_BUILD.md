# AuroraMP CEF native keyboard-layout event

This patch targets AuroraMP CEF commit:

```text
6a7412ad0caf6236e5a3058515927fb1f3b131c8
```

## What it adds

The Windows client listens for `WM_INPUTLANGCHANGE`, converts the active `HKL`
to a BCP 47 locale such as `ar-EG` or `en-US`, and emits a client-only CEF
event to every browser:

```javascript
cef.on("cef:keyboard_layout", locale => {
    console.log(locale);
});
```

The event is sent immediately when the input language changes. The currently
active locale is also sent when each browser is created. Nothing is sent to
Pawn or across the network.

## Apply the patch

From a clean clone of AuroraMP CEF:

```powershell
git checkout 6a7412ad0caf6236e5a3058515927fb1f3b131c8
git submodule update --init --recursive
git apply auroramp-keyboard-layout-native.patch
```

## Easiest build: GitHub Actions

1. Push the patched source to your own GitHub repository.
2. Open **Actions**.
3. Select **Build Custom Keyboard Layout Client**.
4. Choose **Run workflow**.
5. Download the `client-files-vkeyboard-layout` artifact after it finishes.

Distribute the complete generated client package to players. Do not mix CEF
runtime files from different AuroraMP releases.

## Manual Windows build

Use Visual Studio 2022, CMake, the Windows SDK, and vcpkg. Build the client as
Win32/x86, matching the official workflow:

```powershell
cmake -B build -S . `
  -A Win32 `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_FEATURE_FLAGS="manifests;binarycaching" `
  -DVCPKG_MANIFEST_FEATURES=client `
  -DVCPKG_TARGET_TRIPLET=x86-windows-static `
  -DDEV_ALL_TARGETS=OFF `
  -DBUILD_CLIENT=ON `
  -DBUILD_SERVER_OMP=OFF `
  -DBUILD_SERVER_SAMP=OFF

cmake --build build --config Release --parallel
```

Expected binaries:

```text
build/src/client/core/Release/client.dll
build/src/client/loader/Release/cef.asi
build/src/client/renderer/Release/Renderer.exe
```

## Runtime verification

Enable debug logging in the player's CEF config. A language switch should add
a line similar to this to the client log without requiring any typed text:

```text
[CEF] Active keyboard layout changed to 'ar-EG'.
```
