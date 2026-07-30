#include "detector_core.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void setPixel(std::vector<std::uint8_t>& bgra, int width, int x, int y, colorbot::Rgb rgb) {
    const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 4;
    bgra[index] = rgb.b;
    bgra[index + 1] = rgb.g;
    bgra[index + 2] = rgb.r;
    bgra[index + 3] = 255;
}

colorbot::DetectionConfig testConfig() {
    colorbot::DetectionConfig config;
    config.maxDeltaE = 10.0;
    config.minimumSaturation = 20;
    config.minimumComponentPixels = 5;
    config.minimumGatePixels = 3;
    config.triggerGateSize = 8;
    config.minimumConfidence = 0.50;
    return config;
}

void testColorConversion() {
    const colorbot::Rgb yellow{255, 255, 35};
    const auto lab = colorbot::rgbToLab(yellow);
    require(colorbot::deltaE76(lab, lab) < 1e-12, "Identical colors must have zero Delta E");
    const auto black = colorbot::rgbToLab(colorbot::Rgb{0, 0, 0});
    require(colorbot::deltaE76(lab, black) > 50.0, "Yellow and black must be well separated");
}

void testConnectedComponentDetection() {
    constexpr int width = 32;
    constexpr int height = 32;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4, 0);
    const colorbot::Rgb yellow{255, 255, 35};
    for (int y = 14; y <= 17; ++y) {
        for (int x = 14; x <= 17; ++x) {
            setPixel(pixels, width, x, y, yellow);
        }
    }

    const std::vector targets{colorbot::makePrototype("yellow", yellow)};
    const auto result = colorbot::analyzeBgra(
        pixels.data(), width, height, width * 4, targets, testConfig());
    require(result.detected, "A centered 4x4 target component should be detected");
    require(result.best.has_value(), "Detected result must contain a candidate");
    require(result.best->area == 16, "Connected-component area should be exact");
    require(result.best->box.width() == 4 && result.best->box.height() == 4, "Bounding box should be exact");
}

void testNoiseAndGateRejection() {
    constexpr int width = 32;
    constexpr int height = 32;
    const colorbot::Rgb yellow{255, 255, 35};
    const std::vector targets{colorbot::makePrototype("yellow", yellow)};

    std::vector<std::uint8_t> noise(static_cast<std::size_t>(width) * height * 4, 0);
    setPixel(noise, width, 16, 16, yellow);
    const auto noiseResult = colorbot::analyzeBgra(
        noise.data(), width, height, width * 4, targets, testConfig());
    require(!noiseResult.detected, "One matching pixel must not trigger detection");

    std::vector<std::uint8_t> offCenter(static_cast<std::size_t>(width) * height * 4, 0);
    for (int y = 2; y < 6; ++y) {
        for (int x = 2; x < 6; ++x) {
            setPixel(offCenter, width, x, y, yellow);
        }
    }
    const auto offCenterResult = colorbot::analyzeBgra(
        offCenter.data(), width, height, width * 4, targets, testConfig());
    require(!offCenterResult.detected, "A component outside the activation gate must not trigger");

    std::vector<std::uint8_t> oversized(static_cast<std::size_t>(width) * height * 4, 0);
    for (int y = 4; y < 28; ++y) {
        for (int x = 4; x < 28; ++x) {
            setPixel(oversized, width, x, y, yellow);
        }
    }
    const auto oversizedResult = colorbot::analyzeBgra(
        oversized.data(), width, height, width * 4, targets, testConfig());
    require(!oversizedResult.detected, "A component consuming most of the ROI must be rejected");
}

