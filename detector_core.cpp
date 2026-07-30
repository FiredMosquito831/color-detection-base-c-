#include "detector_core.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace colorbot {
namespace {

[[nodiscard]] const std::array<double, 256>& linearRgbTable() {
    static const std::array<double, 256> table = [] {
        std::array<double, 256> values{};
        for (std::size_t index = 0; index < values.size(); ++index) {
            const double value = static_cast<double>(index) / 255.0;
            values[index] = value <= 0.04045
                ? value / 12.92
                : std::pow((value + 0.055) / 1.055, 2.4);
        }
        return values;
    }();
    return table;
}

[[nodiscard]] double labPivot(double value) {
    constexpr double epsilon = 216.0 / 24389.0;
    constexpr double kappa = 24389.0 / 27.0;
    return value > epsilon ? std::cbrt(value) : (kappa * value + 16.0) / 116.0;
}

[[nodiscard]] int saturation255(Rgb rgb) {
    const int maximum = std::max({static_cast<int>(rgb.r), static_cast<int>(rgb.g), static_cast<int>(rgb.b)});
    const int minimum = std::min({static_cast<int>(rgb.r), static_cast<int>(rgb.g), static_cast<int>(rgb.b)});
    if (maximum == 0) {
        return 0;
    }
    return static_cast<int>(std::lround(255.0 * static_cast<double>(maximum - minimum) / maximum));
}

[[nodiscard]] double clamp01(double value) {
    return std::clamp(value, 0.0, 1.0);
}

[[nodiscard]] int luma255(Rgb rgb) {
    return static_cast<int>(std::lround(
        0.2126 * rgb.r + 0.7152 * rgb.g + 0.0722 * rgb.b));
}

// One-dimensional sliding-window extremum along x, then along y. A square
// structuring element is separable, so a radius-r dilation or erosion costs
// two linear passes instead of one quadratic one.
void morphologicalPass(
    std::vector<std::uint8_t>& mask,
    int width,
    int height,
    int radius,
    bool dilate) {
    if (radius <= 0) {
        return;
    }
    const std::uint8_t neutral = dilate ? 0 : 1;
    std::vector<std::uint8_t> scratch(mask.size(), neutral);

    for (int y = 0; y < height; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * width;
        for (int x = 0; x < width; ++x) {
            std::uint8_t value = neutral;
            const int from = std::max(0, x - radius);
            const int to = std::min(width - 1, x + radius);
            for (int sample = from; sample <= to; ++sample) {
                const std::uint8_t current = mask[row + static_cast<std::size_t>(sample)];
                value = dilate ? std::max(value, current) : std::min(value, current);
            }
            scratch[row + static_cast<std::size_t>(x)] = value;
        }
    }

    for (int x = 0; x < width; ++x) {
        for (int y = 0; y < height; ++y) {
            std::uint8_t value = neutral;
            const int from = std::max(0, y - radius);
            const int to = std::min(height - 1, y + radius);
            for (int sample = from; sample <= to; ++sample) {
                const std::uint8_t current =
                    scratch[static_cast<std::size_t>(sample) * width + static_cast<std::size_t>(x)];
                value = dilate ? std::max(value, current) : std::min(value, current);
            }
            mask[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)] = value;
        }
    }
}

// Closing is dilation followed by erosion. It fills gaps narrower than the
// structuring element while leaving the outer silhouette dimensions intact,
// which is exactly the branch/wire/mullion case.
void morphologicalClose(
    std::vector<std::uint8_t>& mask,
    int width,
    int height,
    int radius) {
    morphologicalPass(mask, width, height, radius, true);
    morphologicalPass(mask, width, height, radius, false);
}

