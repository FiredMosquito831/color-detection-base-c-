# Color Detection Engineering PoC

This repository is a Windows computer-vision, tracking, pinning, and event-control proof of concept. It captures one center-screen region, classifies configured colors, extracts connected components, applies temporal confidence, draws a click-through diagnostic overlay, and demonstrates hold-to-trigger and hold-to-pin controllers.

The default is detector-only: detections are visualized and logged, but no input is emitted. `--trigger` and `--track` explicitly enable the two demonstration controllers.

## What changed in version 2

- One `BitBlt` capture per frame instead of one `GetPixel` call per pixel.
- Actual primary-screen dimensions instead of a hard-coded 1920x1080 center.
- Perceptual CIE Lab color distance instead of independent RGB channel cubes.
- Saturation rejection, connected components, minimum area, and gate overlap.
- Confidence scoring from color quality, area, gate overlap, and centrality.
- Multi-frame confirmation, multi-frame release, rising-edge events, and cooldown.
- Hold-to-trigger mode: Mouse 4 arms a center-gated, temporally confirmed event.
- Hold-to-pin mode: Mouse 5 selects the closest candidate and follows its centroid.
- Persistent target identity based on frame-to-frame centroid association.
- Adjustable target smoothing, cursor gain, dead zone, maximum step, reacquisition radius, and occlusion tolerance.
- Non-blocking event path: no 100 ms mouse hold and no synchronous `Beep` in detection.
- Thread-safe immutable overlay snapshots; Win32 painting stays on the UI thread.
- Correct color-keyed, click-through, capture-excluded diagnostic overlay.
- Pinned-target box, cursor-to-target link, visible/occluded state, and candidate count.
- Optional calibrated monocular distance estimate from apparent object height.
- Separate moving-target presentation fixture with an on-screen click counter.
- Command-line configuration, CMake builds, synthetic tests, and a reproducible benchmark.

The historical `failedAtttemptWithDetectionSquares.cpp` is retained for comparison but is not included in the build.

## Build and test

From PowerShell with CMake, Ninja, and a C++20 compiler available:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Run detector-only mode:

```powershell
.\build\color_detector.exe
```

Run a short capture smoke test without showing the overlay:

```powershell
.\build\color_detector.exe --no-overlay --max-frames 120
```

Add `--diagnostics` to print candidate counts once per second.

Enable the optional click event for an authorized test window:

```powershell
.\build\color_detector.exe --trigger
```

Enable nearest-target pinning and cursor following:

```powershell
.\build\color_detector.exe --track
```

Enable both university demonstrations:

```powershell
.\build\color_detector.exe --trigger --track
```

Hold `Mouse 4` for trigger mode. Hold `Mouse 5` for pin/track mode. `F8` pauses or resumes the entire system, and `F9` exits cleanly.

## University presentation fixture

The build includes `color_target_demo.exe`, a controlled scene with three independently moving yellow targets:

- Target A repeatedly crosses the exact center gate.
- Targets B and C follow separate paths for nearest-target selection.
- The fixture counts clicks it receives, making trigger output directly observable.
- Cursor movement makes smoothing, dead zone, pin retention, and reacquisition visible.

Run the fixture:

```powershell
.\build\color_target_demo.exe
```

In a second PowerShell window, run the detector:

```powershell
.\build\color_detector.exe --trigger --track --delta-e 12 --min-area 20
```

Bring the target fixture to the foreground. Hold `Mouse 4` while target A crosses the center to demonstrate the trigger. Hold `Mouse 5` to select and follow the closest target. Press `Esc` in the fixture to close it.

## Important options