void testTemporalGate() {
    colorbot::TemporalConfig config;
    config.confirmationFrames = 3;
    config.releaseFrames = 2;
    config.retriggerCooldown = 100ms;
    colorbot::TemporalGate gate(config);
    const auto start = std::chrono::steady_clock::time_point{};

    require(!gate.update(true, start).triggerEvent, "Frame one must not trigger");
    require(!gate.update(true, start + 10ms).triggerEvent, "Frame two must not trigger");
    const auto confirmed = gate.update(true, start + 20ms);
    require(confirmed.active && confirmed.triggerEvent, "Frame three must confirm and trigger once");
    require(!gate.update(true, start + 30ms).triggerEvent, "Persistent evidence must not retrigger");
    require(gate.update(false, start + 40ms).active, "One miss must not release");
    require(!gate.update(false, start + 50ms).active, "Two misses must release");

    (void)gate.update(true, start + 150ms);
    (void)gate.update(true, start + 160ms);
    require(gate.update(true, start + 170ms).triggerEvent, "A later rising edge should retrigger");
}

colorbot::Candidate candidateAt(double x, double y, double confidence = 0.9) {
    colorbot::Candidate candidate;
    candidate.box = colorbot::BoundingBox{
        static_cast<int>(x) - 2,
        static_cast<int>(y) - 2,
        static_cast<int>(x) + 2,
        static_cast<int>(y) + 2};
    candidate.area = 25;
    candidate.centroidX = x;
    candidate.centroidY = y;
    candidate.confidence = confidence;
    return candidate;
}

void testTargetTracker() {
    colorbot::TrackerConfig config;
    config.maximumMissedFrames = 2;
    config.reacquireRadiusPixels = 30.0;
    config.smoothingFactor = 0.5;
    config.confidencePenaltyPixels = 10.0;
    colorbot::TargetTracker tracker(config);

    const std::vector firstFrame{
        candidateAt(10.0, 10.0),
        candidateAt(52.0, 50.0),
        candidateAt(90.0, 90.0)};
    const auto initial = tracker.update(firstFrame, true, 50.0, 50.0);
    require(initial.pinned && initial.visible && initial.newlyPinned, "Tracker should pin a visible target");
    require(std::abs(initial.smoothedX - 52.0) < 0.01, "Tracker should choose the closest candidate");

    // A different candidate becomes closer to the selection origin, but identity
    // association must stay with the candidate near the previous observation.
    const std::vector secondFrame{
        candidateAt(40.0, 50.0),
        candidateAt(63.0, 50.0)};
    const auto maintained = tracker.update(secondFrame, true, 50.0, 50.0);
    require(maintained.pinned && maintained.visible && !maintained.newlyPinned, "Pin should persist");
    require(maintained.candidate.centroidX == 63.0, "Tracker should associate by previous position");
    require(std::abs(maintained.smoothedX - 57.5) < 0.01, "Tracker should smooth target motion");

    require(!tracker.update({}, true, 50.0, 50.0).visible, "One missing frame should preserve an occluded pin");
    require(tracker.update({}, true, 50.0, 50.0).pinned, "Configured miss tolerance should preserve identity");
    require(!tracker.update({}, true, 50.0, 50.0).pinned, "Pin should drop after miss tolerance");

    require(!tracker.update(firstFrame, false, 50.0, 50.0).pinned, "Releasing activation must clear the pin");
}

void testDistanceEstimate() {
    const auto distance = colorbot::estimateMonocularDistance(1.8, 60.0, 1080, 180);
    require(distance.has_value(), "Valid calibration should produce a distance");
    require(*distance > 9.0 && *distance < 9.5, "Pinhole estimate should match the calibrated geometry");
    require(!colorbot::estimateMonocularDistance(0.0, 60.0, 1080, 180), "Unknown height must disable distance");
}

void runSyntheticBenchmark() {
    constexpr int width = 256;
    constexpr int height = 192;
    constexpr int iterations = 30;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4, 0);
    const colorbot::Rgb yellow{255, 255, 35};
    for (int y = height / 2 - 8; y < height / 2 + 8; ++y) {
        for (int x = width / 2 - 8; x < width / 2 + 8; ++x) {
            setPixel(pixels, width, x, y, yellow);
        }
    }
    const std::vector targets{colorbot::makePrototype("yellow", yellow)};
    auto config = testConfig();
    const auto start = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const auto result = colorbot::analyzeBgra(
            pixels.data(), width, height, width * 4, targets, config);
        require(result.detected, "Synthetic benchmark target disappeared");
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << "Synthetic 256x192 analysis average: " << elapsed / iterations << " ms\n";
}

