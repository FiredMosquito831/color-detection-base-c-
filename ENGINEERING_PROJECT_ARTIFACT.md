# Color Detection, Trigger, Tracking, Pinning, and Range-Estimation System

## Complete Engineering Project Artifact

**Project type:** Windows C++ computer-vision and control-systems proof of concept  
**Purpose:** University presentation of real-time color detection, center-gated event triggering, nearest-target selection, persistent target pinning, cursor following, confidence filtering, and calibrated monocular range estimation  
**Current workspace:** `C:\Users\fgghk\PycharmProjects\TranscriptionAppRelease\colorobotTEST\C version`  
**Upstream repository:** `FiredMosquito831/color-detection-base-c-`  
**Verified upstream starting commit:** `a0e1b4fe3c5bfce7bef31e7b3f007c7a3269865f`  
**Artifact date:** 2026-07-30  
**Current source state:** Implemented and tested locally; not committed and not pushed

---

## 1. Executive summary

The original repository was a minimal Win32 color-trigger experiment. It sampled a small fixed area around the center of a hard-coded 1920x1080 screen, compared individual RGB channels against one or two fixed colors, and immediately emitted a left click whenever any sampled pixel matched.

The project has been transformed into a modular, testable engineering system with:

- One-block screen capture instead of one operating-system call per pixel.
- Actual primary-screen geometry instead of fixed 1920x1080 coordinates.
- Perceptual CIE Lab color comparison.
- Saturation filtering.
- Connected-component extraction.
- Component bounding boxes, centroids, area, gate overlap, and confidence.
- Noise, off-center, and oversized-background rejection.
- Multi-frame confirmation and release hysteresis.
- Rising-edge-only trigger events.
- Hold-to-trigger activation on **Mouse 4**.
- Hold-to-pin/track activation on **Mouse 5**.
- Nearest-target selection.
- Persistent target identity between frames.
- Target-position smoothing.
- Temporary-occlusion tolerance.
- Configurable cursor-follow control.
- A thread-safe diagnostic overlay.
- Optional calibrated monocular distance estimation.
- Complete command-line configuration.
- A dedicated moving-target university demonstration fixture.
- CMake build configuration.
- Synthetic unit and regression tests.
- Live screen-capture validation.
- A repeatable performance benchmark.

When started **with** arguments, the executable remains detector-only unless
`--trigger`, `--track`, or both are explicitly supplied.

When started with **no** arguments, which is the double-click case where there
is no opportunity to pass flags, both controllers are enabled and the process
requests elevation. `--detector-only` restores observation-only behavior.

---

## 2. Original repository state

The verified upstream repository contained only:

- `main.cpp`
- `failedAtttemptWithDetectionSquares.cpp`

It did not contain:

- A build system.
- A README.
- Tests.
- A calibration workflow.
- A configuration format.
- Error handling.
- Clean thread shutdown.
- Reusable detection modules.

`main.cpp` contained several historical versions embedded as large comments. The active version used:

- Target color 1: `RGB(255, 255, 35)`.
- Target color 2: `RGB(96, 50, 43)`.
- Per-channel RGB tolerance: `40`.
- Fixed center: `(960, 540)`.
- Fixed detection area: `6x6`.
- Thirty-six `GetPixel` calls per scan.
- A match condition satisfied by one pixel.
- A blocking 100 ms mouse hold.
- A blocking 50 ms beep.
- A 10 ms sleep.
- A continuously repeating event while the color remained present.
- A level-polled minus key for pause/resume.

The overlay experiment attempted to draw rectangles but:

- Always drew the same fixed center rectangle.
- Did not discover target boundaries.
- Modified a global `std::vector<RECT>` from one thread while painting it from another.
- Used whole-window opacity instead of a transparent drawing surface.
- Could add the same rectangle once for every matching pixel.
- Painted and invalidated from the detection worker.
- Had incomplete GDI object restoration.
- Had no clean `WM_DESTROY` shutdown path.

The historical overlay source remains in the repository for comparison but is excluded from the new build.

---

## 3. Problems identified in the original detector

### 3.1 Excessively broad RGB classifier

The original active classifier used independent channel tolerances:

```text
abs(R - targetR) <= tolerance
abs(G - targetG) <= tolerance
abs(B - targetB) <= tolerance
```

This forms an axis-aligned cube in RGB space rather than a perceptual color neighborhood.

For the two active colors with tolerance 40:

- Yellow prototype accepted 127,756 RGB values.
- Dark-red prototype accepted 531,441 RGB values.
- Their red ranges did not overlap.
- Combined accepted values: 659,197.
- Combined share of 24-bit RGB space: approximately 3.9291%.

One matching pixel out of 36 was sufficient to emit an event.

### 3.2 No object model

The original program did not identify:

- A complete target.
- A connected region.
- A bounding box.
- A centroid.
- Area.
- Shape.
- Multiple targets.
- Target identity.

It answered only: “Did any pixel match?”

### 3.3 No temporal evidence

A one-frame anomaly was treated exactly like a persistent target. There was no:

- Confirmation window.
- Release threshold.
- Hysteresis.
- Rising-edge state.
- Cooldown.

### 3.4 Blocking response path

After a match, the active version blocked for at least:

```text
100 ms mouse hold
+ 50 ms beep
+ 10 ms sleep
+ capture and processing overhead
```

That created a blind period of more than 160 ms.

### 3.5 Repeated triggering

As long as a matching pixel remained in the gate, the original loop continued to click. It did not distinguish:

- Target entered.
- Target remained.
- Target exited.
- Target re-entered.

### 3.6 Hard-coded geometry

The detector assumed:

- 1920x1080 resolution.
- Primary screen origin `(0, 0)`.
- No DPI-related coordinate differences.
- No resolution changes.

### 3.7 Unreachable cleanup

The infinite detection loop prevented normal execution of `ReleaseDC`. Both worker threads also ran forever and were immediately joined.

---

## 4. Design goals of the new system

The new implementation was designed around the following goals:

1. Separate platform-independent detection logic from Win32 capture and input.
2. Replace per-pixel OS calls with one frame-region transfer.
3. Use perceptual color comparison.
4. Require spatially coherent evidence.
5. Require temporally coherent evidence.
6. Support multiple simultaneous candidate targets.
7. Support center-gated trigger demonstrations.
8. Support nearest-target pinning and tracking demonstrations.
9. Preserve target identity through movement and brief occlusion.
10. Make behavior observable through an overlay and diagnostics.
11. Keep every important threshold configurable.
12. Provide a controlled presentation fixture.
13. Make core behavior deterministic and synthetic-testable.
14. Keep the default mode non-actuating.
15. Provide clean startup, pause, shutdown, and resource ownership.

---

## 5. Final architecture