// Fraction by which the middle band of a silhouette is wider than its top
// band. An upright human has a narrow head over broad shoulders and scores
// high; a rectangle or a round blob scores near zero.
[[nodiscard]] double verticalProfileScore(
    const std::vector<int>& labels,
    int label,
    int width,
    const BoundingBox& box) {
    const int boxHeight = box.height();
    const int boxWidth = box.width();
    if (boxHeight < 4 || boxWidth <= 0) {
        return 0.0;
    }

    // A standing human's head spans roughly the top eighth of their height,
    // and the shoulders-to-hips span sits between one third and two thirds.
    const int topEnd = box.top + std::max(1, boxHeight / 8);
    const int middleStart = box.top + boxHeight / 3;
    const int middleEnd = box.top + std::max(boxHeight / 3 + 1, (2 * boxHeight) / 3);

    int topLeft = width;
    int topRight = -1;
    int middleLeft = width;
    int middleRight = -1;

    for (int y = box.top; y <= box.bottom; ++y) {
        const bool inTop = y < topEnd;
        const bool inMiddle = y >= middleStart && y < middleEnd;
        if (!inTop && !inMiddle) {
            continue;
        }
        for (int x = box.left; x <= box.right; ++x) {
            if (labels[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)] != label) {
                continue;
            }
            if (inTop) {
                topLeft = std::min(topLeft, x);
                topRight = std::max(topRight, x);
            }
            if (inMiddle) {
                middleLeft = std::min(middleLeft, x);
                middleRight = std::max(middleRight, x);
            }
        }
    }

    if (topRight < 0 || middleRight < 0) {
        return 0.0;
    }
    const double topWidth = topRight - topLeft + 1;
    const double middleWidth = middleRight - middleLeft + 1;
    if (middleWidth <= 0.0) {
        return 0.0;
    }
    return clamp01((middleWidth - topWidth) / middleWidth);
}

// Disjoint-set over component indices, used by proximity merging.
[[nodiscard]] int findRoot(std::vector<int>& parent, int node) {
    while (parent[static_cast<std::size_t>(node)] != node) {
        parent[static_cast<std::size_t>(node)] =
            parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(node)])];
        node = parent[static_cast<std::size_t>(node)];
    }
    return node;
}

void unionNodes(std::vector<int>& parent, int first, int second) {
    const int a = findRoot(parent, first);
    const int b = findRoot(parent, second);
    if (a != b) {
        parent[static_cast<std::size_t>(b)] = a;
    }
}

// Raw accumulation for one labeled component, before merging and scoring.
struct ComponentAccumulator {
    BoundingBox box{};
    int area{};
    int gateHits{};
    int scoredPixels{};
    double sumX{};
    double sumY{};
    double sumDeltaE{};
    int label{};
};

}  // namespace

