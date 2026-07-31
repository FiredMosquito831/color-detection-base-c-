#define NOMINMAX
#include <Windows.h>

#include <shellapi.h>

#include "detector_core.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

namespace {

using Clock = std::chrono::steady_clock;
constexpr UINT kSnapshotMessage = WM_APP + 1;
constexpr int kToggleHotkey = 1;
constexpr int kExitHotkey = 2;

struct AppConfig {
    int roiWidth{256};
    int roiHeight{192};
    int targetFps{120};
    int maxFrames{0};
    bool triggerEnabled{false};
    bool trackingEnabled{false};
    bool overlayEnabled{true};
    bool soundEnabled{false};
    bool diagnosticsEnabled{false};
    bool keyTestRequested{false};
    bool elevateRequested{false};
    bool detectorOnlyRequested{false};
    bool persistenceEnabled{false};
    double persistenceDecay{0.80};
    double persistenceThreshold{0.45};
    double persistenceGain{0.34};
    int triggerVirtualKey{VK_XBUTTON1};
    int trackingVirtualKey{VK_XBUTTON2};
    // Latching alternatives to the hold keys. Either path activates its
    // controller; the hold key remains usable while a toggle is off.
    int triggerToggleVirtualKey{VK_OEM_MINUS};
    int trackingToggleVirtualKey{VK_OEM_PLUS};
    // Aim from the crosshair at screen center rather than the OS cursor. This
    // is correct for any application that reads raw input and hides its
    // pointer; cursor mode is for desktop demonstrations.
    bool aimAtScreenCenter{true};
    // How long the left button is held down. A game that samples input once
    // per frame needs the press to span at least one sample.
    int clickHoldMilliseconds{40};
    double trackingGain{0.45};
    int trackingDeadZonePixels{3};
    int trackingMaximumStepPixels{35};
    double knownObjectHeight{};
    double verticalFovDegrees{};
    colorbot::DetectionConfig detection;
    colorbot::TemporalConfig temporal;
    colorbot::TrackerConfig tracker;
    std::vector<colorbot::ColorPrototype> targets{
        colorbot::makePrototype("yellow", colorbot::Rgb{255, 255, 35})};
};

struct UiSnapshot {
    RECT roi{};
    RECT gate{};
    std::optional<RECT> candidate;
    bool enabled{true};
    bool rawDetected{};
    bool temporallyActive{};
    bool triggerEnabled{};
    bool trackingEnabled{};
    bool triggerHeld{};
    bool trackingHeld{};
    bool triggerToggled{};
    bool trackingToggled{};
    bool pinVisible{};
    bool pinOccluded{};
    int matchedPixels{};
    int componentArea{};
    int candidateCount{};
    int fragments{};
    int shapeRejections{};
    int profileRejections{};
    double profileScore{};
    double aspectRatio{};
    double fillRatio{};
    bool silhouetteTelemetry{};
    double confidence{};
    double processingMilliseconds{};
    double loopFps{};
    std::optional<double> distance;
    std::optional<RECT> pinnedCandidate;
    std::optional<POINT> pinnedPoint;
    std::optional<POINT> cursorPoint;
};

struct SharedState {
    std::atomic<bool> stop{false};
    std::atomic<bool> enabled{true};
    // Set when RegisterHotKey succeeded for both F8 and F9. When it did not
    // (another application already owns those combinations), the detection
    // worker falls back to polling their async key state instead.
    std::atomic<bool> hotkeysRegistered{false};
    std::mutex snapshotMutex;
    UiSnapshot snapshot;
};

[[nodiscard]] std::string requireValue(int& index, int argc, char* argv[], const char* option) {
    if (index + 1 >= argc) {
        throw std::invalid_argument(std::string("Missing value after ") + option);
    }
    return argv[++index];
}

[[nodiscard]] int parseInteger(const std::string& text, const char* label) {
    std::size_t consumed = 0;
    const int value = std::stoi(text, &consumed);
    if (consumed != text.size()) {
        throw std::invalid_argument(std::string("Invalid ") + label + ": " + text);
    }
    return value;
}

[[nodiscard]] double parseDouble(const std::string& text, const char* label) {
    std::size_t consumed = 0;
    const double value = std::stod(text, &consumed);
    if (consumed != text.size()) {
        throw std::invalid_argument(std::string("Invalid ") + label + ": " + text);
    }
    return value;
}

[[nodiscard]] std::pair<int, int> parseDimensions(const std::string& text) {
    const auto separator = text.find_first_of("xX");
    if (separator == std::string::npos) {
        throw std::invalid_argument("ROI must use WIDTHxHEIGHT, for example 256x192");
    }
    return {
        parseInteger(text.substr(0, separator), "ROI width"),
        parseInteger(text.substr(separator + 1), "ROI height")};
}

[[nodiscard]] std::pair<double, double> parseRange(const std::string& text, const char* what) {
    const auto separator = text.find(',');
    if (separator == std::string::npos) {
        throw std::invalid_argument(
            std::string("The ") + what + " range must use MIN,MAX, for example 0.2,0.9");
    }
    const double minimum = parseDouble(text.substr(0, separator), what);
    const double maximum = parseDouble(text.substr(separator + 1), what);
    if (minimum > maximum) {
        throw std::invalid_argument(
            std::string("The ") + what + " range minimum must not exceed its maximum");
    }
    return {minimum, maximum};
}

[[nodiscard]] colorbot::Rgb parseRgb(const std::string& text) {
    std::istringstream input(text);
    std::string part;
    int values[3]{};
    for (int index = 0; index < 3; ++index) {
        if (!std::getline(input, part, ',')) {
            throw std::invalid_argument("Target must use R,G,B, for example 255,255,35");
        }
        values[index] = parseInteger(part, "RGB channel");
        if (values[index] < 0 || values[index] > 255) {
            throw std::invalid_argument("RGB channels must be between 0 and 255");
        }
    }
    if (std::getline(input, part, ',')) {
        throw std::invalid_argument("Target must contain exactly three RGB channels");
    }
    return colorbot::Rgb{
        static_cast<std::uint8_t>(values[0]),
        static_cast<std::uint8_t>(values[1]),
        static_cast<std::uint8_t>(values[2])};
}

[[nodiscard]] std::string lowerCase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return text;
}

[[nodiscard]] int parseVirtualKey(const std::string& input) {
    const std::string key = lowerCase(input);
    if (key == "space") return VK_SPACE;
    if (key == "shift" || key == "lshift") return VK_LSHIFT;
    if (key == "rshift") return VK_RSHIFT;
    if (key == "ctrl" || key == "lctrl") return VK_LCONTROL;
    if (key == "rctrl") return VK_RCONTROL;
    if (key == "alt" || key == "lalt") return VK_LMENU;
    if (key == "ralt") return VK_RMENU;
    if (key == "xbutton1" || key == "mouse4") return VK_XBUTTON1;
    if (key == "xbutton2" || key == "mouse5") return VK_XBUTTON2;
    if (key == "-" || key == "minus") return VK_OEM_MINUS;
    if (key == "=" || key == "equals" || key == "plus") return VK_OEM_PLUS;
    if (key == "[" || key == "lbracket") return VK_OEM_4;
    if (key == "]" || key == "rbracket") return VK_OEM_6;
    if (key.size() >= 2 && key[0] == 'f') {
        const int functionNumber = parseInteger(key.substr(1), "function key");
        if (functionNumber >= 1 && functionNumber <= 24) {
            return VK_F1 + functionNumber - 1;
        }
    }
    if (key.size() == 1) {
        const unsigned char character = static_cast<unsigned char>(key[0]);
        if (std::isalnum(character)) {
            return std::toupper(character);
        }
    }
    throw std::invalid_argument(
        "Unsupported key '" + input +
        "'. Use F1-F24, A-Z, 0-9, space, shift, ctrl, alt, mouse4, mouse5, "
        "minus, equals, lbracket, or rbracket.");
}

