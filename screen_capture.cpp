#include "screen_capture.hpp"

#include <d3d11.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace colorbot {
namespace {

// Minimal COM handle. The project targets MinGW as well as MSVC, so this
// avoids depending on WRL.
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept : pointer_(other.pointer_) { other.pointer_ = nullptr; }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset();
            pointer_ = other.pointer_;
            other.pointer_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] T** put() {
        reset();
        return &pointer_;
    }
    [[nodiscard]] T* get() const { return pointer_; }
    T* operator->() const { return pointer_; }
    explicit operator bool() const { return pointer_ != nullptr; }

    void reset() {
        if (pointer_) {
            pointer_->Release();
            pointer_ = nullptr;
        }
    }

private:
    T* pointer_{};
};

class GdiScreenCapture final : public ScreenCapture {
public:
    GdiScreenCapture(int width, int height)
        : width_(width), height_(height), stride_(width * 4) {
        if (width <= 0 || height <= 0) {
            throw std::invalid_argument("Capture dimensions must be positive");
        }
        screenDc_ = GetDC(nullptr);
        if (!screenDc_) {
            throw std::runtime_error("GetDC failed");
        }
        memoryDc_ = CreateCompatibleDC(screenDc_);
        if (!memoryDc_) {
            release();
            throw std::runtime_error("CreateCompatibleDC failed");
        }

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width_;
        info.bmiHeader.biHeight = -height_;  // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap_ = CreateDIBSection(
            screenDc_, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels_), nullptr, 0);
        if (!bitmap_ || !pixels_) {
            release();
            throw std::runtime_error("CreateDIBSection failed");
        }
        previousBitmap_ = SelectObject(memoryDc_, bitmap_);
        if (!previousBitmap_ || previousBitmap_ == HGDI_ERROR) {
            previousBitmap_ = nullptr;
            release();
            throw std::runtime_error("SelectObject failed");
        }
    }

    ~GdiScreenCapture() override { release(); }

    [[nodiscard]] bool capture(int screenX, int screenY) override {
        return BitBlt(memoryDc_, 0, 0, width_, height_, screenDc_, screenX, screenY, SRCCOPY) !=
            FALSE;
    }

    [[nodiscard]] const std::uint8_t* pixels() const override { return pixels_; }
    [[nodiscard]] int stride() const override { return stride_; }
    [[nodiscard]] const char* backendName() const override { return "GDI BitBlt"; }