Lab rgbToLab(Rgb rgb) {
    const auto& linear = linearRgbTable();
    const double r = linear[rgb.r];
    const double g = linear[rgb.g];
    const double b = linear[rgb.b];

    // sRGB, D65 reference white.
    const double x = (0.4124564 * r + 0.3575761 * g + 0.1804375 * b) / 0.95047;
    const double y = (0.2126729 * r + 0.7151522 * g + 0.0721750 * b);
    const double z = (0.0193339 * r + 0.1191920 * g + 0.9503041 * b) / 1.08883;

    const double fx = labPivot(x);
    const double fy = labPivot(y);
    const double fz = labPivot(z);
    return Lab{116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

double deltaE76(const Lab& first, const Lab& second) {
    const double dl = first.l - second.l;
    const double da = first.a - second.a;
    const double db = first.b - second.b;
    return std::sqrt(dl * dl + da * da + db * db);
}

ColorPrototype makePrototype(std::string name, Rgb rgb) {
    return ColorPrototype{std::move(name), rgb, rgbToLab(rgb)};
}

DetectionResult analyzeBgra(
    const std::uint8_t* pixels,
    int width,
    int height,
    int strideBytes,
    const std::vector<ColorPrototype>& prototypes,
    const DetectionConfig& config,
    PersistenceAccumulator* persistence,
    double persistenceThreshold) {
    if (pixels == nullptr || width <= 0 || height <= 0 || strideBytes < width * 4) {
        throw std::invalid_argument("Invalid BGRA image dimensions or stride");
    }
    if (prototypes.empty()) {
        throw std::invalid_argument("At least one color prototype is required");
    }
    if (config.maxDeltaE <= 0.0 || config.minimumComponentPixels <= 0 ||
        config.minimumGatePixels <= 0 || config.triggerGateSize <= 0 ||
        config.maximumComponentFraction <= 0.0 || config.maximumComponentFraction > 1.0) {
        throw std::invalid_argument("Detection thresholds must be positive");
    }

    const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
    std::vector<std::uint8_t> mask(pixelCount, 0);
    std::vector<double> distances(pixelCount, std::numeric_limits<double>::infinity());

    DetectionResult result;

    if (config.thermalMode == ThermalMode::Off) {
        for (int y = 0; y < height; ++y) {
            const auto* row = pixels + static_cast<std::size_t>(y) * strideBytes;
            for (int x = 0; x < width; ++x) {
                const auto* bgra = row + static_cast<std::size_t>(x) * 4;
                const Rgb rgb{bgra[2], bgra[1], bgra[0]};
                if (saturation255(rgb) < config.minimumSaturation) {
                    continue;
                }

                const Lab lab = rgbToLab(rgb);
                double bestDistance = std::numeric_limits<double>::infinity();
                for (const auto& prototype : prototypes) {
                    bestDistance = std::min(bestDistance, deltaE76(lab, prototype.lab));
                }

                const std::size_t index = static_cast<std::size_t>(y) * width + x;
                distances[index] = bestDistance;
                if (bestDistance <= config.maxDeltaE) {
                    mask[index] = 1;
                    ++result.matchedPixels;
                }
            }
        }
    } else {
        // Thermal segmentation. Intensity carries the signal, so the target is
        // whatever is far enough above (white-hot) or below (black-hot) the
        // threshold, optionally also standing out from its local surroundings.
        const bool whiteHot = config.thermalMode == ThermalMode::WhiteHot;
        std::vector<int> intensity(pixelCount, 0);
        for (int y = 0; y < height; ++y) {
            const auto* row = pixels + static_cast<std::size_t>(y) * strideBytes;
            for (int x = 0; x < width; ++x) {
                const auto* bgra = row + static_cast<std::size_t>(x) * 4;
                const int value = luma255(Rgb{bgra[2], bgra[1], bgra[0]});
                // Normalize so that "hotter" always means "larger".
                intensity[static_cast<std::size_t>(y) * width + x] = whiteHot ? value : 255 - value;
            }
        }

        // Summed-area table so the local mean over any window costs four reads
        // regardless of radius.
        std::vector<std::int64_t> integral((static_cast<std::size_t>(width) + 1) *
                                           (static_cast<std::size_t>(height) + 1), 0);
        const int integralStride = width + 1;
        for (int y = 0; y < height; ++y) {
            std::int64_t rowSum = 0;
            for (int x = 0; x < width; ++x) {
                rowSum += intensity[static_cast<std::size_t>(y) * width + x];
                integral[static_cast<std::size_t>(y + 1) * integralStride + (x + 1)] =
                    integral[static_cast<std::size_t>(y) * integralStride + (x + 1)] + rowSum;
            }
        }

        const int radius = std::max(1, config.thermalLocalRadius);
        const int threshold = whiteHot
            ? config.thermalThreshold
            : 255 - config.thermalThreshold;

        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const std::size_t index = static_cast<std::size_t>(y) * width + x;
                const int value = intensity[index];
                if (value < threshold) {
                    continue;
                }

                if (config.thermalLocalContrast > 0) {
                    const int left = std::max(0, x - radius);
                    const int top = std::max(0, y - radius);
                    const int right = std::min(width, x + radius + 1);
                    const int bottom = std::min(height, y + radius + 1);
                    const std::int64_t sum =
                        integral[static_cast<std::size_t>(bottom) * integralStride + right] -
                        integral[static_cast<std::size_t>(top) * integralStride + right] -
                        integral[static_cast<std::size_t>(bottom) * integralStride + left] +
                        integral[static_cast<std::size_t>(top) * integralStride + left];
                    const std::int64_t count =
                        static_cast<std::int64_t>(right - left) * (bottom - top);
                    const double localMean =
                        count > 0 ? static_cast<double>(sum) / static_cast<double>(count) : 0.0;
                    if (value - localMean < config.thermalLocalContrast) {
                        continue;
                    }
                }

                // Express thermal margin on the same scale the color path uses
                // so that the downstream confidence model needs no special case:
                // a pixel at the threshold is a worst-case match, a saturated
                // pixel is a perfect one.
                const double margin = static_cast<double>(value - threshold) /
                    std::max(1.0, 255.0 - static_cast<double>(threshold));
                distances[index] = config.maxDeltaE * (1.0 - clamp01(margin));
                mask[index] = 1;
                ++result.matchedPixels;
            }
        }
    }

    // Fold in evidence from recent frames. A silhouette glimpsed through
    // shifting foliage is never whole in any single frame, but the decayed
    // union across a short window is.
    if (persistence != nullptr) {
        if (persistence->width() != width || persistence->height() != height) {
            throw std::invalid_argument("Persistence accumulator size does not match the frame");
        }
        persistence->apply(mask.data(), persistenceThreshold);
    }

    // Bridge thin occluders before the mask is broken into components.
    if (config.morphologyRadius > 0) {
        morphologicalClose(mask, width, height, config.morphologyRadius);
    }

    const int gateWidth = std::min(config.triggerGateSize, width);
    const int gateHeight = std::min(config.triggerGateSize, height);
    const int gateLeft = (width - gateWidth) / 2;
    const int gateTop = (height - gateHeight) / 2;
    const int gateRight = gateLeft + gateWidth;
    const int gateBottom = gateTop + gateHeight;

    std::vector<int> labels(pixelCount, -1);
    std::vector<int> stack;
    stack.reserve(pixelCount / 8 + 1);
    std::vector<ComponentAccumulator> components;

    const double frameCenterX = (width - 1) / 2.0;
    const double frameCenterY = (height - 1) / 2.0;
    const double maximumCenterDistance = std::hypot(frameCenterX, frameCenterY);

    for (int seedY = 0; seedY < height; ++seedY) {
        for (int seedX = 0; seedX < width; ++seedX) {
            const std::size_t seedIndex = static_cast<std::size_t>(seedY) * width + seedX;
            if (!mask[seedIndex] || labels[seedIndex] >= 0) {
                continue;
            }

            const int label = static_cast<int>(components.size());
            ComponentAccumulator component;
            component.label = label;
            component.box = BoundingBox{seedX, seedY, seedX, seedY};

            stack.clear();
            stack.push_back(static_cast<int>(seedIndex));
            labels[seedIndex] = label;

            while (!stack.empty()) {
                const int index = stack.back();
                stack.pop_back();
                const int x = index % width;
                const int y = index / width;

                ++component.area;
                component.sumX += x;
                component.sumY += y;
                // Pixels introduced by morphological closing never matched, so
                // they carry no color evidence and must not dilute the mean.
                const double distance = distances[static_cast<std::size_t>(index)];
                if (distance != std::numeric_limits<double>::infinity()) {
                    component.sumDeltaE += distance;
                    ++component.scoredPixels;
                }
                component.box.left = std::min(component.box.left, x);
                component.box.top = std::min(component.box.top, y);
                component.box.right = std::max(component.box.right, x);
                component.box.bottom = std::max(component.box.bottom, y);
                if (x >= gateLeft && x < gateRight && y >= gateTop && y < gateBottom) {
                    ++component.gateHits;
                }

                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) {
                            continue;
                        }
                        const int nx = x + dx;
                        const int ny = y + dy;
                        if (nx < 0 || nx >= width || ny < 0 || ny >= height) {
                            continue;
                        }
                        const std::size_t neighborIndex =
                            static_cast<std::size_t>(ny) * width + static_cast<std::size_t>(nx);
                        if (mask[neighborIndex] && labels[neighborIndex] < 0) {
                            labels[neighborIndex] = label;
                            stack.push_back(ny * width + nx);
                        }
                    }
                }
            }

            components.push_back(component);
        }
    }

    // Reassemble targets an occluder split into several components. Only
    // horizontally overlapping neighbors are joined, so a person behind a
    // railing merges vertically while two people standing side by side do not.
    std::vector<int> parent(components.size());
    for (std::size_t index = 0; index < parent.size(); ++index) {
        parent[index] = static_cast<int>(index);
    }
    if (config.mergeGapPixels > 0) {
        const int gap = config.mergeGapPixels;
        for (std::size_t first = 0; first < components.size(); ++first) {
            for (std::size_t second = first + 1; second < components.size(); ++second) {
                const BoundingBox& a = components[first].box;
                const BoundingBox& b = components[second].box;
                const bool horizontallyOverlapping =
                    a.left <= b.right + gap && b.left <= a.right + gap;
                const bool verticallyClose =
                    a.top <= b.bottom + gap && b.top <= a.bottom + gap;
                if (!horizontallyOverlapping || !verticallyClose) {
                    continue;
                }
                // Require genuine column overlap, not just proximity, so that
                // side-by-side targets stay distinct.
                const int overlap = std::min(a.right, b.right) - std::max(a.left, b.left) + 1;
                const int narrower = std::min(a.width(), b.width());
                if (overlap < std::max(1, narrower / 4)) {
                    continue;
                }
                unionNodes(parent, static_cast<int>(first), static_cast<int>(second));
            }
        }
    }

    // Fold every component into its merge root.
    std::vector<ComponentAccumulator> merged;
    std::vector<int> rootToMerged(components.size(), -1);
    std::vector<int> fragmentCounts;
    std::vector<std::vector<int>> mergedLabels;
    for (std::size_t index = 0; index < components.size(); ++index) {
        const int root = findRoot(parent, static_cast<int>(index));
        int slot = rootToMerged[static_cast<std::size_t>(root)];
        if (slot < 0) {
            slot = static_cast<int>(merged.size());
            rootToMerged[static_cast<std::size_t>(root)] = slot;
            merged.push_back(components[index]);
            fragmentCounts.push_back(1);
            mergedLabels.push_back({components[index].label});
            continue;
        }
        ComponentAccumulator& target = merged[static_cast<std::size_t>(slot)];
        const ComponentAccumulator& source = components[index];
        target.box.left = std::min(target.box.left, source.box.left);
        target.box.top = std::min(target.box.top, source.box.top);
        target.box.right = std::max(target.box.right, source.box.right);
        target.box.bottom = std::max(target.box.bottom, source.box.bottom);
        target.area += source.area;
        target.gateHits += source.gateHits;
        target.scoredPixels += source.scoredPixels;
        target.sumX += source.sumX;
        target.sumY += source.sumY;
        target.sumDeltaE += source.sumDeltaE;
        ++fragmentCounts[static_cast<std::size_t>(slot)];
        mergedLabels[static_cast<std::size_t>(slot)].push_back(source.label);
    }

    for (std::size_t index = 0; index < merged.size(); ++index) {
        const ComponentAccumulator& component = merged[index];
        if (component.area < config.minimumComponentPixels ||
            static_cast<double>(component.area) / static_cast<double>(pixelCount) >
                config.maximumComponentFraction) {
            continue;
        }

        Candidate candidate;
        candidate.box = component.box;
        candidate.area = component.area;
        candidate.gateHits = component.gateHits;
        candidate.centroidX = component.sumX / component.area;
        candidate.centroidY = component.sumY / component.area;
        candidate.meanDeltaE = component.scoredPixels > 0
            ? component.sumDeltaE / component.scoredPixels
            : config.maxDeltaE;
        candidate.fragments = fragmentCounts[index];
        if (candidate.fragments > 1) {
            ++result.mergedComponents;
        }

        const double boxWidth = candidate.box.width();
        const double boxHeight = candidate.box.height();
        candidate.aspectRatio = boxHeight > 0.0 ? boxWidth / boxHeight : 0.0;
        candidate.fillRatio = (boxWidth * boxHeight) > 0.0
            ? static_cast<double>(candidate.area) / (boxWidth * boxHeight)
            : 0.0;

        if (config.shapeGateEnabled) {
            if (candidate.aspectRatio < config.minimumAspectRatio ||
                candidate.aspectRatio > config.maximumAspectRatio ||
                candidate.fillRatio < config.minimumFillRatio ||
                candidate.fillRatio > config.maximumFillRatio) {
                ++result.shapeRejections;
                continue;
            }
        }

        if (config.profileGateEnabled || config.shapeGateEnabled) {
            // Score against the largest constituent piece; merged fragments
            // share a bounding box but are separate labels.
            double bestProfile = 0.0;
            for (const int label : mergedLabels[index]) {
                bestProfile = std::max(
                    bestProfile, verticalProfileScore(labels, label, width, candidate.box));
            }
            candidate.profileScore = bestProfile;
        }
        if (config.profileGateEnabled && candidate.profileScore < config.minimumProfileScore) {
            ++result.profileRejections;
            continue;
        }

        const double colorScore = clamp01(1.0 - candidate.meanDeltaE / config.maxDeltaE);
        const double areaDenominator = std::max(1, config.minimumComponentPixels * 6);
        const double areaScore = clamp01(static_cast<double>(candidate.area) / areaDenominator);
        const double gateScore = clamp01(
            static_cast<double>(candidate.gateHits) / std::max(1, config.minimumGatePixels * 2));
        const double centerDistance = std::hypot(
            candidate.centroidX - frameCenterX,
            candidate.centroidY - frameCenterY);
        const double centerScore = maximumCenterDistance > 0.0
            ? clamp01(1.0 - centerDistance / maximumCenterDistance)
            : 1.0;
        candidate.confidence =
            0.45 * colorScore + 0.25 * areaScore + 0.20 * gateScore + 0.10 * centerScore;

        // Silhouette evidence contributes to the score only when the operator
        // asked for it, so default behavior and its thresholds are unchanged.
        if (config.shapeGateEnabled || config.profileGateEnabled) {
            candidate.confidence = 0.80 * candidate.confidence + 0.20 * candidate.profileScore;
        }

        ++result.acceptedComponents;
        if (candidate.confidence >= config.minimumConfidence) {
            result.candidates.push_back(candidate);
            if (candidate.gateHits >= config.minimumGatePixels &&
                (!result.best || candidate.confidence > result.best->confidence)) {
                result.best = candidate;
            }
        }
    }

    result.detected = result.best.has_value();
    return result;
}

