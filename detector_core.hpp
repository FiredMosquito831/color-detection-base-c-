#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace colorbot {

struct Rgb {
    std::uint8_t r{};
    std::uint8_t g{};
    std::uint8_t b{};
};

struct Lab {
    double l{};
    double a{};
    double b{};
};

struct ColorPrototype {
    std::string name;
    Rgb rgb;
    Lab lab;
};

struct BoundingBox {
    int left{};
    int top{};
    int right{};
    int bottom{};

    [[nodiscard]] int width() const { return right - left + 1; }
    [[nodiscard]] int height() const { return bottom - top + 1; }
};

struct Candidate {
    BoundingBox box;
    int area{};
    int gateHits{};
    double centroidX{};
    double centroidY{};
    double meanDeltaE{};
    double confidence{};
    // Width divided by height of the bounding box. Upright humans sit near
    // 0.3-0.55; vehicles, vents and sunlit walls are far wider.
    double aspectRatio{};
    // Area divided by bounding-box area. A silhouette fills roughly half its
    // box; a solid hot rectangle approaches 1.0.
    double fillRatio{};
    // Head-over-torso vertical projection score from 0 to 1. See
    // verticalProfileScore in the implementation.
    double profileScore{};
    // Number of separately labeled pieces merged into this candidate. Greater
    // than one means the target was reassembled across an occluder.
    int fragments{1};
};

// Thermal palettes encode temperature as intensity, not hue, so Lab distance
// to a color prototype is the wrong classifier for them. These modes segment
// on luminance and local contrast instead.
enum class ThermalMode {
    Off,       // Lab color-prototype matching (default).
    WhiteHot,  // Hot targets are bright.
    BlackHot,  // Hot targets are dark.
};

struct DetectionConfig {
    double maxDeltaE{24.0};
    int minimumSaturation{35};
    int minimumComponentPixels{8};
    int minimumGatePixels{2};
    int triggerGateSize{8};
    double minimumConfidence{0.55};
    double maximumComponentFraction{0.25};

    // Thermal segmentation. When not Off, the color prototypes and the
    // saturation filter are bypassed.
    ThermalMode thermalMode{ThermalMode::Off};
    int thermalThreshold{170};
    // Require the pixel to exceed its local neighborhood mean by this much.
    // Rejects large uniformly bright regions such as sky or sunlit walls while
    // keeping small hot silhouettes. Zero disables the test.
    int thermalLocalContrast{0};
    int thermalLocalRadius{16};

    // Morphological closing radius applied to the mask before labeling.
    // Bridges thin occluders such as branches, wire and window mullions so a
    // fragmented silhouette becomes one component. Zero disables it.
    int morphologyRadius{0};

    // After labeling, merge components whose bounding boxes come within this
    // many pixels and overlap horizontally. Recovers a target split by an
    // occluder too wide for morphological closing. Zero disables it.
    int mergeGapPixels{0};

    // Human-silhouette shape gate. Applied only when enabled.
    bool shapeGateEnabled{false};
    double minimumAspectRatio{0.20};
    double maximumAspectRatio{0.95};
    double minimumFillRatio{0.20};
    double maximumFillRatio{0.92};

    // Vertical projection profile gate. Requires a narrower top than middle,
    // which distinguishes an upright human from a blob or a rectangle.
    bool profileGateEnabled{false};
    double minimumProfileScore{0.15};
};

struct DetectionResult {
    bool detected{};
    int matchedPixels{};
    int acceptedComponents{};
    // Components discarded by the shape or profile gate. Useful when tuning:
    // a high count with no candidates means the gate is too tight.
    int shapeRejections{};
    int profileRejections{};
    // Components that morphological closing or proximity merging reassembled.
    int mergedComponents{};
    std::vector<Candidate> candidates;
    std::optional<Candidate> best;
};

// Accumulates per-pixel detection evidence across frames with exponential
// decay. A target seen through foliage exposes a different fraction of itself
// each frame; no single frame contains the whole silhouette, but the union
// over a short window does. Feeding the accumulated mask back into
// segmentation therefore recovers targets that are never fully visible.
class PersistenceAccumulator {
public:
    PersistenceAccumulator(int width, int height, double decay, double gain);

    // Decays the stored evidence, adds the current mask, and writes back the
    // union of the current mask and every pixel whose evidence exceeds
    // threshold. mask must hold width * height bytes of 0 or 1.
    void apply(std::uint8_t* mask, double threshold);
    void reset();

    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int height() const { return height_; }

private:
    int width_{};
    int height_{};
    double decay_{};
    double gain_{};
    std::vector<double> evidence_;
};

[[nodiscard]] Lab rgbToLab(Rgb rgb);
[[nodiscard]] double deltaE76(const Lab& first, const Lab& second);
[[nodiscard]] ColorPrototype makePrototype(std::string name, Rgb rgb);

// The input is a top-down BGRA image. strideBytes may be larger than width * 4.
// When persistence is supplied it is updated with this frame's mask and its
// accumulated evidence is folded back in before components are extracted; its
// dimensions must match width and height.
[[nodiscard]] DetectionResult analyzeBgra(
    const std::uint8_t* pixels,
    int width,
    int height,
    int strideBytes,
    const std::vector<ColorPrototype>& prototypes,
    const DetectionConfig& config,
    PersistenceAccumulator* persistence = nullptr,
    double persistenceThreshold = 0.45);

// Pinhole-camera estimate. It is meaningful only when the complete known-size
// object is visible and the vertical FOV is calibrated for the current view.
[[nodiscard]] std::optional<double> estimateMonocularDistance(
    double knownObjectHeight,
    double verticalFovDegrees,
    int fullFrameHeightPixels,
    int observedObjectHeightPixels);

struct TemporalConfig {
    int confirmationFrames{3};
    int releaseFrames{3};
    std::chrono::milliseconds retriggerCooldown{250};
};

struct TemporalDecision {
    bool active{};
    bool triggerEvent{};
    int hitStreak{};
    int missStreak{};
};

class TemporalGate {
public:
    explicit TemporalGate(TemporalConfig config);

    [[nodiscard]] TemporalDecision update(
        bool rawDetection,
        std::chrono::steady_clock::time_point timestamp);
    void reset();

private:
    TemporalConfig config_;
    bool active_{};
    int hitStreak_{};
    int missStreak_{};
    std::optional<std::chrono::steady_clock::time_point> lastTrigger_;
};

struct TrackerConfig {
    int maximumMissedFrames{6};
    double reacquireRadiusPixels{80.0};
    double smoothingFactor{0.35};
    double confidencePenaltyPixels{30.0};
};

struct TrackState {
    bool pinned{};
    bool visible{};
    bool newlyPinned{};
    int missedFrames{};
    double smoothedX{};
    double smoothedY{};
    Candidate candidate;
};

// Maintains target identity by associating each frame with the candidate nearest
// the previously observed centroid. A new pin starts with the candidate closest
// to the supplied selection origin, adjusted by confidence.
class TargetTracker {
public:
    explicit TargetTracker(TrackerConfig config);

    [[nodiscard]] TrackState update(
        const std::vector<Candidate>& candidates,
        bool activationHeld,
        double selectionOriginX,
        double selectionOriginY);
    void reset();

private:
    TrackerConfig config_;
    bool pinned_{};
    int missedFrames_{};
    double lastObservedX_{};
    double lastObservedY_{};
    double smoothedX_{};
    double smoothedY_{};
    Candidate lastCandidate_;
};

}  // namespace colorbot