// Draws a crude upright human silhouette: a narrow head over a wider torso.
void drawSilhouette(
    std::vector<std::uint8_t>& pixels,
    int width,
    int left,
    int top,
    colorbot::Rgb color) {
    for (int y = 0; y < 4; ++y) {
        for (int x = 3; x < 7; ++x) {
            setPixel(pixels, width, left + x, top + y, color);
        }
    }
    for (int y = 4; y < 20; ++y) {
        for (int x = 0; x < 10; ++x) {
            setPixel(pixels, width, left + x, top + y, color);
        }
    }
}

// Erases horizontal bands to imitate a target seen through branches or a
// railing: the silhouette survives, but only as disconnected slices.
void eraseHorizontalBands(
    std::vector<std::uint8_t>& pixels,
    int width,
    int left,
    int top,
    int bandSpacing) {
    for (int y = top; y < top + 20; ++y) {
        if ((y - top) % bandSpacing != 0) {
            continue;
        }
        for (int x = left; x < left + 10; ++x) {
            setPixel(pixels, width, x, y, colorbot::Rgb{0, 0, 0});
        }
    }
}

void testMorphologicalClosingBridgesOccluders() {
    constexpr int width = 48;
    constexpr int height = 48;
    const colorbot::Rgb yellow{255, 255, 35};
    const std::vector targets{colorbot::makePrototype("yellow", yellow)};

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4, 0);
    drawSilhouette(pixels, width, 19, 14, yellow);
    eraseHorizontalBands(pixels, width, 19, 14, 3);

    auto config = testConfig();
    config.minimumComponentPixels = 40;

    const auto fragmented = colorbot::analyzeBgra(
        pixels.data(), width, height, width * 4, targets, config);
    require(
        fragmented.candidates.empty(),
        "Without closing, an occluded silhouette should fragment below the area floor");

    config.morphologyRadius = 1;
    const auto closed = colorbot::analyzeBgra(
        pixels.data(), width, height, width * 4, targets, config);
    require(
        !closed.candidates.empty(),
        "Morphological closing should reassemble a banded silhouette");
    require(
        closed.candidates.front().box.height() >= 18,
        "The reassembled component should span the full silhouette height");
}

void testProximityMergingRecoversSplitTarget() {
    constexpr int width = 48;
    constexpr int height = 48;
    const colorbot::Rgb yellow{255, 255, 35};
    const std::vector targets{colorbot::makePrototype("yellow", yellow)};

    // Two vertically stacked blocks separated by a 4-pixel gap: one target
    // behind a wide occluder, too wide for a small closing radius.
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4, 0);
    for (int y = 10; y < 20; ++y) {
        for (int x = 20; x < 30; ++x) {
            setPixel(pixels, width, x, y, yellow);
        }
    }
    for (int y = 24; y < 34; ++y) {
        for (int x = 20; x < 30; ++x) {
            setPixel(pixels, width, x, y, yellow);
        }
    }

    auto config = testConfig();
    config.minimumComponentPixels = 5;

    const auto split = colorbot::analyzeBgra(
        pixels.data(), width, height, width * 4, targets, config);
    require(split.acceptedComponents == 2, "Without merging the target stays split in two");

    config.mergeGapPixels = 6;
    const auto merged = colorbot::analyzeBgra(
        pixels.data(), width, height, width * 4, targets, config);
    require(merged.acceptedComponents == 1, "Proximity merging should rejoin the two halves");
    require(merged.mergedComponents == 1, "The merge should be reported");
    require(merged.candidates.front().fragments == 2, "Fragment count should be preserved");
    require(merged.candidates.front().area == 200, "Merged area is the sum of both halves");
}

void testProximityMergingKeepsSideBySideTargetsApart() {
    constexpr int width = 48;
    constexpr int height = 48;
    const colorbot::Rgb yellow{255, 255, 35};
    const std::vector targets{colorbot::makePrototype("yellow", yellow)};

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4, 0);
    drawSilhouette(pixels, width, 12, 14, yellow);
    drawSilhouette(pixels, width, 26, 14, yellow);

    auto config = testConfig();
    config.mergeGapPixels = 6;
    const auto result = colorbot::analyzeBgra(
        pixels.data(), width, height, width * 4, targets, config);
    require(
        result.acceptedComponents == 2,
        "Two people standing side by side must not be merged into one target");
}

