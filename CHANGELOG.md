# Changelog

## 1.0.3

- Better pinch accuracy. A VR log showed quick pinches being recognised only when the fingers
  were already nearly touching (about 1.2 cm), then dropped after 0.3 s because the tracked gap
  while touching bounced between about 0.8 and 2.5 cm without crossing the pinch mark. The pinch
  then registered later as a slow pinch, with the thumb snapping in from the open-hand distance,
  which is what put it off target.
  - Quick pinches are recognised earlier, from about 3 cm (closing at least 1 cm, faster than
    10 cm/s, down from 15 cm/s).
  - Once a quick pinch gets within 0.4 cm of the pinch mark, it counts as landed and is held,
    even if the bouncing gap never crosses the mark. The thumb is no longer pushed back out
    while your fingers touch.

## 1.0.2

- Fixed: the laser could stay on while a finger rested close to the thumb. After a quick move
  toward the thumb, tracking jitter dipping under the pinch mark counted as a landed pinch,
  and the filter then kept passing real positions through, which X-Plane saw as a half pinch,
  until the fingers opened past 3 cm. A quick pinch is now held as a firm pinch once it lands,
  the same as a slow pinch.
- A quick pinch now needs a real closing movement (at least 1.5 cm within 0.3 s), so tracking
  jitter on a resting hand can't trigger it, and a quick close that doesn't become a pinch within
  0.3 s is hidden again.

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
