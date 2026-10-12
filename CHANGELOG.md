# Changelog

## 1.0.0

First release.

- Stops X-Plane's VR laser from appearing on a half pinch during hand tracking (tested with
  Apple Vision Pro over CloudXR on X-Plane 12.4.3).
- Thumb–index pinch is reported to X-Plane as fully open or fully pinched, with adjustable
  pinch, release and open-hand distances (defaults 1.1 / 3.0 / 7.0 cm).
- Also snaps 0–1 trigger and pinch values for controllers and runtimes that report them.
- Tray app: on/off, per-hand filtering, settings, install/remove, start with Windows,
  start with X-Plane.
- Settings apply within a second, even mid-flight.
- Automatic updates from GitHub Releases, verified with SHA-256 and installed only while
  X-Plane isn't in VR.