PersistenceAccumulator::PersistenceAccumulator(int width, int height, double decay, double gain)
    : width_(width), height_(height), decay_(decay), gain_(gain) {
    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("Persistence accumulator requires positive dimensions");
    }
    if (decay < 0.0 || decay >= 1.0 || gain <= 0.0) {
        throw std::invalid_argument("Persistence decay must be in [0,1) and gain positive");
    }
    evidence_.assign(static_cast<std::size_t>(width) * height, 0.0);
}

void PersistenceAccumulator::apply(std::uint8_t* mask, double threshold) {
    if (mask == nullptr) {
        throw std::invalid_argument("Persistence accumulator requires a mask");
    }
    for (std::size_t index = 0; index < evidence_.size(); ++index) {
        double value = evidence_[index] * decay_;
        if (mask[index]) {
            value += gain_;
        }
        value = std::min(value, 1.0);
        evidence_[index] = value;
        if (value >= threshold) {
            mask[index] = 1;
        }
    }
}

void PersistenceAccumulator::reset() {
    std::fill(evidence_.begin(), evidence_.end(), 0.0);
}

std::optional<double> estimateMonocularDistance(
    double knownObjectHeight,
    double verticalFovDegrees,
    int fullFrameHeightPixels,
    int observedObjectHeightPixels) {
    if (knownObjectHeight <= 0.0 || verticalFovDegrees <= 0.0 || verticalFovDegrees >= 179.0 ||
        fullFrameHeightPixels <= 0 || observedObjectHeightPixels <= 0) {
        return std::nullopt;
    }

    constexpr double pi = 3.14159265358979323846;
    const double halfFovRadians = verticalFovDegrees * pi / 360.0;
    const double focalLengthPixels = fullFrameHeightPixels / (2.0 * std::tan(halfFovRadians));
    return knownObjectHeight * focalLengthPixels / observedObjectHeightPixels;
}