void testShapeAndProfileGates() {
    constexpr int width = 48;
    constexpr int height = 48;
    const colorbot::Rgb yellow{255, 255, 35};
    const std::vector targets{colorbot::makePrototype("yellow", yellow)};

    std::vector<std::uint8_t> human(static_cast<std::size_t>(width) * height * 4, 0);
    drawSilhouette(human, width, 19, 14, yellow);

    // A wide solid block: the shape of a vehicle or a hot vent, not a person.
    std::vector<std::uint8_t> block(static_cast<std::size_t>(width) * height * 4, 0);
    for (int y = 18; y < 30; ++y) {
        for (int x = 8; x < 40; ++x) {
            setPixel(block, width, x, y, yellow);
        }
    }

    auto config = testConfig();
    config.shapeGateEnabled = true;
    config.profileGateEnabled = true;
    config.minimumComponentPixels = 20;
    config.minimumConfidence = 0.0;

    const auto humanResult = colorbot::analyzeBgra(
        human.data(), width, height, width * 4, targets, config);
    require(!humanResult.candidates.empty(), "An upright silhouette must pass both gates");
    require(
        humanResult.candidates.front().profileScore > 0.15,
        "A narrow head over a wide torso should score above the profile floor");

    const auto blockResult = colorbot::analyzeBgra(
        block.data(), width, height, width * 4, targets, config);
    require(blockResult.candidates.empty(), "A wide solid block must be rejected");
    require(
        blockResult.shapeRejections + blockResult.profileRejections > 0,
        "The rejection should be attributed to a silhouette gate");
}

void testThermalSegmentation() {
    constexpr int width = 48;
    constexpr int height = 48;
    // Thermal imagery is achromatic, so the color path cannot see it at all.
    const colorbot::Rgb hot{240, 240, 240};
    const colorbot::Rgb warmBackground{120, 120, 120};
    const std::vector targets{colorbot::makePrototype("yellow", colorbot::Rgb{255, 255, 35})};

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4, 0);
    for (std::size_t index = 0; index < static_cast<std::size_t>(width) * height; ++index) {
        setPixel(
            pixels,
            width,
            static_cast<int>(index % width),
            static_cast<int>(index / width),
            warmBackground);
    }
    drawSilhouette(pixels, width, 19, 14, hot);

    auto config = testConfig();
    config.minimumComponentPixels = 20;
    config.minimumConfidence = 0.0;

    const auto colorResult = colorbot::analyzeBgra(
        pixels.data(), width, height, width * 4, targets, config);
    require(
        colorResult.candidates.empty(),
        "Lab matching against yellow must not fire on a grayscale thermal frame");

    config.thermalMode = colorbot::ThermalMode::WhiteHot;
    config.thermalThreshold = 200;
    const auto thermalResult = colorbot::analyzeBgra(
        pixels.data(), width, height, width * 4, targets, config);
    require(!thermalResult.candidates.empty(), "White-hot mode should find the hot silhouette");
    require(
        thermalResult.candidates.front().area == 176,
        "The thermal component should cover the whole silhouette");

    // Black-hot inverts the polarity; the same bright target must vanish.
    config.thermalMode = colorbot::ThermalMode::BlackHot;
    const auto invertedResult = colorbot::analyzeBgra(
        pixels.data(), width, height, width * 4, targets, config);
    require(
        invertedResult.candidates.empty(),
        "Black-hot mode must not select a bright target");
}