[[nodiscard]] std::string virtualKeyName(int virtualKey) {
    if (virtualKey >= VK_F1 && virtualKey <= VK_F24) {
        return "F" + std::to_string(virtualKey - VK_F1 + 1);
    }
    if (virtualKey >= 'A' && virtualKey <= 'Z') return std::string(1, static_cast<char>(virtualKey));
    if (virtualKey >= '0' && virtualKey <= '9') return std::string(1, static_cast<char>(virtualKey));
    if (virtualKey == VK_SPACE) return "Space";
    if (virtualKey == VK_LSHIFT) return "Left Shift";
    if (virtualKey == VK_RSHIFT) return "Right Shift";
    if (virtualKey == VK_LCONTROL) return "Left Ctrl";
    if (virtualKey == VK_RCONTROL) return "Right Ctrl";
    if (virtualKey == VK_LMENU) return "Left Alt";
    if (virtualKey == VK_RMENU) return "Right Alt";
    if (virtualKey == VK_XBUTTON1) return "Mouse 4";
    if (virtualKey == VK_XBUTTON2) return "Mouse 5";
    if (virtualKey == VK_OEM_MINUS) return "-";
    if (virtualKey == VK_OEM_PLUS) return "=";
    if (virtualKey == VK_OEM_4) return "[";
    if (virtualKey == VK_OEM_6) return "]";
    return "VK " + std::to_string(virtualKey);
}

void printHelp() {
    std::cout
        << "Color Detection Engineering PoC\n\n"
        << "Default behavior is detector-only: it draws/logs detections but does not click.\n\n"
        << "Usage: color_detector [options]\n\n"
        << "  --trigger               Hold trigger key to click on a confirmed center target\n"
        << "  --click                 Alias for --trigger\n"
        << "  --track                 Hold tracking key to pin/follow the closest target\n"
        << "  --detector-only         Observe and draw only; no keys actuate anything\n"
        << "  --trigger-key KEY       Hold key for trigger mode (default Mouse 4)\n"
        << "  --track-key KEY         Hold key for tracking mode (default Mouse 5)\n"
        << "  --trigger-toggle-key KEY  Latching toggle for trigger mode (default -)\n"
        << "  --track-toggle-key KEY  Latching toggle for tracking mode (default =)\n"
        << "  --aim-reference WHICH   Aim from screen 'center' (crosshair) or 'cursor'\n"
        << "                          (default center; use cursor for desktop demos)\n"
        << "  --click-hold-ms MS      Left-button hold duration (default 40)\n"
        << "  --track-gain VALUE      Proportional cursor-follow gain (default 0.45)\n"
        << "  --track-deadzone PX     Stop moving inside this error radius (default 3)\n"
        << "  --track-max-step PX     Maximum relative movement per frame (default 35)\n"
        << "  --reacquire-radius PX   Maximum target association distance (default 80)\n"
        << "  --occlusion-frames N    Frames to retain a temporarily hidden pin (default 6)\n"
        << "  --pin-smoothing VALUE   Target-position EMA from 0 to 1 (default 0.35)\n"
        << "  --sound                 Play an asynchronous notification on trigger\n"
        << "  --diagnostics           Log candidate counts once per second\n"
        << "  --keytest               Report live key state and injection permissions, then exit\n"
        << "  --elevate               Relaunch elevated; required to read keys and send input\n"
        << "                          while an application running as administrator is focused\n"
        << "  --no-overlay            Run with the overlay window hidden\n"
        << "  --roi WIDTHxHEIGHT      Center capture region (default 256x192)\n"
        << "  --gate PIXELS           Center activation gate size (default 8)\n"
        << "  --fps FPS               Capture target rate (default 120)\n"
        << "  --max-frames N          Stop after N frames; useful for smoke tests\n"
        << "  --target R,G,B          Add a target; first custom target replaces defaults\n"
        << "  --delta-e VALUE         Maximum CIE Lab color distance (default 24)\n"
        << "  --min-saturation N      Minimum saturation from 0 to 255 (default 35)\n"
        << "  --min-area PIXELS       Minimum connected-component area (default 8)\n"
        << "  --min-gate PIXELS       Minimum component pixels inside gate (default 2)\n"
        << "  --max-area-ratio VALUE  Reject components covering too much ROI (default 0.25)\n"
        << "\n Silhouette and thermal options (all off by default):\n"
        << "  --thermal MODE          Segment on intensity: white, black, or off\n"
        << "  --thermal-threshold N   Intensity a hot pixel must reach (default 170)\n"
        << "  --thermal-contrast N    Required excess over the local mean (default 0 = off)\n"
        << "  --thermal-radius N      Local-mean window radius (default 16)\n"
        << "  --close-radius N        Morphological closing radius; bridges thin occluders\n"
        << "  --merge-gap N           Merge vertically aligned components within N pixels\n"
        << "  --shape-gate            Require human-like aspect and fill ratios\n"
        << "  --aspect MIN,MAX        Bounding-box width/height range (default 0.20,0.95)\n"
        << "  --fill MIN,MAX          Area/box-area range (default 0.20,0.92)\n"
        << "  --profile-gate          Require a narrower top than middle\n"
        << "  --min-profile VALUE     Minimum profile score from 0 to 1 (default 0.15)\n"
        << "  --persistence DECAY     Accumulate evidence across frames, decay 0 to <1\n"
        << "  --persistence-threshold V  Evidence needed to keep a pixel (default 0.45)\n"
        << "  --thermal-human         Preset combining all of the above for occluded people\n"
        << '\n'
        << "  --confidence VALUE      Minimum score from 0 to 1 (default 0.55)\n"
        << "  --confirm FRAMES        Frames required to activate (default 3)\n"
        << "  --release FRAMES        Misses required to release (default 3)\n"
        << "  --cooldown-ms MS        Minimum interval between trigger events (default 250)\n"
        << "  --known-height METERS   Known real object height for range estimation\n"
        << "  --vfov DEGREES          Calibrated vertical field of view\n"
        << "  --help                   Show this help\n\n"
        << "Controls: hold Mouse 4 to trigger or press - to latch it on; hold Mouse 5 to\n"
        << "pin/track or press = to latch it on; F8 pauses; F9 exits.\n";
}