| Option | Meaning | Default |
|---|---|---:|
| `--trigger` | Enable hold-to-trigger controller | disabled |
| `--track` | Enable hold-to-pin/follow controller | disabled |
| `--trigger-key KEY` | Trigger activation key | `Mouse 4` |
| `--track-key KEY` | Tracking activation key | `Mouse 5` |
| `--track-gain VALUE` | Cursor proportional gain | `0.45` |
| `--track-deadzone PX` | No movement inside this error | `3` |
| `--track-max-step PX` | Maximum movement per frame | `35` |
| `--reacquire-radius PX` | Identity-association search radius | `80` |
| `--occlusion-frames N` | Missing frames retained before dropping pin | `6` |
| `--pin-smoothing VALUE` | Target centroid EMA factor | `0.35` |
| `--diagnostics` | Log candidate counts once per second | disabled |
| `--roi WIDTHxHEIGHT` | Captured center region | `256x192` |
| `--gate PIXELS` | Center activation square | `8` |
| `--fps FPS` | Target capture frequency | `120` |
| `--target R,G,B` | Color prototype; repeat to add colors | legacy yellow |
| `--delta-e VALUE` | Maximum Lab color distance | `24` |
| `--min-saturation N` | Reject low-chroma pixels | `35/255` |
| `--min-area PIXELS` | Minimum connected component | `8` |
| `--min-gate PIXELS` | Component pixels required in activation gate | `2` |
| `--max-area-ratio VALUE` | Reject implausibly large/background components | `0.25` |
| `--confidence VALUE` | Final acceptance score | `0.55` |
| `--confirm FRAMES` | Consecutive detections before activation | `3` |
| `--release FRAMES` | Consecutive misses before release | `3` |
| `--cooldown-ms MS` | Minimum time between rising-edge events | `250` |

The built-in target is the legacy yellow prototype. The previous broad dark-red prototype is intentionally opt-in because it produced large false-positive background components during live smoke testing. The first custom `--target` replaces the built-in color; subsequent occurrences add prototypes:

```powershell
.\build\color_detector.exe --target 255,255,35 --target 245,210,20 --delta-e 16
```

## Distance model

Distance is disabled unless both calibration values are supplied:

```powershell
.\build\color_detector.exe --known-height 1.80 --vfov 60
```

The estimate uses the pinhole-camera relationship:

```text
focalLengthPixels = frameHeight / (2 * tan(verticalFov / 2))
distance = knownObjectHeight * focalLengthPixels / observedBoundingBoxHeight
```

This is only meaningful when the complete object is segmented, the real height is known, and the vertical FOV is correct for the current camera/zoom. The application suppresses the estimate when the component touches an ROI boundary because its apparent height is then clipped. Cropping, partial visibility, pose, perspective and post-processing introduce error. If those conditions cannot be controlled, treat the output as a relative range proxy rather than an absolute measurement.

## Calibration workflow

1. Start in dry-run mode with a narrow `--delta-e` such as `10` or `12`.
2. Capture representative positive and negative screenshots under every expected lighting/display condition.
3. Add color prototypes sampled from true positives rather than only widening Delta E.
4. Increase `--min-area` until isolated noise disappears.
5. Set `--min-gate` to require meaningful overlap with the activation region.
6. Tune `--confirm` and `--release` using recorded sequences, not individual screenshots.
7. Measure precision, recall, false events per minute, and processing-time percentiles.
8. Enable an output action only after the dry-run confusion matrix is acceptable.

GPU processing is not automatically faster for a small ROI. At this scale, capture synchronization and memory transfer dominate; keeping the ROI on the GPU becomes useful only if the application expands to larger-frame segmentation or already receives a GPU surface.

## Architecture

```text
screen ROI capture
    -> Lab prototype matching + saturation gate
    -> 8-connected components
    -> area/confidence candidate set
       |-> center-gate confirmation -> temporal hysteresis -> trigger event
       `-> nearest selection -> identity association -> smoothed pin controller
    -> immutable UI snapshot
    -> overlay/log/presentation outputs
```

`detector_core.*` is platform-independent and directly synthetic-testable. `main.cpp` owns Windows capture, input, hold keys, lifecycle, and overlay rendering. `target_demo.cpp` is the controlled presentation fixture.