private:
    void release() noexcept {
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

class DuplicationScreenCapture final : public ScreenCapture {
public:
    // Bounded so a static desktop cannot stall the detection loop.
    static constexpr UINT kAcquireTimeoutMs = 2;

    DuplicationScreenCapture(int width, int height)
        : width_(width), height_(height), stride_(width * 4) {
        if (width <= 0 || height <= 0) {
            throw std::invalid_argument("Capture dimensions must be positive");
        }
        buffer_.assign(static_cast<std::size_t>(stride_) * height_, 0);
        initialize();
    }

    [[nodiscard]] bool capture(int screenX, int screenY) override {
        if (!duplication_) {
            // A previous frame lost the duplication interface, which happens on
            // desktop switches, resolution changes and secure-desktop
            // transitions. Rebuilding is the documented recovery.
            try {
                initialize();
            } catch (const std::exception&) {
                return false;
            }
        }

        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        ComPtr<IDXGIResource> resource;
        // Wait briefly rather than not at all. A zero timeout returns
        // immediately whenever the compositor has not yet published the next
        // desktop frame, and the caller then analyzes the previous contents;
        // measured against a continuously animating window that path missed
        // roughly four fifths of the updates. A few milliseconds is long
        // enough to pick up an imminent frame and still well inside the loop's
        // period, and a genuine timeout means the desktop really is unchanged,
        // so reusing the buffer is correct rather than stale.
        const HRESULT acquired =
            duplication_->AcquireNextFrame(kAcquireTimeoutMs, &frameInfo, resource.put());
        if (acquired == DXGI_ERROR_WAIT_TIMEOUT) {
            return true;  // No update; the existing buffer still describes the screen.
        }
        if (FAILED(acquired)) {
            duplication_.reset();
            return acquired == DXGI_ERROR_ACCESS_LOST ? capture(screenX, screenY) : false;
        }

        bool copied = false;
        ComPtr<ID3D11Texture2D> frame;
        if (SUCCEEDED(resource->QueryInterface(
                __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(frame.put())))) {
            copied = copyRegion(frame.get(), screenX, screenY);
        }
        duplication_->ReleaseFrame();
        return copied;
    }

    [[nodiscard]] const std::uint8_t* pixels() const override { return buffer_.data(); }
    [[nodiscard]] int stride() const override { return stride_; }
    [[nodiscard]] const char* backendName() const override { return "Desktop Duplication"; }

private:
    void initialize() {
        device_.reset();
        context_.reset();
        duplication_.reset();
        staging_.reset();

        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        D3D_FEATURE_LEVEL obtained{};
        HRESULT result = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0,
            levels,
            static_cast<UINT>(std::size(levels)),
            D3D11_SDK_VERSION,
            device_.put(),
            &obtained,
            context_.put());
        if (FAILED(result)) {
            throw std::runtime_error("D3D11CreateDevice failed");
        }

        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(device_->QueryInterface(
                __uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgiDevice.put())))) {
            throw std::runtime_error("IDXGIDevice query failed");
        }
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDevice->GetAdapter(adapter.put()))) {
            throw std::runtime_error("IDXGIAdapter query failed");
        }

        // Find the output whose desktop rectangle contains the screen origin,
        // which is the primary display the detector already targets.
        ComPtr<IDXGIOutput> output;
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIOutput> candidate;
            if (adapter->EnumOutputs(index, candidate.put()) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_OUTPUT_DESC description{};
            if (FAILED(candidate->GetDesc(&description))) {
                continue;
            }
            if (description.DesktopCoordinates.left == 0 &&
                description.DesktopCoordinates.top == 0) {
                outputOriginX_ = description.DesktopCoordinates.left;
                outputOriginY_ = description.DesktopCoordinates.top;
                output = std::move(candidate);
                break;
            }
        }
        if (!output) {
            throw std::runtime_error("No primary DXGI output found");
        }

        ComPtr<IDXGIOutput1> output1;
        if (FAILED(output->QueryInterface(
                __uuidof(IDXGIOutput1), reinterpret_cast<void**>(output1.put())))) {
            throw std::runtime_error("IDXGIOutput1 is unavailable");
        }
        if (FAILED(output1->DuplicateOutput(device_.get(), duplication_.put()))) {
            throw std::runtime_error("DuplicateOutput failed");
        }

        D3D11_TEXTURE2D_DESC staging{};
        staging.Width = static_cast<UINT>(width_);
        staging.Height = static_cast<UINT>(height_);
        staging.MipLevels = 1;
        staging.ArraySize = 1;
        staging.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        staging.SampleDesc.Count = 1;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&staging, nullptr, staging_.put()))) {
            throw std::runtime_error("Staging texture creation failed");
        }
    }

    [[nodiscard]] bool copyRegion(ID3D11Texture2D* frame, int screenX, int screenY) {
        D3D11_TEXTURE2D_DESC description{};
        frame->GetDesc(&description);

        // Desktop Duplication reports coordinates relative to the output, so
        // translate the requested screen position and clamp the box to the
        // surface. An out-of-range box makes CopySubresourceRegion a no-op and
        // would silently leave a stale frame in the buffer.
        const int localX = screenX - outputOriginX_;
        const int localY = screenY - outputOriginY_;
        if (localX < 0 || localY < 0 ||
            localX + width_ > static_cast<int>(description.Width) ||
            localY + height_ > static_cast<int>(description.Height)) {
            return false;
        }

        D3D11_BOX box{};
        box.left = static_cast<UINT>(localX);
        box.top = static_cast<UINT>(localY);
        box.front = 0;
        box.right = static_cast<UINT>(localX + width_);
        box.bottom = static_cast<UINT>(localY + height_);
        box.back = 1;
        context_->CopySubresourceRegion(staging_.get(), 0, 0, 0, 0, frame, 0, &box);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            return false;
        }
        const auto* source = static_cast<const std::uint8_t*>(mapped.pData);
        for (int y = 0; y < height_; ++y) {
            std::memcpy(
                buffer_.data() + static_cast<std::size_t>(y) * stride_,
                source + static_cast<std::size_t>(y) * mapped.RowPitch,
                static_cast<std::size_t>(stride_));
        }
        context_->Unmap(staging_.get(), 0);
        return true;
    }

    int width_{};
    int height_{};
    int stride_{};
    int outputOriginX_{};
    int outputOriginY_{};
    std::vector<std::uint8_t> buffer_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<ID3D11Texture2D> staging_;
};

}  // namespace

std::unique_ptr<ScreenCapture> createScreenCapture(
    CaptureBackend backend,
    int width,
    int height,
    std::string& fallbackReason) {
    fallbackReason.clear();

    if (backend == CaptureBackend::Gdi) {
        return std::make_unique<GdiScreenCapture>(width, height);
    }
    if (backend == CaptureBackend::DesktopDuplication) {
        return std::make_unique<DuplicationScreenCapture>(width, height);
    }

    try {
        return std::make_unique<DuplicationScreenCapture>(width, height);
    } catch (const std::exception& error) {
        fallbackReason = error.what();
        return std::make_unique<GdiScreenCapture>(width, height);
    }
}

}  // namespace colorbot