```text
Primary-screen ROI
    |
    v
Single BitBlt capture into top-down 32-bit DIB
    |
    v
Saturation pre-filter
    |
    v
sRGB -> linear RGB -> XYZ D65 -> CIE Lab
    |
    v
Minimum Delta E against configured color prototypes
    |
    v
Binary matching-pixel mask
    |
    v
8-connected component extraction
    |
    +--> area / bounding box / centroid / gate overlap / mean Delta E
    |
    v
Noise and oversized-component rejection
    |
    v
Confidence-scored candidate set
    |
    +-------------------------------+
    |                               |
    v                               v
Center-gate candidates          All valid candidates
    |                               |
    v                               v
Temporal confirmation          Nearest-target selection
and release hysteresis             |
    |                               v
    v                           Previous-centroid association
Rising-edge trigger                |
    |                               v
    v                           Target-position smoothing
Optional click                      |
                                    v
                                Occlusion retention
                                    |
                                    v
                                Proportional cursor following

Both branches publish one immutable UI snapshot
    |
    v
Transparent overlay + diagnostics + range estimate
```

---

## 6. Repository layout

### `main.cpp`

Owns the Windows-specific application:

- Command-line parsing.
- Key-name parsing.
- Primary-screen discovery.
- DPI awareness.
- Screen capture resources.
- Mouse button hold-state checks.
- Click output.
- Relative cursor movement.
- Overlay window creation.
- Hotkeys.
- UI painting.
- Detection worker.
- Thread synchronization.
- Runtime lifecycle.
- Diagnostics.

### `detector_core.hpp`

Defines the platform-independent data model:

- `Rgb`
- `Lab`
- `ColorPrototype`
- `BoundingBox`
- `Candidate`
- `DetectionConfig`
- `DetectionResult`
- `TemporalConfig`
- `TemporalDecision`
- `TemporalGate`
- `TrackerConfig`
- `TrackState`
- `TargetTracker`

### `detector_core.cpp`

Implements:

- sRGB linearization lookup table.
- RGB-to-CIE-Lab conversion.
- Delta E 76 distance.
- Saturation measurement.
- Pixel segmentation.
- Connected components.
- Candidate confidence.
- Monocular distance.
- Temporal confirmation/release state machine.
- Persistent nearest-target tracker.

### `target_demo.cpp`

Implements the controlled university presentation fixture:

- Three moving yellow targets.
- Target A crosses the exact screen center repeatedly.
- Targets B and C follow independent paths.
- Visible center crosshair.
- Click counter.
- Topmost deterministic presentation window.
- Escape-to-close.
- Optional timed auto-close argument for automated testing.

### `tests/detector_core_tests.cpp`

Contains synthetic tests for:

- Color conversion.
- Delta E identity.
- Connected-component detection.
- Bounding-box dimensions.
- Exact component area.
- Single-pixel rejection.
- Off-gate rejection.
- Oversized-component rejection.
- Temporal confirmation.
- Temporal release.
- Rising-edge behavior.
- Target selection.
- Target identity association.
- Position smoothing.
- Occlusion retention.
- Pin release.
- Distance estimation.
- Invalid distance input.
- Synthetic performance benchmark.

### `CMakeLists.txt`

Builds:

- `detector_core` library.
- `color_detector.exe`.
- `color_target_demo.exe`.
- `detector_core_tests.exe`.

It enables C++20 and strict warnings for MSVC or GCC/MinGW.

### `README.md`

Contains concise operator documentation, build instructions, presentation commands, options, calibration, and architecture.

### `.gitignore`

Excludes build output and common compiler artifacts.

### `failedAtttemptWithDetectionSquares.cpp`

Retained as historical reference. It is not compiled.

---

## 7. Screen capture subsystem

The original 36 `GetPixel` calls were replaced by `ScreenRoiCapture`.

### Current process

1. Obtain one screen device context.
2. Create one compatible memory device context.
3. Create one 32-bit top-down DIB section.
4. Select the bitmap into the memory context.
5. Copy the complete ROI using one `BitBlt`.
6. Analyze the directly accessible BGRA memory.
7. Restore and release every GDI resource through RAII cleanup.

### Defaults

- ROI: `256x192`.
- Target rate: `120 FPS`.
- ROI position: center of the actual primary screen.

### Measured frame cost and why GPU compute is not the answer

A common instinct is to move the classifier to the GPU. Measurement does not
support it. For the default 256x192 ROI:

```text
BitBlt capture      wall 7.14 ms   CPU 0.25 ms
Desktop Duplication wall 7.41 ms   CPU 0.12 ms
analysis, default        0.48 ms
analysis, thermal-human  1.47 ms
```

Two conclusions follow.

First, capture dominates **wall** time but barely registers in **CPU** time.
The seven milliseconds are spent blocked waiting for the display to produce a
new frame, not computing. Under a continuously animating window both backends
observed roughly 63 genuinely distinct frames per second. That is the real
ceiling on reaction time: the detector cannot see a change before the display
publishes it, and no amount of compute acceleration moves that number.

Second, the analysis is already small. Moving 1.47 ms of work to the GPU could
save at most 1.47 ms of an 8.6 ms frame, while adding an upload, a kernel
launch, and a readback whose synchronization typically costs more than the
work saved. GPU offload is a throughput optimization, and this is a latency
problem on a small image.

The capture backend was still worth changing, for reasons other than speed:
Desktop Duplication halves CPU use and delivers tear-free composited frames,
whereas GDI reads the front buffer and can return partially composited
content.

### Desktop Duplication acquisition timeout

`AcquireNextFrame` is called with a small non-zero timeout. A zero timeout was
measured returning `DXGI_ERROR_WAIT_TIMEOUT` on every single call: **zero
distinct frames out of 250**, because a frame is essentially never already
pending at the instant of the call. The buffer would then never be filled and
the detector would analyze blank pixels forever, silently. A timeout of a few
milliseconds recovers normal delivery, and a genuine timeout then means the
desktop really is unchanged, so reusing the previous buffer is correct rather
than stale.

### Why CPU processing is appropriate

At `256x192`, the classifier operates on 49,152 pixels. GPU acceleration would add synchronization and transfer overhead unless:

- The source already exists as a GPU surface.
- The ROI becomes substantially larger.
- More expensive segmentation is added.
- Several streams are processed simultaneously.

The arithmetic is not the primary bottleneck at the current size.

---

## 8. Color classification

### 8.1 Default prototype

The default prototype is:

```text
RGB(255, 255, 35)
```

The previous dark-red prototype:

```text
RGB(96, 50, 43)
```

is no longer enabled by default. It remains available through `--target`.

### 8.2 CIE Lab conversion

Each sufficiently saturated pixel is converted:

```text
sRGB
  -> linear RGB
  -> XYZ using the sRGB D65 matrix
  -> CIE L*a*b*
```

The 256 possible sRGB channel linearization values are precomputed in a lookup table.

### 8.3 Color distance

For every pixel, the detector calculates Delta E 76 against every configured prototype:

```text
DeltaE = sqrt(
    (L1 - L2)^2 +
    (a1 - a2)^2 +
    (b1 - b2)^2
)
```

The pixel matches when the smallest prototype distance is at most `--delta-e`.

Default:

```text
maximum Delta E = 24
```

The university fixture is intentionally exact and can use:

```text
--delta-e 12
```

### 8.4 Saturation rejection

Pixels below the configured saturation are rejected before Lab conversion.

Default:

```text
minimum saturation = 35 / 255
```

This prevents grayscale and low-chroma background pixels from entering the component stage.

---

## 9. Connected-component analysis

The binary matching mask is processed using 8-connectivity.

For every component, the detector calculates:

- Pixel area.
- Left, top, right, and bottom edges.
- Width and height.
- Centroid X and Y.
- Matching pixels inside the center gate.
- Mean Delta E.
- Final confidence.

### Rejection conditions

A component is rejected when:

- Area is smaller than `--min-area`.
- Area consumes more than `--max-area-ratio` of the ROI.
- Confidence is below `--confidence`.

For center-trigger eligibility, it must additionally contain at least `--min-gate` pixels inside the activation gate.

### Defaults

```text
minimum area              = 8 pixels
minimum gate overlap      = 2 pixels
maximum ROI area fraction = 0.25
minimum confidence        = 0.55
center gate               = 8x8
```

---

## 10. Confidence model

The current component confidence is:

```text
confidence =
    0.45 * colorScore +
    0.25 * areaScore +
    0.20 * gateScore +
    0.10 * centerScore
```

Where:

- `colorScore` increases as mean Delta E approaches zero.
- `areaScore` increases above the minimum-area threshold.
- `gateScore` increases with meaningful center-gate overlap.
- `centerScore` increases as the centroid approaches ROI center.

For tracking, off-gate candidates can still qualify because color, area, and centrality provide most of the score. For triggering, gate overlap is mandatory.

---

## 11. Temporal filtering

The `TemporalGate` converts raw center detection into stable state.

### Defaults

```text
confirmation frames = 3
release frames      = 3
retrigger cooldown  = 250 ms
```

### State behavior

```text
inactive
    |
    | 3 consecutive detections
    v
active + one trigger event
    |
    | persistent detection
    v
active, no repeated trigger
    |
    | 3 consecutive misses
    v
inactive
```

Releasing the trigger activation button resets the temporal gate. Pressing it again starts a new confirmation sequence.

---

## 12. Hold-to-trigger controller

### Default activation

```text
Mouse 4 / XBUTTON1
```

### Enable mode

```powershell
.\build\color_detector.exe --trigger
```

`--click` is retained as an alias for `--trigger`.

### Event requirements

All of the following must be true:

1. `--trigger` is enabled.
2. Mouse 4 is held.
3. A valid component overlaps the center gate.
4. Gate overlap is at least `--min-gate`.
5. Component confidence reaches `--confidence`.
6. Detection survives `--confirm` consecutive frames.
7. Cooldown permits a new rising-edge event.

### Action

The event sends one left-button-down, then one left-button-up after
`--click-hold-ms` milliseconds, as two separate `SendInput` calls.

Submitting both in a single batch, as an earlier revision did, gives them the
same timestamp and delivers them between two consecutive input polls. An
application that samples button state once per frame then sees the button down
and up within one sample and registers no click at all. This is why the
original click worked on the desktop but had no effect in a game.

The release is scheduled and performed by the detection loop rather than by
sleeping, so the hold duration costs no detection frames. Every exit path from
the loop, including pause and shutdown, releases a pending press, so the left
button can never be left stuck down.

Default:

```text
click hold = 40 ms
```

---

## 13. Hold-to-pin and tracking controller

### Default activation

```text
Mouse 5 / XBUTTON2
```

### Enable mode

```powershell
.\build\color_detector.exe --track
```

### Initial target selection

When Mouse 5 is first held, the tracker evaluates every valid candidate.

The selection score is:

```text
selectionScore =
    pixelDistanceFromSelectionOrigin +
    confidencePenaltyPixels * (1 - confidence)
```

Defaults:

```text
selection origin            = ROI center
confidence penalty          = 30 pixels
```

The candidate with the lowest score becomes pinned.

### Identity association

On later frames, the tracker does not simply choose the currently closest candidate again.

It selects the component whose centroid is closest to the previously observed centroid, provided the distance is within:

```text
reacquire radius = 80 pixels
```

This reduces target switching when another candidate temporarily becomes closer to screen center.

### Position smoothing

The target point is smoothed with an exponential moving average:

```text
smoothed =
    previousSmoothed +
    smoothingFactor * (observed - previousSmoothed)
```

Default:

```text
smoothing factor = 0.35
```

Lower values are smoother but lag more. Higher values react faster but transmit more jitter.

### Occlusion tolerance

If no candidate is associated:

- The pin remains allocated.
- The overlay reports `OCCLUDED`.
- Cursor movement is suspended.
- The previous identity is retained.

Default:

```text
maximum missed frames = 6
```

After the seventh consecutive miss, the pin is dropped.

### Cursor-follow controller

For a visible pin:

```text
error = smoothedTargetScreenPosition - aimReference
```

The aim reference defaults to **screen center**, which is where a game's
crosshair sits and therefore where its shot lands.

### Unit mismatch and the learned scale

The error above is measured in **screen pixels**, but `SendInput` accepts
**mouse counts**, and the conversion between them is the application's
sensitivity setting. Commanding `gain * error` counts therefore applies an
arbitrary and unknown loop gain.

Let `k` be the pixels of apparent target movement produced by one count. The
realized loop gain is `gain * k`. When that product exceeds 1 the loop
overshoots every frame; the step limiter then bounds the overshoot instead of
correcting it, and the controller settles into a limit cycle that circles the
target rather than converging on it. At a typical `k` of 3 to 12, the previous
default gain of 0.45 gave a realized gain of 1.35 to 5.4. This was the cause
of the reported orbiting, and no fixed gain can fix it, because `k` differs per
game, per sensitivity setting, and per user.

`AimController` estimates `k` online instead. Injecting `m` counts should
reduce the error by `k * m`, so each frame yields an observation:

```text
k_observed = (previousError - currentError) / injectedCounts
```

Observations are taken only when `|m|` is at least `minimumLearningCounts`,
are discarded when non-positive, since such a frame was dominated by target or
player motion rather than by the injection, and are folded into an exponential
moving average clamped to a plausible range. The command becomes:

```text
counts = gain * (error + lead * drift) / k_estimate
```

which is dimensionally correct and gives the same closed-loop behavior at any
sensitivity. A gain of 1.0 is deadbeat; the 0.65 default leaves margin for
estimate error.

### Velocity feedforward

`drift` is the part of the observed error change that the injection does not
explain:

```text
drift = currentError - (previousError - k_estimate * injectedCounts)
```

This is the target's own apparent motion. A proportional-only loop carries a
permanent trailing error against a target moving at constant speed; leading by
`drift` removes it. `--aim-lead 0` disables this and restores the trailing
behavior, which the test suite asserts is measurably worse.

### Aim point

The loop aims at the **raw** candidate centroid, not the smoothed one. The
exponential smoothing exists to keep the overlay readable; feeding its lag into
a feedback loop is itself a source of limit cycling. `--aim-smoothed` restores
the old behavior.

Using the operating-system cursor as the reference, as an earlier revision
did, is wrong for any application that reads raw input: the pointer is hidden
and parked, it does not track the view, and it does not respond to injected
relative movement. The controller then compares the target against a stale
point every frame and converges *around* the target rather than onto it.
`--aim-reference cursor` restores the old behavior for desktop demonstrations,
where the visible pointer really is the thing being steered.

Note that relative mouse counts are not screen pixels. The application
converts counts to view rotation through its own sensitivity setting, so
`--track-gain` has to be tuned per game and per sensitivity. Symptoms:
overshoot and oscillation mean the gain is too high; a slow crawl that never
arrives means it is too low.

If error magnitude is inside the dead zone, no movement is emitted.

Otherwise:

```text
movement = trackingGain * error
```

Movement magnitude is capped at the configured maximum step.

Defaults:

```text
gain         = 0.45
dead zone    = 3 pixels
maximum step = 35 pixels per frame
```

Releasing Mouse 5 immediately resets the tracker.

---

## 14. Overlay

The overlay is:

- Topmost.
- Layered.
- Color-key transparent.
- Non-activating.
- Tool-window styled.
- Mouse-click transparent.
- Excluded from capture when supported by Windows.

### Thread model

The detection worker never paints directly.

It publishes one `UiSnapshot` under a mutex and posts a custom window message. The UI thread then:

- Copies the snapshot.
- Draws the current state.
- Releases every selected GDI object correctly.

### Visual elements

- Blue rectangle: complete capture ROI.
- Orange rectangle: center trigger gate.
- Orange/green component box: raw or stable center-trigger candidate.
- Pink box: pinned target.
- Cyan line: cursor-to-smoothed-target relationship.
- Purple pin state: temporarily occluded target.

### Telemetry

The overlay displays:

- `RUN` or `PAUSED`.
- Trigger disabled, armed, or held.
- Tracking disabled, armed, or held.
- Raw center detection.
- Stable temporal state.
- Pin visible, occluded, or absent.
- Confidence.
- Component area.
- Candidate count.
- Total matched pixels.
- Processing milliseconds.
- Smoothed FPS.
- Estimated range when available.

---

## 15. Controls and supported key names

### Default controls

| Control | Function |
|---|---|
| Mouse 4 | Hold-to-trigger |
| Mouse 5 | Hold-to-pin and follow |
| `-` | Latching toggle for trigger mode |
| `=` | Latching toggle for pin/follow mode |
| F8 | Pause/resume complete detector |
| F9 | Exit detector cleanly |
| Escape in fixture | Close presentation fixture |

Each controller is active when **either** its hold key is down **or** its
toggle is latched on, so both input styles remain usable at once. The toggles
are edge-detected on key-down and are polled even while the detector is paused,
so their state never depends on when they were pressed.

Latching a toggle off resets the same state that releasing the hold key does:
the trigger toggle resets the temporal gate, and the tracking toggle resets the
tracker and drops the pin. Both toggles emit a confirmation beep, and the
overlay reports `TOGGLE` rather than `HELD` so a latched controller cannot be
mistaken for a held button.

Each toggle key must differ from its own hold key and from the other toggle
key; violating this is a startup error.

### Supported configurable activation keys

- F1 through F24.
- A through Z.
- 0 through 9.
- Space.
- Left or right Shift.
- Left or right Ctrl.
- Left or right Alt.
- Mouse 4 / `xbutton1`.
- Mouse 5 / `xbutton2`.
- `-` / `minus`.
- `=` / `equals` / `plus`.
- `[` / `lbracket`.
- `]` / `rbracket`.

Examples:

```powershell
--trigger-key f6
--track-key f7
--trigger-key mouse4
--track-key mouse5
--track-key space
```

---

## 16. Complete command-line reference

| Option | Purpose | Default |
|---|---|---:|
| `--trigger` | Enable hold-to-trigger controller | disabled |
| `--detector-only` | Observe and draw only; no key actuates anything | disabled |
| `--click` | Alias for `--trigger` | disabled |
| `--track` | Enable hold-to-pin/follow controller | disabled |
| `--trigger-key KEY` | Trigger hold key | Mouse 4 |
| `--track-key KEY` | Tracking hold key | Mouse 5 |
| `--trigger-toggle-key KEY` | Latching toggle for trigger mode | `-` |
| `--track-toggle-key KEY` | Latching toggle for tracking mode | `=` |
| `--aim-reference WHICH` | Aim from screen `center` or `cursor` | `center` |
| `--aim-scale VALUE` | Starting pixels-per-count estimate | `1.0` |
| `--no-aim-learning` | Keep the scale fixed instead of learning it | learning on |
| `--aim-lead VALUE` | Target-motion feedforward, 0 to 2 | `1.0` |
| `--aim-smoothed` | Aim at the smoothed centroid | raw centroid |
| `--soft-gates` | Penalize shape/profile failures instead of rejecting | disabled |
| `--click-hold-ms MS` | Left-button hold duration | `40` |
| `--track-gain VALUE` | Proportional cursor-follow gain | `0.45` |
| `--track-deadzone PX` | Movement dead-zone radius | `3` |
| `--track-max-step PX` | Maximum relative movement per frame | `35` |
| `--reacquire-radius PX` | Target identity association radius | `80` |
| `--occlusion-frames N` | Missing frames retained before dropping pin | `6` |
| `--pin-smoothing VALUE` | Target-position EMA factor | `0.35` |
| `--sound` | Queue a notification on trigger | disabled |
| `--diagnostics` | Log candidate counts and center transitions | disabled |
| `--no-overlay` | Keep overlay hidden | disabled |
| `--roi WIDTHxHEIGHT` | Center capture dimensions | `256x192` |
| `--gate PIXELS` | Center activation square size | `8` |
| `--fps FPS` | Target capture frequency | `120` |
| `--max-frames N` | Stop after a bounded number of frames | unlimited |
| `--target R,G,B` | Replace/add target prototypes | yellow |
| `--delta-e VALUE` | Maximum Lab color distance | `24` |
| `--min-saturation N` | Minimum saturation from 0 to 255 | `35` |
| `--min-area PIXELS` | Minimum connected-component area | `8` |
| `--min-gate PIXELS` | Minimum target pixels inside center gate | `2` |
| `--max-area-ratio VALUE` | Maximum component fraction of ROI | `0.25` |
| `--confidence VALUE` | Minimum final candidate confidence | `0.55` |
| `--confirm FRAMES` | Consecutive frames required to activate | `3` |
| `--release FRAMES` | Consecutive misses required to release | `3` |
| `--cooldown-ms MS` | Minimum interval between trigger events | `250` |
| `--known-height METERS` | Known real object height | disabled |
| `--vfov DEGREES` | Calibrated vertical camera FOV | disabled |
| `--keytest` | Report live key state and injection permissions, then exit | — |
| `--elevate` | Relaunch with an elevated token | disabled |
| `--thermal MODE` | Intensity segmentation: `white`, `black`, or `off` | `off` |
| `--thermal-threshold N` | Intensity a hot pixel must reach | `170` |
| `--thermal-contrast N` | Required excess over the local mean | `0` (off) |
| `--thermal-radius N` | Local-mean window radius | `16` |
| `--close-radius N` | Morphological closing radius | `0` (off) |
| `--merge-gap N` | Proximity merge distance for split components | `0` (off) |
| `--shape-gate` | Require human-like aspect and fill ratios | disabled |
| `--aspect MIN,MAX` | Bounding-box width/height range | `0.20,0.95` |
| `--fill MIN,MAX` | Area over box-area range | `0.20,0.92` |
| `--profile-gate` | Require a narrower top than middle | disabled |
| `--min-profile VALUE` | Minimum vertical profile score | `0.15` |
| `--persistence DECAY` | Cross-frame evidence decay, 0 to below 1 | disabled |
| `--persistence-threshold V` | Evidence required to retain a pixel | `0.45` |
| `--thermal-human` | Preset combining every silhouette stage | disabled |
| `--help` | Display executable help | — |