TemporalGate::TemporalGate(TemporalConfig config) : config_(config) {
    if (config_.confirmationFrames <= 0 || config_.releaseFrames <= 0 ||
        config_.retriggerCooldown.count() < 0) {
        throw std::invalid_argument("Invalid temporal gate configuration");
    }
}

TemporalDecision TemporalGate::update(
    bool rawDetection,
    std::chrono::steady_clock::time_point timestamp) {
    bool triggerEvent = false;

    if (rawDetection) {
        missStreak_ = 0;
        if (!active_) {
            ++hitStreak_;
            if (hitStreak_ >= config_.confirmationFrames) {
                active_ = true;
                const bool cooldownExpired = !lastTrigger_ ||
                    timestamp - *lastTrigger_ >= config_.retriggerCooldown;
                if (cooldownExpired) {
                    triggerEvent = true;
                    lastTrigger_ = timestamp;
                }
            }
        }
    } else {
        hitStreak_ = 0;
        if (active_) {
            ++missStreak_;
            if (missStreak_ >= config_.releaseFrames) {
                active_ = false;
                missStreak_ = 0;
            }
        }
    }

    return TemporalDecision{active_, triggerEvent, hitStreak_, missStreak_};
}

void TemporalGate::reset() {
    active_ = false;
    hitStreak_ = 0;
    missStreak_ = 0;
    lastTrigger_.reset();
}