[[nodiscard]] AppConfig parseArguments(int argc, char* argv[], bool& helpRequested) {
    AppConfig config;
    bool customTargets = false;
    helpRequested = false;

    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--help" || option == "-h") {
            helpRequested = true;
        } else if (option == "--click" || option == "--trigger") {
            config.triggerEnabled = true;
        } else if (option == "--track") {
            config.trackingEnabled = true;
        } else if (option == "--detector-only") {
            config.detectorOnlyRequested = true;
        } else if (option == "--keytest") {
            config.keyTestRequested = true;
        } else if (option == "--elevate") {
            config.elevateRequested = true;
        } else if (option == "--trigger-key") {
            config.triggerVirtualKey = parseVirtualKey(
                requireValue(index, argc, argv, "--trigger-key"));
        } else if (option == "--track-key") {
            config.trackingVirtualKey = parseVirtualKey(
                requireValue(index, argc, argv, "--track-key"));
        } else if (option == "--trigger-toggle-key") {
            config.triggerToggleVirtualKey = parseVirtualKey(
                requireValue(index, argc, argv, "--trigger-toggle-key"));
        } else if (option == "--track-toggle-key") {
            config.trackingToggleVirtualKey = parseVirtualKey(
                requireValue(index, argc, argv, "--track-toggle-key"));
        } else if (option == "--aim-reference") {
            const std::string reference = requireValue(index, argc, argv, "--aim-reference");
            if (reference == "center" || reference == "crosshair") {
                config.aimAtScreenCenter = true;
            } else if (reference == "cursor") {
                config.aimAtScreenCenter = false;
            } else {
                throw std::invalid_argument(
                    "Unsupported aim reference '" + reference + "'. Use center or cursor.");
            }
        } else if (option == "--click-hold-ms") {
            config.clickHoldMilliseconds = parseInteger(
                requireValue(index, argc, argv, "--click-hold-ms"), "click hold");
        } else if (option == "--track-gain") {
            config.trackingGain = parseDouble(
                requireValue(index, argc, argv, "--track-gain"), "tracking gain");
        } else if (option == "--track-deadzone") {
            config.trackingDeadZonePixels = parseInteger(
                requireValue(index, argc, argv, "--track-deadzone"), "tracking dead zone");
        } else if (option == "--track-max-step") {
            config.trackingMaximumStepPixels = parseInteger(
                requireValue(index, argc, argv, "--track-max-step"), "tracking maximum step");
        } else if (option == "--reacquire-radius") {
            config.tracker.reacquireRadiusPixels = parseDouble(
                requireValue(index, argc, argv, "--reacquire-radius"), "reacquire radius");
        } else if (option == "--occlusion-frames") {
            config.tracker.maximumMissedFrames = parseInteger(
                requireValue(index, argc, argv, "--occlusion-frames"), "occlusion frames");
        } else if (option == "--pin-smoothing") {
            config.tracker.smoothingFactor = parseDouble(
                requireValue(index, argc, argv, "--pin-smoothing"), "pin smoothing");
        } else if (option == "--sound") {
            config.soundEnabled = true;
        } else if (option == "--diagnostics") {
            config.diagnosticsEnabled = true;
        } else if (option == "--no-overlay") {
            config.overlayEnabled = false;
        } else if (option == "--roi") {
            const auto [width, height] = parseDimensions(requireValue(index, argc, argv, "--roi"));
            config.roiWidth = width;
            config.roiHeight = height;
        } else if (option == "--gate") {
            config.detection.triggerGateSize = parseInteger(
                requireValue(index, argc, argv, "--gate"), "gate size");
        } else if (option == "--fps") {
            config.targetFps = parseInteger(requireValue(index, argc, argv, "--fps"), "FPS");
        } else if (option == "--max-frames") {
            config.maxFrames = parseInteger(
                requireValue(index, argc, argv, "--max-frames"), "maximum frame count");
        } else if (option == "--target") {
            if (!customTargets) {
                config.targets.clear();
                customTargets = true;
            }
            const auto rgb = parseRgb(requireValue(index, argc, argv, "--target"));
            config.targets.push_back(colorbot::makePrototype("custom-" + std::to_string(config.targets.size() + 1), rgb));
        } else if (option == "--delta-e") {
            config.detection.maxDeltaE = parseDouble(
                requireValue(index, argc, argv, "--delta-e"), "Delta E");
        } else if (option == "--min-saturation") {
            config.detection.minimumSaturation = parseInteger(
                requireValue(index, argc, argv, "--min-saturation"), "minimum saturation");
        } else if (option == "--min-area") {
            config.detection.minimumComponentPixels = parseInteger(
                requireValue(index, argc, argv, "--min-area"), "minimum area");
        } else if (option == "--min-gate") {
            config.detection.minimumGatePixels = parseInteger(
                requireValue(index, argc, argv, "--min-gate"), "minimum gate pixels");
        } else if (option == "--max-area-ratio") {
            config.detection.maximumComponentFraction = parseDouble(
                requireValue(index, argc, argv, "--max-area-ratio"), "maximum area ratio");
        } else if (option == "--thermal") {
            const std::string mode = requireValue(index, argc, argv, "--thermal");
            if (mode == "white" || mode == "white-hot") {
                config.detection.thermalMode = colorbot::ThermalMode::WhiteHot;
            } else if (mode == "black" || mode == "black-hot") {
                config.detection.thermalMode = colorbot::ThermalMode::BlackHot;
            } else if (mode == "off") {
                config.detection.thermalMode = colorbot::ThermalMode::Off;
            } else {
                throw std::invalid_argument(
                    "Unsupported thermal mode '" + mode + "'. Use white, black, or off.");
            }
        } else if (option == "--thermal-threshold") {
            config.detection.thermalThreshold = parseInteger(
                requireValue(index, argc, argv, "--thermal-threshold"), "thermal threshold");
        } else if (option == "--thermal-contrast") {
            config.detection.thermalLocalContrast = parseInteger(
                requireValue(index, argc, argv, "--thermal-contrast"), "thermal local contrast");
        } else if (option == "--thermal-radius") {
            config.detection.thermalLocalRadius = parseInteger(
                requireValue(index, argc, argv, "--thermal-radius"), "thermal local radius");
        } else if (option == "--close-radius") {
            config.detection.morphologyRadius = parseInteger(
                requireValue(index, argc, argv, "--close-radius"), "closing radius");
        } else if (option == "--merge-gap") {
            config.detection.mergeGapPixels = parseInteger(
                requireValue(index, argc, argv, "--merge-gap"), "merge gap");
        } else if (option == "--shape-gate") {
            config.detection.shapeGateEnabled = true;
        } else if (option == "--aspect") {
            const auto [minimum, maximum] =
                parseRange(requireValue(index, argc, argv, "--aspect"), "aspect ratio");
            config.detection.minimumAspectRatio = minimum;
            config.detection.maximumAspectRatio = maximum;
        } else if (option == "--fill") {
            const auto [minimum, maximum] =
                parseRange(requireValue(index, argc, argv, "--fill"), "fill ratio");
            config.detection.minimumFillRatio = minimum;
            config.detection.maximumFillRatio = maximum;
        } else if (option == "--profile-gate") {
            config.detection.profileGateEnabled = true;
        } else if (option == "--min-profile") {
            config.detection.minimumProfileScore = parseDouble(
                requireValue(index, argc, argv, "--min-profile"), "minimum profile score");
        } else if (option == "--persistence") {
            config.persistenceDecay = parseDouble(
                requireValue(index, argc, argv, "--persistence"), "persistence decay");
            config.persistenceEnabled = true;
        } else if (option == "--persistence-threshold") {
            config.persistenceThreshold = parseDouble(
                requireValue(index, argc, argv, "--persistence-threshold"),
                "persistence threshold");
        } else if (option == "--thermal-human") {
            // Preset for the occluded-silhouette case: intensity segmentation,
            // gap bridging, fragment reassembly, silhouette gating and
            // cross-frame evidence, all at once.
            config.detection.thermalMode = colorbot::ThermalMode::WhiteHot;
            config.detection.thermalLocalContrast = 25;
            config.detection.morphologyRadius = 2;
            config.detection.mergeGapPixels = 6;
            config.detection.shapeGateEnabled = true;
            config.detection.profileGateEnabled = true;
            config.detection.minimumComponentPixels = 24;
            config.persistenceEnabled = true;
            config.persistenceDecay = 0.80;
        } else if (option == "--confidence") {
            config.detection.minimumConfidence = parseDouble(
                requireValue(index, argc, argv, "--confidence"), "confidence");
        } else if (option == "--confirm") {
            config.temporal.confirmationFrames = parseInteger(
                requireValue(index, argc, argv, "--confirm"), "confirmation frames");
        } else if (option == "--release") {
            config.temporal.releaseFrames = parseInteger(
                requireValue(index, argc, argv, "--release"), "release frames");
        } else if (option == "--cooldown-ms") {
            config.temporal.retriggerCooldown = std::chrono::milliseconds(parseInteger(
                requireValue(index, argc, argv, "--cooldown-ms"), "cooldown"));
        } else if (option == "--known-height") {
            config.knownObjectHeight = parseDouble(
                requireValue(index, argc, argv, "--known-height"), "known height");
        } else if (option == "--vfov") {
            config.verticalFovDegrees = parseDouble(
                requireValue(index, argc, argv, "--vfov"), "vertical FOV");
        } else {
            throw std::invalid_argument("Unknown option: " + option);
        }
    }

    if (config.roiWidth <= 0 || config.roiHeight <= 0 || config.targetFps <= 0 ||
        config.targetFps > 1000 || config.maxFrames < 0) {
        throw std::invalid_argument("ROI, FPS and frame limit values are outside their valid range");
    }
    if (config.detection.triggerGateSize <= 0 ||
        config.detection.triggerGateSize > std::min(config.roiWidth, config.roiHeight)) {
        throw std::invalid_argument("Gate must be positive and fit inside the ROI");
    }
    if (config.detection.minimumSaturation < 0 || config.detection.minimumSaturation > 255 ||
        config.detection.minimumConfidence < 0.0 || config.detection.minimumConfidence > 1.0 ||
        config.detection.maximumComponentFraction <= 0.0 ||
        config.detection.maximumComponentFraction > 1.0) {
        throw std::invalid_argument("Saturation, confidence, or area ratio is outside its valid range");
    }
    if (config.clickHoldMilliseconds < 1 || config.clickHoldMilliseconds > 1000) {
        throw std::invalid_argument("Click hold must be between 1 and 1000 milliseconds");
    }
    if (config.trackingGain <= 0.0 || config.trackingGain > 1.0 ||
        config.trackingDeadZonePixels < 0 || config.trackingMaximumStepPixels <= 0 ||
        config.tracker.maximumMissedFrames < 0 || config.tracker.reacquireRadiusPixels <= 0.0 ||
        config.tracker.smoothingFactor <= 0.0 || config.tracker.smoothingFactor > 1.0) {
        throw std::invalid_argument("Tracking controller values are outside their valid range");
    }
    if (config.triggerToggleVirtualKey == config.triggerVirtualKey ||
        config.trackingToggleVirtualKey == config.trackingVirtualKey ||
        config.triggerToggleVirtualKey == config.trackingToggleVirtualKey) {
        throw std::invalid_argument(
            "Each toggle key must differ from its hold key and from the other toggle key");
    }
    if (config.detection.thermalThreshold < 0 || config.detection.thermalThreshold > 255 ||
        config.detection.thermalLocalContrast < 0 || config.detection.thermalLocalContrast > 255 ||
        config.detection.thermalLocalRadius <= 0) {
        throw std::invalid_argument("Thermal segmentation values are outside their valid range");
    }
    if (config.detection.morphologyRadius < 0 || config.detection.morphologyRadius > 32 ||
        config.detection.mergeGapPixels < 0) {
        throw std::invalid_argument("Closing radius or merge gap is outside its valid range");
    }
    if (config.detection.minimumAspectRatio < 0.0 || config.detection.minimumFillRatio < 0.0 ||
        config.detection.maximumFillRatio > 1.0 ||
        config.detection.minimumProfileScore < 0.0 ||
        config.detection.minimumProfileScore > 1.0) {
        throw std::invalid_argument("Shape or profile gate values are outside their valid range");
    }
    if (config.persistenceDecay < 0.0 || config.persistenceDecay >= 1.0 ||
        config.persistenceThreshold <= 0.0 || config.persistenceThreshold > 1.0) {
        throw std::invalid_argument("Persistence decay must be in [0,1) and threshold in (0,1]");
    }
    const bool partialDistanceConfig =
        (config.knownObjectHeight > 0.0) != (config.verticalFovDegrees > 0.0);
    if (partialDistanceConfig) {
        throw std::invalid_argument("Distance estimation requires both --known-height and --vfov");
    }
    return config;
}