The first custom `--target` replaces the default. Later `--target` options append prototypes.

Example:

```powershell
.\build\color_detector.exe `
  --trigger `
  --track `
  --target 255,255,35 `
  --target 245,210,20 `
  --delta-e 16
```

---

## 17. Distance-estimation model

Thermal or display color does not uniquely encode metric distance. The program therefore does not infer distance from color intensity alone.

Distance uses a calibrated pinhole-camera model:

```text
focalLengthPixels =
    fullFrameHeightPixels /
    (2 * tan(verticalFovRadians / 2))

distance =
    knownObjectHeight *
    focalLengthPixels /
    observedBoundingBoxHeightPixels
```

Enable it with both:

```powershell
--known-height 1.80 --vfov 60
```

### Required conditions

- The real object height is known.
- The full object is visible.
- The component corresponds to that full object.
- Vertical FOV is correct for the current view.
- Zoom does not change without recalibration.

### Automatic validity guard

The system suppresses range output when the selected component touches an ROI boundary. A boundary-touching component may be clipped, making its apparent height invalid.

### Sources of error

- Pose.
- Perspective.
- Partial occlusion.
- Segmentation gaps.
- Object-height variation.
- FOV mismatch.
- Digital zoom.
- Display scaling.
- Post-processing.

When calibration is not controlled, the output should be described as a relative range proxy rather than exact metric distance.

---

## 18. University presentation fixture

The controlled fixture is built as:

```text
build\color_target_demo.exe
```

### Fixture behavior

- Borderless centered 960x640 window.
- Topmost to make screen capture deterministic.
- Dark nonmatching background.
- Visible screen-center crosshair.
- Three exact yellow targets.
- Target A moves horizontally through center.
- Targets B and C follow independent elliptical paths.
- Targets have different sizes.
- Clicks received by the fixture are counted on-screen.
- Escape closes the fixture.

### Timed mode

For automated testing, pass a number of seconds:

```powershell
.\build\color_target_demo.exe 7
```

The fixture closes itself after seven seconds.

### Presentation procedure

PowerShell window 1:

```powershell
.\build\color_target_demo.exe
```

PowerShell window 2:

```powershell
.\build\color_detector.exe `
  --trigger `
  --track `
  --delta-e 12 `
  --min-area 20