TargetTracker::TargetTracker(TrackerConfig config) : config_(config) {
    if (config_.maximumMissedFrames < 0 || config_.reacquireRadiusPixels <= 0.0 ||
        config_.smoothingFactor <= 0.0 || config_.smoothingFactor > 1.0 ||
        config_.confidencePenaltyPixels < 0.0) {
        throw std::invalid_argument("Invalid target tracker configuration");
    }
}

TrackState TargetTracker::update(
    const std::vector<Candidate>& candidates,
    bool activationHeld,
    double selectionOriginX,
    double selectionOriginY) {
    if (!activationHeld) {
        reset();
        return {};
    }

    const Candidate* selected = nullptr;
    bool newlyPinned = false;

    if (!pinned_) {
        double bestScore = std::numeric_limits<double>::infinity();
        for (const auto& candidate : candidates) {
            const double distance = std::hypot(
                candidate.centroidX - selectionOriginX,
                candidate.centroidY - selectionOriginY);
            const double score =
                distance + config_.confidencePenaltyPixels * (1.0 - clamp01(candidate.confidence));
            if (score < bestScore) {
                bestScore = score;
                selected = &candidate;
            }
        }
        if (selected) {
            pinned_ = true;
            newlyPinned = true;
            missedFrames_ = 0;
            lastObservedX_ = selected->centroidX;
            lastObservedY_ = selected->centroidY;
            smoothedX_ = selected->centroidX;
            smoothedY_ = selected->centroidY;
            lastCandidate_ = *selected;
        }
    } else {
        double bestDistance = std::numeric_limits<double>::infinity();
        for (const auto& candidate : candidates) {
            const double distance = std::hypot(
                candidate.centroidX - lastObservedX_,
                candidate.centroidY - lastObservedY_);
            if (distance <= config_.reacquireRadiusPixels && distance < bestDistance) {
                bestDistance = distance;
                selected = &candidate;
            }
        }

        if (selected) {
            missedFrames_ = 0;
            lastObservedX_ = selected->centroidX;
            lastObservedY_ = selected->centroidY;
            smoothedX_ += config_.smoothingFactor * (selected->centroidX - smoothedX_);
            smoothedY_ += config_.smoothingFactor * (selected->centroidY - smoothedY_);
            lastCandidate_ = *selected;
        } else {
            ++missedFrames_;
            if (missedFrames_ > config_.maximumMissedFrames) {
                reset();
                return {};
            }
        }
    }

    if (!pinned_) {
        return {};
    }
    return TrackState{
        true,
        selected != nullptr,
        newlyPinned,
        missedFrames_,
        smoothedX_,
        smoothedY_,
        lastCandidate_};
}

void TargetTracker::reset() {
    pinned_ = false;
    missedFrames_ = 0;
    lastObservedX_ = 0.0;
    lastObservedY_ = 0.0;
    smoothedX_ = 0.0;
    smoothedY_ = 0.0;
    lastCandidate_ = {};
}

}  // namespace colorbot