class ScreenRoiCapture {
public:
    ScreenRoiCapture(int width, int height) : width_(width), height_(height), stride_(width * 4) {
        screenDc_ = GetDC(nullptr);
        if (!screenDc_) {
            throw std::runtime_error("GetDC failed");
        }
        memoryDc_ = CreateCompatibleDC(screenDc_);
        if (!memoryDc_) {
            releaseResources();
            throw std::runtime_error("CreateCompatibleDC failed");
        }

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width_;
        info.bmiHeader.biHeight = -height_;  // top-down image
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap_ = CreateDIBSection(screenDc_, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels_), nullptr, 0);
        if (!bitmap_ || !pixels_) {
            releaseResources();
            throw std::runtime_error("CreateDIBSection failed");
        }
        previousBitmap_ = SelectObject(memoryDc_, bitmap_);
        if (!previousBitmap_ || previousBitmap_ == HGDI_ERROR) {
            previousBitmap_ = nullptr;
            releaseResources();
            throw std::runtime_error("SelectObject failed");
        }
    }

    ScreenRoiCapture(const ScreenRoiCapture&) = delete;
    ScreenRoiCapture& operator=(const ScreenRoiCapture&) = delete;

    ~ScreenRoiCapture() { releaseResources(); }

    [[nodiscard]] bool capture(int screenX, int screenY) {
        return BitBlt(memoryDc_, 0, 0, width_, height_, screenDc_, screenX, screenY, SRCCOPY) != FALSE;
    }

    [[nodiscard]] const std::uint8_t* pixels() const { return pixels_; }
    [[nodiscard]] int stride() const { return stride_; }

private:
    void releaseResources() noexcept {
        if (memoryDc_ && previousBitmap_ && previousBitmap_ != HGDI_ERROR) {
            SelectObject(memoryDc_, previousBitmap_);
        }
        if (bitmap_) {
            DeleteObject(bitmap_);
        }
        if (memoryDc_) {
            DeleteDC(memoryDc_);
        }
        if (screenDc_) {
            ReleaseDC(nullptr, screenDc_);
        }
        previousBitmap_ = nullptr;
        bitmap_ = nullptr;
        memoryDc_ = nullptr;
        screenDc_ = nullptr;
        pixels_ = nullptr;
    }
    int width_{};
    int height_{};
    int stride_{};
    HDC screenDc_{};
    HDC memoryDc_{};
    HBITMAP bitmap_{};
    HGDIOBJ previousBitmap_{};
    std::uint8_t* pixels_{};
};

// Press and release are sent as two separate events separated in time.
//
// Submitting both in one SendInput batch gives them the same timestamp and
// delivers them between two consecutive input polls. An application that
// samples button state once per frame then observes the button down and up
// within a single sample and registers no click at all, which is why the
// original single-batch click had no effect in a game while succeeding on the
// desktop.
[[nodiscard]] bool sendLeftButtonDown() {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

[[nodiscard]] bool sendLeftButtonUp() {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

[[nodiscard]] bool isKeyHeld(int virtualKey) {
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

// True when this process runs with an elevated (high-integrity) token.
// A medium-integrity process cannot SendInput into an elevated foreground
// window: the call succeeds and returns 1, but User Interface Privilege
// Isolation silently discards the event. That is the single most common
// reason trigger/track appear dead over a game running as administrator.
[[nodiscard]] bool isProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const bool ok =
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned) != FALSE;
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

// The foreground window belongs to a process we are not allowed to open for
// query. That is a strong indicator of a higher-integrity target (elevated
// game or anti-cheat protected process) whose input we cannot synthesise.
[[nodiscard]] bool foregroundWindowIsInaccessible() {
    const HWND foreground = GetForegroundWindow();
    if (!foreground) {
        return false;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    if (processId == 0 || processId == GetCurrentProcessId()) {
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) {
        return true;
    }
    CloseHandle(process);
    return false;
}

// Relaunches this executable with an elevated token, preserving the original
// arguments minus --elevate. Windows denies a medium-integrity process both
// the ability to read key state while a higher-integrity window is in the
// foreground and the ability to inject input into it, so matching the target's
// integrity level is the only way to make hold-to-trigger work over an
// elevated application.
[[nodiscard]] bool relaunchElevated(int argc, char* argv[]) {
    wchar_t modulePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) == 0) {
        std::cerr << "Error: could not determine this executable's path for elevation.\n";
        return false;
    }

    std::wstring parameters;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--elevate") {
            continue;
        }
        if (!parameters.empty()) {
            parameters.push_back(L' ');
        }
        parameters.push_back(L'"');
        parameters.append(argument.begin(), argument.end());
        parameters.push_back(L'"');
    }

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = modulePath;
    info.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        const DWORD error = GetLastError();
        if (error == ERROR_CANCELLED) {
            std::cerr << "Elevation was declined at the consent prompt.\n";
        } else {
            std::cerr << "Error: elevation failed with code " << error << ".\n";
        }
        return false;
    }
    if (info.hProcess) {
        CloseHandle(info.hProcess);
    }
    return true;
}