```

Then:

1. Bring the fixture to the foreground.
2. Show the blue ROI and orange center gate.
3. Hold Mouse 4 while target A crosses center.
4. Point out three-frame confirmation and the fixture click counter.
5. Hold Mouse 5.
6. Show nearest-target selection.
7. Show the pink pin box and cyan cursor relationship.
8. Observe smoothing while the selected target moves.
9. Discuss identity association and occlusion tolerance.
10. Press F8 to demonstrate global pause.
11. Press F8 again to resume.
12. Press F9 to close the detector.
13. Press Escape to close the fixture.

---

## 19. Build instructions

Requirements:

- Windows.
- CMake.
- Ninja.
- A C++20 compiler.

Validated compiler:

```text
GNU/MinGW C++ 14.2.0
```

Build:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Run tests:

```powershell
ctest --test-dir build --output-on-failure
```

Run the benchmark directly:

```powershell
.\build\detector_core_tests.exe
```

Produced artifacts:

```text
build\color_detector.exe
build\color_target_demo.exe
build\detector_core_tests.exe
```

---

## 20. Test coverage

### Color tests

- Identical Lab colors produce zero Delta E.
- Yellow and black remain strongly separated.

### Spatial tests

- A centered 4x4 yellow component is detected.
- Area is exactly 16.
- Bounding box is exactly 4x4.
- One isolated pixel is rejected.
- A valid off-center component is not center-trigger eligible.
- A component consuming most of the ROI is rejected.

### Temporal tests

- Frame one does not trigger.
- Frame two does not trigger.
- Frame three confirms and emits one event.
- Persistent evidence does not repeat.
- One miss does not release.
- Two configured misses release in the test configuration.
- A later rising edge can trigger again.

### Tracker tests

- The closest initial candidate is selected.
- Pin state is created.
- A later distractor does not automatically steal the pin when previous-position association favors the original track.
- Smoothing produces the expected intermediate coordinate.
- Temporary missing frames preserve the pin.
- Exceeding missed-frame tolerance drops the pin.
- Releasing activation immediately clears the pin.

### Aim controller tests

- The loop converges within 25 frames at an unknown sensitivity and stays on
  target, rather than orbiting it.
- The learned pixels-per-count scale approaches the simulated true value.
- One configuration converges at sensitivities spanning `0.15` to `12.0`
  pixels per count, which no fixed gain achieves.
- Velocity feedforward keeps a constantly moving target near center, and
  disabling it measurably increases trailing error.
- The dead zone suppresses movement and the step limit bounds a single frame.
- A zero gain is rejected.

### Soft-gate tests

- Hard gates drop a crouched silhouette; soft gates retain it without counting
  a rejection.
- A silhouette that passes the gates still outscores one that only survives
  them, so the penalty is real.

### Silhouette and thermal tests

- A banded silhouette fragments below the area floor without closing.
- Morphological closing reassembles it and preserves its full height.
- Proximity merging rejoins a target split by a wide occluder, reports the
  merge, preserves the fragment count, and sums the areas.
- Two people standing side by side are not merged.
- An upright silhouette passes both gates and scores above the profile floor.
- A wide solid block is rejected, and the rejection is attributed to a gate.
- Lab matching against yellow does not fire on a grayscale thermal frame.
- White-hot mode finds the hot silhouette with the exact expected area.
- Black-hot mode does not select a bright target.
- A plain intensity threshold accepts a uniformly bright frame.
- Local-contrast gating rejects that same frame.
- A half-exposed target does not clear the area floor in one frame.
- Cross-frame persistence reassembles it from alternating partial frames.
- Resetting persistence discards prior evidence.
- A decay of 1.0 and a mismatched buffer size are both rejected.

### Distance tests

- Valid geometry produces the expected approximate distance.
- Unknown or invalid height disables the estimate.

### CLI and lifecycle tests

- Help output exposes all current options.
- Invalid key names return exit code 1.
- Partial distance configuration returns exit code 1.
- Hidden bounded capture exits normally.
- Visible overlay bounded capture exits normally.
- F8/F9 hotkeys are registered when available.

---

## 21. Measured performance and live validation

### Core performance

Observed Release synthetic analysis averages during development:

- Approximately 0.465 ms before the RGB linearization lookup optimization.
- Approximately 0.324 ms after lookup optimization in one run.
- Approximately 0.489 ms in a later clean run.
- Approximately 0.330 ms in the final clean verification run.

After the silhouette stages were added, the same synthetic benchmark averages
approximately 0.57 to 0.74 ms across five consecutive runs. The increase over
the earlier figures comes from the component-labeling buffer widening from one
byte to a 32-bit label per pixel, which the profile scorer and the proximity
merger both require. The optional stages themselves are off in this benchmark.

Timing varies with the current CPU state, but remains well below the 8.33 ms budget for 120 FPS in the synthetic scene.

This measures core analysis, not full capture-to-actuation latency.

### Live desktop smoke tests

- 30-frame real capture completed.
- 60-frame distance-configuration capture completed.
- 120-frame default capture completed without a false event after tightening.
- Visible overlay created, rendered, and shut down cleanly.
- Hidden overlay mode shut down cleanly.
- No leftover detector or fixture processes remained after final verification.

### False-positive discovery and correction

During an early live smoke test, the original dark-red prototype merged a large portion of the current desktop:

```text
component area = 23,790 pixels
confidence     = 0.742
bogus range    = 17.54
```

This exposed an important real-world failure that synthetic exact-color testing did not reveal.

Corrections:

1. Removed dark red from the default prototype set.
2. Added maximum component-to-ROI fraction.
3. Set default maximum fraction to 0.25.
4. Suppressed range for boundary-clipped components.
5. Added an oversized-component regression test.

Repeated default and explicit dark-prototype smoke runs then produced no false event in the same desktop condition.

### Presentation fixture validation

Real screen capture against the running fixture observed:

```text
candidates     = 3
matched pixels = approximately 1,827 to 1,831
```

Diagnostics recorded repeated:

```text
center_entered
center_exited
```

as target A crossed the center gate.

Both timed processes exited with code 0.

### Final build state

- Clean Release build.
- No compiler warnings.
- CTest: 1/1 passed.
- Direct core tests: all passed.
- Final synthetic average: approximately 0.33 ms.
- `git diff --check`: no whitespace errors.

---

## 22. Calibration workflow

1. Start with detector-only mode.
2. Use `--diagnostics`.
3. Begin with a narrow Delta E, such as 10 or 12.
4. Capture representative positive and negative scenes.
5. Sample additional true-positive prototypes instead of only widening Delta E.
6. Increase minimum saturation to reject gray backgrounds.
7. Increase minimum area until isolated noise disappears.
8. Set maximum area ratio to reject background floods.
9. Set minimum gate overlap to require meaningful center intersection.
10. Tune confirmation and release on sequences, not screenshots.
11. Tune pin smoothing while observing target lag.
12. Tune tracking gain after smoothing.
13. Set a small dead zone to eliminate end-point jitter.
14. Cap maximum movement to avoid large discontinuities.
15. Set reacquisition radius based on maximum expected inter-frame motion.
16. Set occlusion frames based on expected interruption duration.
17. Measure:
    - Precision.
    - Recall.
    - False events per minute.
    - Missed detections.
    - Pin switches.
    - Reacquisition success.
    - Processing-time P50, P95, and P99.
18. Enable trigger or track output only after detector behavior is understood.

---

## 23. Diagnostics

Enable:

```powershell
.\build\color_detector.exe --diagnostics
```

Once per second it reports:

- Candidate count.
- Whether a center-gate candidate exists.
- Number of matching pixels.

It also reports transitions:

```text
diagnostics center_entered
diagnostics center_exited
```

Trigger logs include:

- Confidence.
- Component area.
- Estimated range when available.
- Whether click submission succeeded.

Pin logs include:

- Newly pinned state.
- Confidence.
- Component area.

---

## 24. Error handling and lifecycle

The implementation checks or validates:

- Missing CLI values.
- Invalid integer and floating-point syntax.
- Invalid ROI dimensions.
- Invalid RGB channels.
- Unsupported activation keys.
- Gate larger than ROI.
- Invalid FPS.
- Invalid confidence.
- Invalid area fraction.
- Invalid tracking gain.
- Invalid dead zone.
- Invalid maximum step.
- Invalid smoothing.
- Invalid reacquisition radius.
- Negative occlusion tolerance.
- Partial distance configuration.
- Screen geometry.
- `GetDC`.
- `CreateCompatibleDC`.
- `CreateDIBSection`.
- `SelectObject`.
- `BitBlt`.
- Overlay class registration.
- Overlay creation.
- Layered-window setup.
- `SendInput` return counts.

### Clean shutdown

- F9 destroys the overlay/control window.
- `WM_DESTROY` sets the shared stop flag.
- The message loop ends.
- The detection worker notices the stop flag.
- The main thread joins the worker.
- Capture GDI objects are restored and released.

F8 pauses:

- Temporal trigger state is reset.
- Pin state is reset.
- No capture analysis or movement is performed until resumed.

---

## 25. Known limitations

### Capture

- Uses Desktop Duplication when available and falls back to GDI `BitBlt`.
- Windows Graphics Capture is not implemented.
- Captures the primary screen only.
- Detection cannot outrun the display: roughly 63 distinct frames per second
  were observed, so a higher `--fps` re-analyzes frames rather than seeing new
  ones.
- Display changes during execution do not recreate the capture surface.
- `SetProcessDPIAware` is used, but multi-monitor per-monitor-DPI behavior is not implemented.

### Classification

- Uses Delta E 76 rather than Delta E 2000.
- No automatic white balance or illumination adaptation.
- No learned classifier.
- Similar-colored UI elements can still become candidates.

### Components

- Overlapping same-color targets may merge.
- Morphological closing and proximity merging are available but off by default.
- Closing bridges gaps only up to its structuring-element size; wider occluders
  need proximity merging, and merging assumes vertical alignment.
- The shape and profile gates assume an upright target. A prone, crouched, or
  heavily foreshortened person will be rejected by them.
- The profile gate needs at least four pixels of component height.
- Persistence assumes a slow-moving target: a fast mover smears its evidence
  across the accumulator, so decay must be lowered as target speed rises.

### Tracking

- Identity uses nearest previous centroid, not appearance features.
- No Kalman filter.
- No explicit velocity prediction.
- No Hungarian multi-object assignment.
- Crossing targets can still cause identity swaps.
- The selection origin is ROI center.

### Cursor control

- Uses relative `SendInput`.
- Windows pointer acceleration and device settings can influence resulting motion.
- Relative counts are not screen pixels; the gain must be retuned whenever the
  application's sensitivity changes.
- The controller is proportional only. It has no velocity feedforward, so it
  lags a target moving at constant speed by a fixed error.
- The aim reference is fixed at screen center or the cursor; a crosshair that
  is not centered is not modeled.

### Distance

- Requires known height and calibrated FOV.
- Assumes a complete segmentation.
- Cannot recover metric depth from thermal color alone.
- Is suppressed only for ROI-edge clipping; other partial occlusions may still bias it.

### Overlay

- Capture exclusion depends on Windows support.
- Color-key transparency can leave antialiasing fringes around text or shapes.

### Automated interaction verification

The environment allowed:

- Core tracker tests.
- Real fixture capture.
- Candidate extraction.
- Center-enter/exit diagnostics.
- Process lifecycle tests.

Synthetic F6/F7 or Mouse 4/Mouse 5 input injection from the external test shell was blocked by desktop execution policy before running. The physical hold-button interaction therefore remains a manual presentation check.

---

## 25b. Thermal-silhouette detection stages

The color pipeline answers "which pixels look like this color". A thermal
silhouette seen through foliage, a railing, or a gap in a wall poses two
different problems: intensity rather than hue carries the signal, and the
target arrives as fragments rather than as one region. Six optional stages
address this. All default to off, so baseline behavior and every existing
threshold are unchanged.

### Intensity segmentation

`--thermal white|black` replaces Lab prototype matching with a luminance
threshold. Thermal palettes encode temperature as brightness, so Delta E to a
color prototype is the wrong classifier for white-hot and black-hot imagery.
Ironbow is the one palette where the existing color path remains appropriate.

`--thermal-contrast N` additionally requires each pixel to exceed the mean of
its local neighborhood by `N`. This is what separates a warm body from a
uniformly bright sky or a sunlit wall, and it is computed from a summed-area
table, so the cost does not grow with `--thermal-radius`.

### Morphological closing

`--close-radius N` dilates then erodes the mask with a square structuring
element. Gaps narrower than the element close, while the outer silhouette
keeps its dimensions. This is the direct fix for branches, wire, and window
mullions. The operation is separable, so it costs two linear passes.

Pixels introduced by closing carry no color evidence and are therefore
excluded from the mean Delta E, so bridging never inflates the color score.

### Proximity merging

`--merge-gap N` unions components whose bounding boxes come within `N` pixels
using a disjoint-set structure. Merging additionally requires genuine column
overlap of at least a quarter of the narrower component, so a person split by
a wide occluder rejoins vertically while two people standing side by side stay
distinct. `Candidate::fragments` reports how many pieces were reassembled.

### Shape gate

`--shape-gate` rejects components outside a human aspect ratio and fill ratio.
An upright person occupies roughly 0.3 to 0.55 width over height and fills
about half of their bounding box; a vehicle, vent, or sunlit wall is far wider
or far more solid. Both quantities were already being computed.

### Vertical projection profile

`--profile-gate` scores how much wider the shoulders-to-hips band is than the
head band, measuring the top eighth against the middle third of the bounding
box. An upright human scores high; a rectangle or a round blob scores near
zero. It survives partial occlusion better than whole-silhouette matching.

### Cross-frame persistence

`--persistence DECAY` accumulates per-pixel evidence with exponential decay and
folds the accumulated mask back into segmentation. A target behind shifting
foliage exposes a different fraction of itself each frame; no single frame
contains the whole silhouette, but the decayed union across a short window
does. This is the principled answer to detection through small holes. The
accumulator is cleared on pause and on resume.

### Preset

`--thermal-human` enables white-hot segmentation, local contrast 25, closing
radius 2, merge gap 6, both silhouette gates, a 24-pixel area floor, and
persistence at 0.80 decay.

### Ordering

```text
intensity or color segmentation
    -> cross-frame persistence
    -> morphological closing
    -> connected components
    -> proximity merging
    -> shape gate
    -> profile gate
    -> confidence