void testThermalLocalContrastRejectsUniformBrightness() {
    constexpr int width = 48;
    constexpr int height = 48;
    const std::vector targets{colorbot::makePrototype("yellow", colorbot::Rgb{255, 255, 35})};

    // A uniformly bright frame, as produced by sky or a sunlit wall.
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4, 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            setPixel(pixels, width, x, y, colorbot::Rgb{230, 230, 230});
        }
    }

    auto config = testConfig();
    config.thermalMode = colorbot::ThermalMode::WhiteHot;
    config.thermalThreshold = 200;
    config.minimumComponentPixels = 20;
    config.minimumConfidence = 0.0;
    config.maximumComponentFraction = 1.0;

    const auto withoutContrast = colorbot::analyzeBgra(
        pixels.data(), width, height, width * 4, targets, config);
    require(
        withoutContrast.matchedPixels > 0,
        "A plain threshold accepts a uniformly bright frame");

    config.thermalLocalContrast = 25;
    config.thermalLocalRadius = 8;
    const auto withContrast = colorbot::analyzeBgra(
        pixels.data(), width, height, width * 4, targets, config);
    require(
        withContrast.matchedPixels == 0,
        "Local-contrast gating must reject a uniformly bright frame");
}

void testPersistenceRecoversIntermittentTarget() {
    constexpr int width = 48;
    constexpr int height = 48;
    const colorbot::Rgb yellow{255, 255, 35};
    const std::vector targets{colorbot::makePrototype("yellow", yellow)};

    // Alternating frames expose complementary halves of the silhouette, so no
    // single frame ever contains enough of it to clear the area floor.
    std::vector<std::vector<std::uint8_t>> frames(2,
        std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4, 0));
    for (int y = 14; y < 34; ++y) {
        for (int x = 19; x < 29; ++x) {
            const int frame = ((y - 14) / 2) % 2;
            setPixel(frames[static_cast<std::size_t>(frame)], width, x, y, yellow);
        }
    }

    auto config = testConfig();
    config.minimumComponentPixels = 150;

    const auto single = colorbot::analyzeBgra(
        frames[0].data(), width, height, width * 4, targets, config);
    require(single.candidates.empty(), "A half-exposed target should not clear the area floor");

    colorbot::PersistenceAccumulator persistence(width, height, 0.8, 0.5);
    bool recovered = false;
    for (int frame = 0; frame < 6; ++frame) {
        const auto result = colorbot::analyzeBgra(
            frames[static_cast<std::size_t>(frame % 2)].data(),
            width,
            height,
            width * 4,
            targets,
            config,
            &persistence,
            0.45);
        recovered = recovered || !result.candidates.empty();
    }
    require(recovered, "Cross-frame persistence should reassemble an intermittent target");

    persistence.reset();
    const auto afterReset = colorbot::analyzeBgra(
        frames[0].data(), width, height, width * 4, targets, config, &persistence, 0.45);
    require(afterReset.candidates.empty(), "Resetting persistence should discard prior evidence");
}

void testPersistenceRejectsInvalidConfiguration() {
    bool threw = false;
    try {
        colorbot::PersistenceAccumulator persistence(16, 16, 1.0, 0.5);
        (void)persistence;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "A decay of 1.0 never forgets and must be rejected");

    threw = false;
    try {
        colorbot::PersistenceAccumulator persistence(16, 16, 0.8, 0.5);
        const std::vector targets{colorbot::makePrototype("yellow", colorbot::Rgb{255, 255, 35})};
        std::vector<std::uint8_t> pixels(32 * 32 * 4, 0);
        (void)colorbot::analyzeBgra(
            pixels.data(), 32, 32, 32 * 4, targets, testConfig(), &persistence, 0.45);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "A persistence buffer of the wrong size must be rejected");
}

}  // namespace

int main() {
    try {
        testColorConversion();
        testConnectedComponentDetection();
        testNoiseAndGateRejection();
        testTemporalGate();
        testTargetTracker();
        testDistanceEstimate();
        testMorphologicalClosingBridgesOccluders();
        testProximityMergingRecoversSplitTarget();
        testProximityMergingKeepsSideBySideTargetsApart();
        testShapeAndProfileGates();
        testThermalSegmentation();
        testThermalLocalContrastRejectsUniformBrightness();
        testPersistenceRecoversIntermittentTarget();
        testPersistenceRejectsInvalidConfiguration();
        runSyntheticBenchmark();
        std::cout << "All detector-core tests passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