// Interactive input self-test. Runs no capture and injects nothing; it only
// reports what the process can actually observe, so a dead hotkey can be
// attributed to the correct layer (key never reaches us, vs. detection never
// fires, vs. injection being discarded).
void runKeyTest(const AppConfig& config) {
    std::cout << "Key self-test. Hold keys to see live state. Press F9 to quit.\n"
              << "elevated=" << (isProcessElevated() ? "yes" : "no") << "\n\n";

    const struct {
        const char* label;
        int key;
    } watched[] = {
        {"trigger", config.triggerVirtualKey},
        {"track", config.trackingVirtualKey},
        {"trigger-toggle", config.triggerToggleVirtualKey},
        {"track-toggle", config.trackingToggleVirtualKey},
        {"F8", VK_F8},
        {"F9", VK_F9},
        {"LBUTTON", VK_LBUTTON},
        {"XBUTTON1", VK_XBUTTON1},
        {"XBUTTON2", VK_XBUTTON2},
    };

    bool warnedAboutForeground = false;
    while (!isKeyHeld(VK_F9)) {
        std::ostringstream line;
        for (const auto& entry : watched) {
            line << entry.label << '=' << (isKeyHeld(entry.key) ? '1' : '0') << ' ';
        }
        if (!warnedAboutForeground && foregroundWindowIsInaccessible()) {
            warnedAboutForeground = true;
            std::cout << "\nNote: the foreground window belongs to a process this one cannot "
                         "query.\nInjected clicks and cursor movement will be discarded unless "
                         "this detector\nis started as administrator.\n";
        }
        std::cout << '\r' << line.str() << std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::cout << "\nKey self-test finished.\n";
}

// Emits a relative mouse movement that reduces the error between an aim
// reference point and the target.
//
// The reference must be the point the application actually aims from. In a
// game that is the crosshair at screen center, not the operating-system
// cursor: the cursor is hidden and parked while the game reads raw input, so
// it neither tracks the view nor responds to injected relative movement.
// Driving the loop from GetCursorPos therefore compares the target against a
// stale point, and the controller converges around the target instead of onto
// it.
[[nodiscard]] bool moveCursorToward(
    const POINT& reference,
    double targetX,
    double targetY,
    double gain,
    int deadZonePixels,
    int maximumStepPixels) {
    const double errorX = targetX - reference.x;
    const double errorY = targetY - reference.y;
    const double distance = std::hypot(errorX, errorY);
    if (distance <= deadZonePixels) {
        return true;
    }

    double scale = gain;
    if (distance * scale > maximumStepPixels) {
        scale = static_cast<double>(maximumStepPixels) / distance;
    }
    const LONG movementX = static_cast<LONG>(std::lround(errorX * scale));
    const LONG movementY = static_cast<LONG>(std::lround(errorY * scale));
    if (movementX == 0 && movementY == 0) {
        return true;
    }

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = movementX;
    input.mi.dy = movementY;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

[[nodiscard]] RECT candidateToScreenRect(const colorbot::Candidate& candidate, const RECT& roi) {
    return RECT{
        roi.left + candidate.box.left,
        roi.top + candidate.box.top,
        roi.left + candidate.box.right + 1,
        roi.top + candidate.box.bottom + 1};
}

void drawOutline(HDC dc, const RECT& rectangle, COLORREF color, int thickness) {
    HPEN pen = CreatePen(PS_SOLID, thickness, color);
    if (!pen) {
        return;
    }
    const HGDIOBJ previousPen = SelectObject(dc, pen);
    const HGDIOBJ previousBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, rectangle.left, rectangle.top, rectangle.right, rectangle.bottom);
    SelectObject(dc, previousBrush);
    SelectObject(dc, previousPen);
    DeleteObject(pen);
}

void drawTrackingLink(HDC dc, const POINT& from, const POINT& to, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    if (!pen) {
        return;
    }
    const HGDIOBJ previousPen = SelectObject(dc, pen);
    const HGDIOBJ previousBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    MoveToEx(dc, from.x, from.y, nullptr);
    LineTo(dc, to.x, to.y);
    Ellipse(dc, to.x - 4, to.y - 4, to.x + 5, to.y + 5);
    SelectObject(dc, previousBrush);
    SelectObject(dc, previousPen);
    DeleteObject(pen);
}

// A controller is active either because its hold key is down or because its
// toggle is latched; the overlay names which, so the operator can tell a stuck
// toggle from a held button at a glance.
[[nodiscard]] const wchar_t* controllerStateText(bool enabled, bool active, bool toggled) {
    if (!enabled) {
        return L"off";
    }
    if (toggled) {
        return active ? L"TOGGLE" : L"armed";
    }
    return active ? L"HELD" : L"armed";
}

[[nodiscard]] std::wstring snapshotText(const UiSnapshot& snapshot) {
    std::wostringstream output;
    output << (snapshot.enabled ? L"RUN" : L"PAUSED")
           << L" | trigger=" << controllerStateText(
                  snapshot.triggerEnabled, snapshot.triggerHeld, snapshot.triggerToggled)
           << L" track=" << controllerStateText(
                  snapshot.trackingEnabled, snapshot.trackingHeld, snapshot.trackingToggled)
           << L" | raw=" << (snapshot.rawDetected ? L"yes" : L"no")
           << L" stable=" << (snapshot.temporallyActive ? L"yes" : L"no")
           << L" pin=" << (snapshot.pinOccluded ? L"OCCLUDED" : (snapshot.pinVisible ? L"VISIBLE" : L"none"))
           << L" | confidence=" << std::fixed << std::setprecision(2) << snapshot.confidence
           << L" area=" << snapshot.componentArea
           << L" candidates=" << snapshot.candidateCount
           << L" matches=" << snapshot.matchedPixels
           << L" | processing=" << std::setprecision(2) << snapshot.processingMilliseconds << L" ms"
           << L" fps=" << std::setprecision(1) << snapshot.loopFps;
    if (snapshot.distance) {
        output << L" | range~" << std::setprecision(2) << *snapshot.distance;
    }
    if (snapshot.silhouetteTelemetry) {
        output << L"\nsilhouette: aspect=" << std::setprecision(2) << snapshot.aspectRatio
               << L" fill=" << snapshot.fillRatio
               << L" profile=" << snapshot.profileScore
               << L" fragments=" << snapshot.fragments
               << L" | rejected shape=" << snapshot.shapeRejections
               << L" profile=" << snapshot.profileRejections;
    }
    return output.str();
}

LRESULT CALLBACK overlayWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }

    auto* state = reinterpret_cast<SharedState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
        case kSnapshotMessage:
            InvalidateRect(window, nullptr, FALSE);
            return 0;

        case WM_HOTKEY:
            if (!state) {
                return 0;
            }
            if (wParam == kToggleHotkey) {
                const bool nowEnabled = !state->enabled.load(std::memory_order_relaxed);
                state->enabled.store(nowEnabled, std::memory_order_relaxed);
                MessageBeep(nowEnabled ? MB_OK : MB_ICONWARNING);
                return 0;
            }
            if (wParam == kExitHotkey) {
                DestroyWindow(window);
                return 0;
            }
            break;

        case WM_NCHITTEST:
            return HTTRANSPARENT;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

            if (state) {
                UiSnapshot snapshot;
                {
                    std::lock_guard lock(state->snapshotMutex);
                    snapshot = state->snapshot;
                }
                drawOutline(dc, snapshot.roi, RGB(40, 160, 255), 1);
                drawOutline(dc, snapshot.gate, RGB(255, 180, 20), 2);
                if (snapshot.candidate) {
                    drawOutline(
                        dc,
                        *snapshot.candidate,
                        snapshot.temporallyActive ? RGB(20, 255, 80) : RGB(255, 100, 20),
                        2);
                }
                if (snapshot.pinnedCandidate) {
                    drawOutline(
                        dc,
                        *snapshot.pinnedCandidate,
                        snapshot.pinOccluded ? RGB(180, 100, 255) : RGB(255, 40, 220),
                        3);
                }
                if (snapshot.cursorPoint && snapshot.pinnedPoint) {
                    drawTrackingLink(
                        dc,
                        *snapshot.cursorPoint,
                        *snapshot.pinnedPoint,
                        snapshot.pinOccluded ? RGB(180, 100, 255) : RGB(50, 255, 255));
                }

                const std::wstring status = snapshotText(snapshot);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(255, 255, 255));
                // DrawTextW rather than TextOutW: the telemetry is multi-line
                // once the silhouette gates are active.
                RECT textArea{16, 16, client.right - 16, client.bottom - 16};
                DrawTextW(
                    dc,
                    status.c_str(),
                    static_cast<int>(status.size()),
                    &textArea,
                    DT_LEFT | DT_TOP | DT_NOCLIP | DT_EXPANDTABS);
            }
            EndPaint(window, &paint);
            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(window);
            return 0;

        case WM_DESTROY:
            if (state) {
                state->stop.store(true, std::memory_order_relaxed);
            }
            UnregisterHotKey(window, kToggleHotkey);
            UnregisterHotKey(window, kExitHotkey);
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