```

When either silhouette gate is enabled, confidence becomes
`0.80 * baseConfidence + 0.20 * profileScore`. With both gates off the original
four-term model applies unchanged.

---

## 25c. Input troubleshooting on protected applications

Hold-to-trigger and hold-to-pin depend on two operating-system services that
both fail silently when the target application runs at a higher integrity
level than the detector.

### Cause

Windows User Interface Privilege Isolation prevents a medium-integrity process
from reading key state while a higher-integrity window holds the foreground,
and from injecting input into it. `GetAsyncKeyState` therefore reports the hold
key as never pressed, and `SendInput` returns success while the event is
discarded. Neither reports an error, so the symptom is simply that the keybind
appears to do nothing.

### Resolution

Run the detector at the same integrity level as the target:

```powershell
.\build\color_detector.exe --elevate --trigger --track
```

`--elevate` relaunches through the consent prompt, preserving all other
arguments.

### Diagnosis

```powershell
.\build\color_detector.exe --keytest
```

This injects nothing. It prints live state for the trigger key, the tracking
key, F8, F9, and the raw mouse buttons, alongside the process elevation state,
and warns when the foreground window belongs to a process it cannot query. If
the hold key reads `1` here but nothing happens in game, the fault lies in
capture or detection rather than in the keybind.

### Other causes addressed

- **Exclusive fullscreen.** GDI `BitBlt` cannot read an exclusive swap chain
  and returns black. The detector now warns after two seconds of an empty ROI
  and recommends borderless windowed mode.
- **Hotkey conflicts.** F8 and F9 are frequently claimed by overlay software.
  Failed registration now falls back to polled edge detection, so pause and
  exit work regardless.
- **Remapped mouse buttons.** Vendor driver software can consume Mouse 4 and
  Mouse 5 before they reach `VK_XBUTTON1`/`VK_XBUTTON2`. `--keytest` makes this
  visible immediately, and `--trigger-key`/`--track-key` provide alternatives.

---

## 26. Future engineering extensions

Reasonable next extensions include:

1. Desktop Duplication capture backend.
2. Foreground-window or named-window capture.
3. Multi-monitor ROI selection.
4. Delta E 2000.
5. Morphological open/close filters.
6. Component shape descriptors.
7. Appearance signatures per pinned target.
8. Alpha-beta or Kalman motion prediction.
9. Multi-target assignment using the Hungarian algorithm.
10. Velocity and acceleration telemetry.
11. Track-history trails.
12. Target-switch statistics.
13. Recorded-frame replay mode.
14. CSV telemetry export.
15. Ground-truth annotation format.
16. Precision/recall report generation.
17. FOV calibration assistant.
18. Relative near/medium/far range classification.
19. Config-file persistence.
20. On-screen controls for live threshold tuning.

---

## 27. Recommended presentation narrative

### Part 1: Motivation

Explain that a single-pixel threshold is fast but unreliable. Show why color variation, noise, UI elements, and one-frame anomalies produce false events.

### Part 2: Perception

Explain:

- Perceptual color space.
- Saturation rejection.
- Connected components.
- Spatial confidence.

### Part 3: Temporal reasoning

Explain:

- Confirmation.
- Release.
- Hysteresis.
- Rising-edge events.

### Part 4: Trigger control

Hold Mouse 4 and demonstrate that an event occurs only when target A crosses the gate and remains confirmed.

### Part 5: Pinning and identity

Hold Mouse 5 and explain:

- Closest-target initialization.
- Previous-position association.
- Smoothing.
- Reacquisition.
- Occlusion tolerance.

### Part 6: Control theory

Explain:

- Position error.
- Proportional gain.
- Dead zone.
- Step saturation.
- Stability versus responsiveness.

### Part 7: Range

Present the pinhole model and explicitly separate:

- Color-based segmentation.
- Geometry-based range estimation.

### Part 8: Validation

Show:

- Unit tests.
- Candidate diagnostics.
- Center transitions.
- Processing time.
- False-positive correction discovered through live testing.

### Part 9: Limitations

Discuss centroid association, target crossing, FOV calibration, and why a Kalman/Hungarian extension would be the next research step.

---

## 28. Quick-start commands

### Build

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

### Detector only

```powershell
.\build\color_detector.exe
```

### Trigger only

```powershell
.\build\color_detector.exe --trigger
```

Hold Mouse 4.

### Pin/track only

```powershell
.\build\color_detector.exe --track
```

Hold Mouse 5.

### Both

```powershell
.\build\color_detector.exe --trigger --track
```

### Complete presentation

Window 1:

```powershell
.\build\color_target_demo.exe
```

Window 2:

```powershell
.\build\color_detector.exe `
  --trigger `
  --track `
  --delta-e 12 `
  --min-area 20 `
  --diagnostics
```

