# XrTriggerSnap

Stops X-Plane's VR laser from appearing on a half pinch when you use hand tracking.
Tested with Apple Vision Pro over CloudXR on X-Plane 12.4.3.

X-Plane reads your hand skeleton and treats a partly closed pinch as "half pressed", which brings
up the laser. XrTriggerSnap sits between X-Plane and the headset runtime as an OpenXR layer and
reports your hand as either fully open or fully pinched, never anything in between. A deliberate
pinch still works; reaching for a switch with your thumb drifting toward your index finger no
longer brings up the laser.

**Download:** [latest release](https://github.com/Soarbywire/XrTriggerSnap/releases/latest), file
`XrTriggerSnap-<version>.zip`.

## What's in the folder

| File | What it is |
|---|---|
| `XrTriggerSnap.exe` | Tray app: on/off, settings, install/remove, startup options, updates |
| `XR_APILAYER_trigger_snap.dll` | The filter itself; X-Plane loads it when VR starts |
| `XR_APILAYER_trigger_snap.json` | Tells OpenXR where the DLL is |
| `trigger_snap.ini` | Your settings (the tray app edits this for you) |
| `README.md` | This file |

Keep them together in one folder.

## Install

1. Unzip into a permanent folder you can write to, e.g. `C:\X-Plane 12\XrTriggerSnap`
   (not `C:\Program Files`, or updates can't be installed).
2. Run `XrTriggerSnap.exe`. It offers to install the layer; Windows asks for admin permission once.
3. Start X-Plane in VR. Hover over the tray icon and it says **Active in X-Plane**.

**Upgrading from a test build (anything before 1.0):** quit X-Plane, copy the new files over the
old ones, delete `install.bat`, `uninstall.bat` and `install.ps1` if they're there, then run
`XrTriggerSnap.exe`. From 1.0 onwards, updates install themselves.

## How it starts

You don't need to start anything before flying. Installing the layer writes one permanent
registry entry (`HKLM\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit`). Every time X-Plane starts VR,
OpenXR reads that entry and loads the filter into X-Plane automatically. That's standard OpenXR
behaviour, not something X-Plane or this app does.

The tray app is only the control panel. The filter keeps working even when it's closed.

- **Start with X-Plane** (on by default): when X-Plane starts VR, the filter opens the tray app,
  and the tray app closes itself about 10 seconds after X-Plane exits.
- **Start with Windows** (off by default): the tray app opens at sign-in and stays in the tray.
  Only needed if you want the icon before launching X-Plane.

## The tray icon

Green = filter on. Grey = filter off, or layer not installed. Hover for status.

- **Double-click:** settings
- **Right-click:** Filter on/off, Hands (both/left/right), Settings, Start with Windows,
  Start with X-Plane, Install/Remove layer, Open log, Open folder, Check for updates,
  Project page on GitHub, About, Exit. When an update is available, **Install update** appears
  at the top.

## Settings

All changes apply within a second, even mid-flight. No X-Plane restart needed.

| Setting | Default | What it does |
|---|---|---|
| Filter on | On | Off = X-Plane behaves exactly as if this wasn't installed. |
| Apply to | Both hands | Limit the filter to one hand. |
| Pinch at | 1.1 cm | Thumb-to-index gap that counts as a pinch. Lower = fewer accidental pinches; too low and real pinches won't register. Touching fingertips track at about 1–1.5 cm. |
| Release at | 3.0 cm | Once pinched, it stays pinched until your fingers open this far, so a shaky pinch doesn't flicker. |
| Open hand | 7.0 cm | Any half pinch is reported to X-Plane as this gap. |
| Start with Windows / X-Plane | Off / On | See [How it starts](#how-it-starts). |
| Check for updates automatically | On | Checks GitHub shortly after the tray app starts, then every 6 hours. |
| Install updates automatically | On | Downloads, verifies and installs new versions on its own. Off = you're told an update is available and install it from the menu. |
| Logging | Normal | Writes `trigger_snap.log`. "Detailed" also logs every trigger value; "Off" writes nothing. |

**Defaults** in the settings window puts everything except Start with Windows back to the values
above (press Save to keep them).

Note that a deliberate pinch still shows the laser. That's how X-Plane does far interaction, so
for switches within reach, poke them with your fingertip.

## Updates

The tray app checks this repository's
[latest release](https://github.com/Soarbywire/XrTriggerSnap/releases/latest). When a newer
version appears it:

1. downloads the new files in the background,
2. checks each one against the release's `SHA256SUMS.txt` and discards anything that doesn't match,
3. waits until X-Plane isn't in VR, so it never touches files X-Plane is using mid-flight,
4. swaps the files in, restarts itself, and shows "XrTriggerSnap updated".

Your `trigger_snap.ini` is never replaced, so your settings carry over. To check straight away, use
**Check for updates** in the tray menu or **Check now** in settings.

If an update can't be installed (for example the folder is read-only), the reason appears in
settings, and you can always install by hand from the releases page.

## Reading the log

`trigger_snap.log` is next to the program (or in `%TEMP%` if that folder isn't writable).

- **`started tray app` / `hand tracker created`**: the filter is loaded and reading your hands.
- **`right pinch ON at 1.2cm` / `off at 3.1cm`**: each pinch it detected.
- **`activity (last 5s)`**: the closest thumb-index gap seen, which helps when tuning "Pinch at".
- **`settings reloaded`**: a change from the tray app was picked up.

## Troubleshooting

- **Tray says "Waiting for X-Plane" while you're in VR:** the layer isn't loading. Right-click
  the tray icon and choose Install layer, then restart X-Plane.
- **"Layer installed from another folder":** you moved the folder. Choose Install layer from
  this folder.
- **Pinches don't register:** raise Pinch at (try 1.3).
- **Laser still appears when you don't mean it to:** lower Pinch at (try 1.0) and check the log's
  `activity` lines for how close your fingers get when relaxed.

## Uninstall

Right-click the tray icon and choose **Remove layer...**, untick **Start with Windows** if you
ticked it, then **Exit** and delete the folder. Nothing else is left behind.

---

## For developers

### Layout

| Path | Contents |
|---|---|
| `version.h` | Version number and the GitHub repo the updater checks |
| `src/layer.cpp` | The OpenXR API layer (hand-skeleton and trigger snapping, live ini reload) |
| `tray/` | Tray app: `tray.cpp` (UI), `updater.cpp` (download/verify/swap), `update_logic.h` (portable parsing) |
| `test/` | Host-side tests: the layer against a fake OpenXR runtime, and the updater's parsing |
| `dist/` | Manifest and default ini that ship with the binaries |
| `scripts/` | `build-windows.sh` (MinGW cross-build), `run-tests.sh` |
| `external/openxr/` | Khronos OpenXR headers (Apache-2.0) |

### Building

On Linux with MinGW-w64 (`g++-mingw-w64-x86-64-posix`), which is what the release workflow uses:

```
scripts/run-tests.sh
scripts/build-windows.sh build/package
```

Or on Windows with Visual Studio:

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

Neither build needs a Visual C++ redistributable at runtime.

### Releasing a new version

1. Bump the numbers in `version.h`.
2. Add a `## <version>` section at the top of `CHANGELOG.md`; it becomes the release notes.
3. Commit and push to `main`.

When `version.h` changes on `main`, the **Release** workflow runs the tests, builds the Windows
binaries, and publishes a GitHub release (tag `v<version>`) with the zip, the individual files and
`SHA256SUMS.txt`. Installed copies pick it up within 6 hours, or at their next start. You can
also start it by hand from the Actions tab, or by pushing a `v<version>` tag. If a release for that
version already exists, it does nothing.