[[nodiscard]] HWND createOverlayWindow(
    HINSTANCE instance,
    SharedState* state,
    int screenWidth,
    int screenHeight,
    bool visible) {
    const wchar_t* className = L"ColorDetectionEngineeringOverlayV2";
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = overlayWindowProcedure;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        throw std::runtime_error("RegisterClassW failed");
    }

    HWND window = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        className,
        L"Color Detection Engineering PoC",
        WS_POPUP,
        0,
        0,
        screenWidth,
        screenHeight,
        nullptr,
        nullptr,
        instance,
        state);
    if (!window) {
        throw std::runtime_error("CreateWindowExW failed");
    }
    if (!SetLayeredWindowAttributes(window, RGB(0, 0, 0), 0, LWA_COLORKEY)) {
        DestroyWindow(window);
        throw std::runtime_error("SetLayeredWindowAttributes failed");
    }
    SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE);

    bool hotkeysRegistered = true;
    if (!RegisterHotKey(window, kToggleHotkey, MOD_NOREPEAT, VK_F8)) {
        hotkeysRegistered = false;
        std::cerr << "Warning: F8 hotkey is already owned by another application.\n";
    }
    if (!RegisterHotKey(window, kExitHotkey, MOD_NOREPEAT, VK_F9)) {
        hotkeysRegistered = false;
        std::cerr << "Warning: F9 hotkey is already owned by another application.\n";
    }
    if (!hotkeysRegistered) {
        std::cerr << "Falling back to polled F8/F9 detection.\n";
    }
    state->hotkeysRegistered.store(hotkeysRegistered, std::memory_order_relaxed);
    if (visible) {
        ShowWindow(window, SW_SHOWNOACTIVATE);
        UpdateWindow(window);
    }
    return window;
}

// The executable is deliberately detector-only unless a mode flag is supplied,
// which makes an unflagged run look like four dead keybinds. Say so once, on
// the first press, rather than leaving the operator to guess.
void reportControllerDisabled(bool& alreadyWarned, const std::string& keyName, const char* flag) {
    if (alreadyWarned) {
        return;
    }
    alreadyWarned = true;
    std::cout << keyName << " was pressed, but " << flag
              << " was not supplied, so that controller is inactive.\n"
              << "Restart with " << flag << " to enable it. The overlay shows "
              << (std::string(flag) == "--trigger" ? "trigger=off" : "track=off")
              << " while it is disabled.\n";
}