### Calibrated range

```powershell
.\build\color_detector.exe `
  --known-height 1.80 `
  --vfov 60
```

### Bounded smoke test

```powershell
.\build\color_detector.exe `
  --no-overlay `
  --max-frames 120 `
  --diagnostics
```

---

## 29. Completion checklist

- [x] Verified current upstream main branch.
- [x] Cloned repository into the empty workspace.
- [x] Preserved historical overlay attempt.
- [x] Replaced active prototype implementation.
- [x] Added modular detector core.
- [x] Added one-block ROI capture.
- [x] Added dynamic screen geometry.
- [x] Added perceptual Lab classification.
- [x] Added saturation rejection.
- [x] Added multiple color prototypes.
- [x] Added connected components.
- [x] Added candidate bounding boxes.
- [x] Added centroids.
- [x] Added confidence.
- [x] Added oversized-background rejection.
- [x] Added center-gate logic.
- [x] Added temporal confirmation.
- [x] Added temporal release.
- [x] Added cooldown.
- [x] Added rising-edge trigger.
- [x] Added Mouse 4 hold activation.
- [x] Added nearest-target selection.
- [x] Added persistent pin state.
- [x] Added previous-centroid identity association.
- [x] Added target smoothing.
- [x] Added Mouse 5 hold activation.
- [x] Added cursor-follow controller.
- [x] Added dead zone.
- [x] Added maximum movement step.
- [x] Added occlusion retention.
- [x] Added reacquisition radius.
- [x] Added overlay ROI.
- [x] Added overlay gate.
- [x] Added trigger candidate visualization.
- [x] Added pinned target visualization.
- [x] Added tracking line.
- [x] Added overlay telemetry.
- [x] Added global pause.
- [x] Added clean exit.
- [x] Added diagnostics.
- [x] Added calibrated range estimate.
- [x] Added clipping guard.
- [x] Added presentation fixture.
- [x] Added click counter.
- [x] Added timed fixture mode.
- [x] Added CMake.
- [x] Added tests.
- [x] Added benchmark.
- [x] Added README.
- [x] Performed clean warning-free Release build.
- [x] Passed all unit tests.
- [x] Performed real capture smoke tests.
- [x] Discovered and corrected live false positive.
- [x] Verified three real fixture candidates.
- [x] Verified center-enter/exit transitions.
- [x] Verified clean process shutdown.
- [x] Added thermal intensity segmentation with local-contrast gating.
- [x] Added morphological closing.
- [x] Added proximity merging with side-by-side protection.
- [x] Added shape gate.
- [x] Added vertical projection profile gate.
- [x] Added cross-frame persistence accumulator.
- [x] Added the thermal-human preset.
- [x] Added silhouette overlay telemetry.
- [x] Added fourteen silhouette, thermal, and persistence tests.
- [x] Added key self-test and elevation relaunch.
- [x] Added polled F8/F9 fallback.
- [x] Added latching trigger and tracking toggles alongside the hold keys.
- [x] Added blank-capture warning.
- [ ] Manually demonstrate physical Mouse 4 trigger hold.
- [ ] Manually demonstrate physical Mouse 5 tracking hold.
- [ ] Validate the thermal preset against real thermal footage.
- [ ] Commit changes.
- [ ] Push changes to GitHub.

---

## 30. Final project status

The engineering proof of concept is implemented, compiled, documented, and validated locally.

The current defaults are:

```text
Trigger hold:   Mouse 4
Track hold:     Mouse 5
Trigger toggle: -
Track toggle:   =
Pause:        F8
Exit:         F9
ROI:          256x192
Gate:         8x8
Target FPS:   120
Target color: RGB(255,255,35)
Delta E:      24
Confirm:      3 frames
Release:      3 frames
Track gain:   0.45
Dead zone:    3 pixels
Max step:     35 pixels/frame
Smoothing:    0.35
Reacquire:    80 pixels
Occlusion:    6 frames
```

The source changes and new files remain uncommitted and unpushed. The build outputs are present in the local `build` directory.
