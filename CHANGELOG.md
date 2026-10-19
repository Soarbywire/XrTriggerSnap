# Changelog

## 1.0.1

- Fixed: finger-and-thumb pinches could land off target or not register, while laser clicks
  worked. To hide a half pinch, the filter reports your thumb further away than it is, and
  X-Plane places a direct pinch between your thumb and index fingertips, so that point was
  being pulled off target. Quick, deliberate pinches now pass your real finger positions
  through, so they land exactly where your fingers are. Slow drift and resting near-pinches
  are still hidden, so the laser stays away.
- New setting: "Quick pinches use your real finger positions" (on by default). Turn it off
  for the 1.0.0 behaviour.
- During a quick pinch you may see the laser flicker briefly, the same as without the filter.
- The log now shows quick pinches and how many happened in each 5-second summary.

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