void detectionLoop(
    const AppConfig config,
    SharedState* state,
    HWND overlayWindow,
    int screenWidth,
    int screenHeight) {
    try {
        const int roiWidth = std::min(config.roiWidth, screenWidth);
        const int roiHeight = std::min(config.roiHeight, screenHeight);
        const int roiLeft = (screenWidth - roiWidth) / 2;
        const int roiTop = (screenHeight - roiHeight) / 2;
        const RECT roi{roiLeft, roiTop, roiLeft + roiWidth, roiTop + roiHeight};

        const int gateWidth = std::min(config.detection.triggerGateSize, roiWidth);
        const int gateHeight = std::min(config.detection.triggerGateSize, roiHeight);
        const int gateLeft = roiLeft + (roiWidth - gateWidth) / 2;
        const int gateTop = roiTop + (roiHeight - gateHeight) / 2;
        const RECT gate{gateLeft, gateTop, gateLeft + gateWidth, gateTop + gateHeight};

        ScreenRoiCapture capture(roiWidth, roiHeight);
        std::optional<colorbot::PersistenceAccumulator> persistence;
        if (config.persistenceEnabled) {
            persistence.emplace(
                roiWidth, roiHeight, config.persistenceDecay, config.persistenceGain);
        }
        colorbot::TemporalGate temporalGate(config.temporal);
        colorbot::TargetTracker targetTracker(config.tracker);
        const auto framePeriod = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(1.0 / config.targetFps));
        auto nextFrame = Clock::now();
        auto previousFrame = nextFrame;
        double smoothedFps = 0.0;
        int frameCount = 0;
        bool previousDiagnosticCenterDetection = false;
        const bool pollHotkeys = !state->hotkeysRegistered.load(std::memory_order_relaxed);
        bool previousToggleKeyHeld = false;
        bool warnedAboutBlankCapture = false;
        bool triggerToggled = false;
        bool trackingToggled = false;
        bool previousTriggerToggleKeyHeld = false;
        bool previousTrackingToggleKeyHeld = false;
        bool warnedAboutDisabledTrigger = false;
        bool warnedAboutDisabledTracking = false;
        std::optional<Clock::time_point> pendingClickRelease;
        const POINT screenCenter{screenWidth / 2, screenHeight / 2};

        // A press that is never released leaves the left button stuck down for
        // the whole system, so every exit path from the loop must release it.
        const auto releasePendingClick = [&pendingClickRelease]() {
            if (!pendingClickRelease) {
                return;
            }
            if (!sendLeftButtonUp()) {
                std::cerr << "Warning: trigger release SendInput failed.\n";
            }
            pendingClickRelease.reset();
        };

        while (!state->stop.load(std::memory_order_relaxed)) {
            // Latching toggles are polled on every iteration, including while
            // paused, so their state never depends on when they were pressed.
            // They are polled even when their controller is disabled, so that
            // pressing one in detector-only mode explains itself instead of
            // appearing to be a broken keybind.
            {
                const bool held = isKeyHeld(config.triggerToggleVirtualKey);
                if (held && !previousTriggerToggleKeyHeld) {
                    if (!config.triggerEnabled) {
                        reportControllerDisabled(
                            warnedAboutDisabledTrigger,
                            virtualKeyName(config.triggerToggleVirtualKey),
                            "--trigger");
                    } else {
                        triggerToggled = !triggerToggled;
                        MessageBeep(triggerToggled ? MB_OK : MB_ICONWARNING);
                        if (!triggerToggled) {
                            temporalGate.reset();
                        }
                        if (config.diagnosticsEnabled) {
                            std::cout << "diagnostics trigger_toggle="
                                      << (triggerToggled ? "on" : "off") << '\n';
                        }
                    }
                }
                previousTriggerToggleKeyHeld = held;
            }
            {
                const bool held = isKeyHeld(config.trackingToggleVirtualKey);
                if (held && !previousTrackingToggleKeyHeld) {
                    if (!config.trackingEnabled) {
                        reportControllerDisabled(
                            warnedAboutDisabledTracking,
                            virtualKeyName(config.trackingToggleVirtualKey),
                            "--track");
                    } else {
                        trackingToggled = !trackingToggled;
                        MessageBeep(trackingToggled ? MB_OK : MB_ICONWARNING);
                        if (!trackingToggled) {
                            targetTracker.reset();
                        }
                        if (config.diagnosticsEnabled) {
                            std::cout << "diagnostics track_toggle="
                                      << (trackingToggled ? "on" : "off") << '\n';
                        }
                    }
                }
                previousTrackingToggleKeyHeld = held;
            }

            // The hold keys carry the same trap: without their mode flag they
            // are read but ignored.
            if (!config.triggerEnabled && isKeyHeld(config.triggerVirtualKey)) {
                reportControllerDisabled(
                    warnedAboutDisabledTrigger,
                    virtualKeyName(config.triggerVirtualKey),
                    "--trigger");
            }
            if (!config.trackingEnabled && isKeyHeld(config.trackingVirtualKey)) {
                reportControllerDisabled(
                    warnedAboutDisabledTracking,
                    virtualKeyName(config.trackingVirtualKey),
                    "--track");
            }

            if (pollHotkeys) {
                const bool toggleHeld = isKeyHeld(VK_F8);
                if (toggleHeld && !previousToggleKeyHeld) {
                    const bool nowEnabled = !state->enabled.load(std::memory_order_relaxed);
                    state->enabled.store(nowEnabled, std::memory_order_relaxed);
                    MessageBeep(nowEnabled ? MB_OK : MB_ICONWARNING);
                }
                previousToggleKeyHeld = toggleHeld;
                if (isKeyHeld(VK_F9)) {
                    state->stop.store(true, std::memory_order_relaxed);
                    PostMessageW(overlayWindow, WM_CLOSE, 0, 0);
                    break;
                }
            }

            const bool enabled = state->enabled.load(std::memory_order_relaxed);
            if (!enabled) {
                temporalGate.reset();
                targetTracker.reset();
                releasePendingClick();
                if (persistence) {
                    persistence->reset();
                }
                UiSnapshot snapshot;
                snapshot.roi = roi;
                snapshot.gate = gate;
                snapshot.enabled = false;
                snapshot.triggerEnabled = config.triggerEnabled;
                snapshot.trackingEnabled = config.trackingEnabled;
                snapshot.triggerToggled = triggerToggled;
                snapshot.trackingToggled = trackingToggled;
                {
                    std::lock_guard lock(state->snapshotMutex);
                    state->snapshot = snapshot;
                }
                PostMessageW(overlayWindow, kSnapshotMessage, 0, 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                nextFrame = Clock::now();
                previousFrame = nextFrame;
                continue;
            }

            const auto processingStart = Clock::now();
            if (!capture.capture(roiLeft, roiTop)) {
                throw std::runtime_error("BitBlt screen capture failed");
            }
            const colorbot::DetectionResult detection = colorbot::analyzeBgra(
                capture.pixels(),
                roiWidth,
                roiHeight,
                capture.stride(),
                config.targets,
                config.detection,
                persistence ? &*persistence : nullptr,
                config.persistenceThreshold);

            // GDI BitBlt cannot read an exclusive-fullscreen swap chain, and a
            // protected surface reads back as pure black. A ROI with zero
            // saturated pixels for an extended period means the detector is
            // looking at nothing, which is indistinguishable from "the hotkeys
            // do not work" from the operator's point of view.
            if (!warnedAboutBlankCapture && frameCount == config.targetFps * 2 &&
                detection.matchedPixels == 0 && detection.candidates.empty()) {
                warnedAboutBlankCapture = true;
                std::cerr << "Warning: no colored pixels have been captured for two seconds.\n"
                          << "If the target application is in exclusive fullscreen, switch it to\n"
                          << "borderless windowed; GDI capture cannot read an exclusive swap chain.\n";
            }

            if (config.diagnosticsEnabled && frameCount % config.targetFps == 0) {
                std::cout << "diagnostics"
                          << " candidates=" << detection.candidates.size()
                          << " center_detected=" << (detection.detected ? "yes" : "no")
                          << " matched_pixels=" << detection.matchedPixels << '\n';
            }
            if (config.diagnosticsEnabled &&
                detection.detected != previousDiagnosticCenterDetection) {
                std::cout << "diagnostics center_"
                          << (detection.detected ? "entered" : "exited") << '\n';
                previousDiagnosticCenterDetection = detection.detected;
            }
            // Either the hold key or the latching toggle activates a
            // controller, so both input styles stay available at once.
            const bool triggerKeyDown = isKeyHeld(config.triggerVirtualKey);
            const bool trackingKeyDown = isKeyHeld(config.trackingVirtualKey);
            const bool triggerHeld =
                config.triggerEnabled && (triggerKeyDown || triggerToggled);
            const bool trackingHeld =
                config.trackingEnabled && (trackingKeyDown || trackingToggled);

            colorbot::TemporalDecision decision;
            if (triggerHeld) {
                decision = temporalGate.update(detection.detected, processingStart);
            } else {
                temporalGate.reset();
            }
            const colorbot::TrackState trackState = targetTracker.update(
                detection.candidates,
                trackingHeld,
                (roiWidth - 1) / 2.0,
                (roiHeight - 1) / 2.0);

            std::optional<double> distance;
            const colorbot::Candidate* rangeCandidate = trackState.pinned
                ? &trackState.candidate
                : (detection.best ? &*detection.best : nullptr);
            const bool completeComponent = rangeCandidate &&
                rangeCandidate->box.left > 0 && rangeCandidate->box.top > 0 &&
                rangeCandidate->box.right < roiWidth - 1 &&
                rangeCandidate->box.bottom < roiHeight - 1;
            if (completeComponent && config.knownObjectHeight > 0.0) {
                distance = colorbot::estimateMonocularDistance(
                    config.knownObjectHeight,
                    config.verticalFovDegrees,
                    screenHeight,
                    rangeCandidate->box.height());
            }

            // Release a click whose hold time has elapsed. Doing this from the
            // loop rather than sleeping keeps the detector running during the
            // hold, so the button duration costs no detection frames.
            if (pendingClickRelease && Clock::now() >= *pendingClickRelease) {
                releasePendingClick();
            }

            if (decision.triggerEvent && !pendingClickRelease) {
                const bool clickSucceeded = sendLeftButtonDown();
                if (clickSucceeded) {
                    pendingClickRelease = Clock::now() +
                        std::chrono::milliseconds(config.clickHoldMilliseconds);
                }
                if (config.soundEnabled) {
                    MessageBeep(MB_OK);
                }
                std::cout << "confirmed detection"
                          << " confidence=" << std::fixed << std::setprecision(3)
                          << (detection.best ? detection.best->confidence : 0.0)
                          << " area=" << (detection.best ? detection.best->area : 0);
                if (distance) {
                    std::cout << " estimated_range=" << std::setprecision(2) << *distance;
                }
                std::cout << " trigger_click=" << (clickSucceeded ? "sent" : "failed");
                std::cout << '\n';
            }

            POINT cursor{};
            const bool cursorAvailable = GetCursorPos(&cursor) != FALSE;
            if (trackState.newlyPinned) {
                std::cout << "target pinned"
                          << " confidence=" << std::fixed << std::setprecision(3)
                          << trackState.candidate.confidence
                          << " area=" << trackState.candidate.area << '\n';
            }
            // Aim from the crosshair by default. Cursor mode remains available
            // for desktop demonstrations, where the visible pointer is the
            // thing being steered.
            const POINT aimReference = config.aimAtScreenCenter ? screenCenter : cursor;
            const bool referenceAvailable = config.aimAtScreenCenter || cursorAvailable;
            if (trackState.pinned && trackState.visible && referenceAvailable) {
                const double targetScreenX = roiLeft + trackState.smoothedX;
                const double targetScreenY = roiTop + trackState.smoothedY;
                if (!moveCursorToward(
                        aimReference,
                        targetScreenX,
                        targetScreenY,
                        config.trackingGain,
                        config.trackingDeadZonePixels,
                        config.trackingMaximumStepPixels)) {
                    std::cerr << "Warning: tracking movement SendInput failed.\n";
                }
            }

            const auto processingEnd = Clock::now();
            const double processingMilliseconds =
                std::chrono::duration<double, std::milli>(processingEnd - processingStart).count();
            const double frameSeconds = std::chrono::duration<double>(processingEnd - previousFrame).count();
            previousFrame = processingEnd;
            if (frameSeconds > 0.0) {
                const double instantaneousFps = 1.0 / frameSeconds;
                smoothedFps = smoothedFps == 0.0
                    ? instantaneousFps
                    : 0.90 * smoothedFps + 0.10 * instantaneousFps;
            }

            UiSnapshot snapshot;
            snapshot.roi = roi;
            snapshot.gate = gate;
            snapshot.enabled = true;
            snapshot.rawDetected = detection.detected;
            snapshot.temporallyActive = decision.active;
            snapshot.triggerEnabled = config.triggerEnabled;
            snapshot.trackingEnabled = config.trackingEnabled;
            snapshot.triggerHeld = triggerHeld;
            snapshot.trackingHeld = trackingHeld;
            snapshot.triggerToggled = triggerToggled;
            snapshot.trackingToggled = trackingToggled;
            snapshot.pinVisible = trackState.pinned && trackState.visible;
            snapshot.pinOccluded = trackState.pinned && !trackState.visible;
            snapshot.matchedPixels = detection.matchedPixels;
            snapshot.candidateCount = static_cast<int>(detection.candidates.size());
            snapshot.processingMilliseconds = processingMilliseconds;
            snapshot.loopFps = smoothedFps;
            snapshot.distance = distance;
            if (trackState.pinned) {
                snapshot.pinnedCandidate = candidateToScreenRect(trackState.candidate, roi);
                snapshot.pinnedPoint = POINT{
                    static_cast<LONG>(std::lround(roiLeft + trackState.smoothedX)),
                    static_cast<LONG>(std::lround(roiTop + trackState.smoothedY))};
                // Draw the aim line from whatever the controller is actually
                // correcting toward, so the overlay cannot disagree with the
                // control law.
                if (referenceAvailable) {
                    snapshot.cursorPoint = aimReference;
                }
                snapshot.componentArea = trackState.candidate.area;
                snapshot.confidence = trackState.candidate.confidence;
                snapshot.fragments = trackState.candidate.fragments;
                snapshot.profileScore = trackState.candidate.profileScore;
                snapshot.aspectRatio = trackState.candidate.aspectRatio;
                snapshot.fillRatio = trackState.candidate.fillRatio;
            } else if (detection.best) {
                snapshot.candidate = candidateToScreenRect(*detection.best, roi);
                snapshot.componentArea = detection.best->area;
                snapshot.confidence = detection.best->confidence;
                snapshot.fragments = detection.best->fragments;
                snapshot.profileScore = detection.best->profileScore;
                snapshot.aspectRatio = detection.best->aspectRatio;
                snapshot.fillRatio = detection.best->fillRatio;
            }
            snapshot.shapeRejections = detection.shapeRejections;
            snapshot.profileRejections = detection.profileRejections;
            snapshot.silhouetteTelemetry =
                config.detection.shapeGateEnabled || config.detection.profileGateEnabled ||
                config.detection.mergeGapPixels > 0 || config.detection.morphologyRadius > 0;
            {
                std::lock_guard lock(state->snapshotMutex);
                state->snapshot = snapshot;
            }
            PostMessageW(overlayWindow, kSnapshotMessage, 0, 0);

            ++frameCount;
            if (config.maxFrames > 0 && frameCount >= config.maxFrames) {
                PostMessageW(overlayWindow, WM_CLOSE, 0, 0);
                break;
            }

            nextFrame += framePeriod;
            const auto now = Clock::now();
            if (nextFrame < now - framePeriod) {
                nextFrame = now;
            }
            std::this_thread::sleep_until(nextFrame);
        }
        releasePendingClick();
    } catch (const std::exception& error) {
        std::cerr << "Detection worker stopped: " << error.what() << '\n';
        state->stop.store(true, std::memory_order_relaxed);
        PostMessageW(overlayWindow, WM_CLOSE, 0, 0);
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    // Launching with no arguments is the double-click case, where there is no
    // opportunity to pass flags. Treat it as "run the thing": enable both
    // controllers and match the target's integrity level, which is what the
    // hold keys and toggles need in order to work at all.
    const bool launchedWithoutArguments = argc <= 1;

    try {
        bool helpRequested = false;
        AppConfig config = parseArguments(argc, argv, helpRequested);
        if (helpRequested) {
            printHelp();
            return 0;
        }

        if (launchedWithoutArguments) {
            config.triggerEnabled = true;
            config.trackingEnabled = true;
            config.elevateRequested = true;
        }
        if (config.detectorOnlyRequested) {
            config.triggerEnabled = false;
            config.trackingEnabled = false;
        }

        if (config.elevateRequested && !isProcessElevated()) {
            if (relaunchElevated(argc, argv)) {
                return 0;
            }
            // An explicit --elevate is a hard requirement. The implicit
            // double-click case falls through instead, because an unelevated
            // detector still works against every target that is not itself
            // elevated, and refusing to start would be worse than degrading.
            if (!launchedWithoutArguments) {
                return 1;
            }
            std::cout << "Continuing without elevation. The hold keys and toggles will not "
                         "work while an\napplication running as administrator holds the "
                         "foreground.\n\n";
        }

        SetProcessDPIAware();

        if (config.keyTestRequested) {
            runKeyTest(config);
            return 0;
        }

        const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
        if (screenWidth <= 0 || screenHeight <= 0) {
            throw std::runtime_error("Could not determine primary-screen dimensions");
        }

        std::cout << "Color Detection Engineering PoC\n"
                  << "screen=" << screenWidth << 'x' << screenHeight
                  << " roi=" << config.roiWidth << 'x' << config.roiHeight
                  << " target_fps=" << config.targetFps
                  << " trigger=" << (config.triggerEnabled ? "enabled" : "disabled")
                  << " tracking=" << (config.trackingEnabled ? "enabled" : "disabled") << '\n';
        if (config.triggerEnabled) {
            std::cout << "Hold " << virtualKeyName(config.triggerVirtualKey)
                      << " for center-gated trigger mode, or press "
                      << virtualKeyName(config.triggerToggleVirtualKey) << " to latch it on.\n";
        }
        if (config.trackingEnabled) {
            std::cout << "Hold " << virtualKeyName(config.trackingVirtualKey)
                      << " to pin/follow the closest target, or press "
                      << virtualKeyName(config.trackingToggleVirtualKey) << " to latch it on.\n";
        }
        if (launchedWithoutArguments) {
            std::cout << "Started with no arguments, so both controllers are enabled.\n"
                      << "Pass --detector-only to observe without actuating anything.\n";
        }
        if (!config.triggerEnabled && !config.trackingEnabled) {
            std::cout << "Detector-only mode: no keys are active. "
                      << virtualKeyName(config.triggerVirtualKey) << ", "
                      << virtualKeyName(config.trackingVirtualKey) << ", "
                      << virtualKeyName(config.triggerToggleVirtualKey) << " and "
                      << virtualKeyName(config.trackingToggleVirtualKey)
                      << " will do nothing\nuntil you add --trigger, --track, or both.\n";
        }
        std::cout << "F8 pauses/resumes; F9 exits.\n";
        if ((config.triggerEnabled || config.trackingEnabled) && !isProcessElevated()) {
            std::cout << "Note: this process is not elevated. Clicks and cursor movement sent to "
                         "an\napplication running as administrator will be discarded by Windows, "
                         "and the hold\nkeys will read as never pressed. Restart with --elevate "
                         "if the target runs\nas administrator; run --keytest to confirm what "
                         "this process can observe.\n";
        }
        if (config.knownObjectHeight > 0.0) {
            std::cout << "Range estimation enabled: known_height=" << config.knownObjectHeight
                      << " vfov=" << config.verticalFovDegrees << " degrees\n";
        }

        SharedState state;
        HINSTANCE instance = GetModuleHandleW(nullptr);
        HWND overlay = createOverlayWindow(
            instance,
            &state,
            screenWidth,
            screenHeight,
            config.overlayEnabled);

        std::thread worker(
            detectionLoop,
            config,
            &state,
            overlay,
            screenWidth,
            screenHeight);

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        state.stop.store(true, std::memory_order_relaxed);
        if (worker.joinable()) {
            worker.join();
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        std::cerr << "Use --help to see valid options.\n";
        // A double-clicked console window closes the instant main returns, so
        // the message above would never be readable.
        if (launchedWithoutArguments) {
            std::cerr << "\nPress Enter to close.\n";
            std::cin.get();
        }
        return 1;
    }
}
