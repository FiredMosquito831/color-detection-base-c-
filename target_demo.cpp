#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>

namespace {

constexpr UINT_PTR kAnimationTimer = 1;
constexpr UINT kAnimationPeriodMilliseconds = 8;

struct DemoState {
    std::chrono::steady_clock::time_point started{std::chrono::steady_clock::now()};
    int receivedClicks{};
    int autoCloseSeconds{};
};

struct TargetVisual {
    double x{};
    double y{};
    int radius{};
    wchar_t label{};
};

[[nodiscard]] std::array<TargetVisual, 3> targetPositions(
    double timeSeconds,
    int centerX,
    int centerY) {
    // Target A crosses the exact center repeatedly to demonstrate the trigger.
    const TargetVisual a{
        centerX + 105.0 * std::sin(timeSeconds * 0.85),
        static_cast<double>(centerY),
        15,
        L'A'};

    // Targets B and C produce independent paths for nearest-target selection.
    const TargetVisual b{
        centerX + 88.0 * std::cos(timeSeconds * 0.62 + 2.1),
        centerY + 62.0 * std::sin(timeSeconds * 0.91 + 0.7),
        19,
        L'B'};
    const TargetVisual c{
        centerX + 112.0 * std::cos(timeSeconds * 0.74 + 4.0),
        centerY + 72.0 * std::sin(timeSeconds * 0.56 + 2.4),
        12,
        L'C'};
    return {a, b, c};
}

void drawCrosshair(HDC dc, int centerX, int centerY) {
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(100, 115, 135));
    const HGDIOBJ previousPen = SelectObject(dc, pen);
    MoveToEx(dc, centerX - 18, centerY, nullptr);
    LineTo(dc, centerX + 19, centerY);
    MoveToEx(dc, centerX, centerY - 18, nullptr);
    LineTo(dc, centerX, centerY + 19);
    SelectObject(dc, previousPen);
    DeleteObject(pen);
}

void drawTarget(HDC dc, const TargetVisual& target) {
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 170, 0));
    HBRUSH brush = CreateSolidBrush(RGB(255, 255, 35));
    const HGDIOBJ previousPen = SelectObject(dc, pen);
    const HGDIOBJ previousBrush = SelectObject(dc, brush);

    const int x = static_cast<int>(std::lround(target.x));
    const int y = static_cast<int>(std::lround(target.y));
    Ellipse(
        dc,
        x - target.radius,
        y - target.radius,
        x + target.radius + 1,
        y + target.radius + 1);

    SelectObject(dc, previousBrush);
    SelectObject(dc, previousPen);
    DeleteObject(brush);
    DeleteObject(pen);

    const wchar_t label[2]{target.label, L'\0'};
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(25, 25, 25));
    TextOutW(dc, x - 4, y - 8, label, 1);
}

LRESULT CALLBACK demoWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    auto* state = reinterpret_cast<DemoState*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    switch (message) {
        case WM_CREATE:
            SetTimer(window, kAnimationTimer, kAnimationPeriodMilliseconds, nullptr);
            return 0;

        case WM_TIMER:
            if (wParam == kAnimationTimer) {
                if (state && state->autoCloseSeconds > 0 &&
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - state->started).count() >=
                        state->autoCloseSeconds) {
                    DestroyWindow(window);
                    return 0;
                }
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN:
            if (state) {
                ++state->receivedClicks;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                DestroyWindow(window);
            }
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            HBRUSH background = CreateSolidBrush(RGB(18, 24, 34));
            FillRect(dc, &client, background);
            DeleteObject(background);

            const int centerX = (client.right - client.left) / 2;
            const int centerY = (client.bottom - client.top) / 2;
            drawCrosshair(dc, centerX, centerY);

            const double elapsedSeconds = state
                ? std::chrono::duration<double>(
                      std::chrono::steady_clock::now() - state->started).count()
                : 0.0;
            for (const auto& target : targetPositions(elapsedSeconds, centerX, centerY)) {
                drawTarget(dc, target);
            }

            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(235, 240, 250));
            const std::wstring title = L"UNIVERSITY TARGET TRACKING / PINNING FIXTURE";
            TextOutW(dc, 20, 18, title.c_str(), static_cast<int>(title.size()));
            const std::wstring instructions =
                L"Target A crosses center | Hold Mouse 4: trigger | Hold Mouse 5: pin/follow | Esc: close";
            TextOutW(dc, 20, 44, instructions.c_str(), static_cast<int>(instructions.size()));
            const std::wstring clickCount =
                L"Clicks received by this fixture: " +
                std::to_wstring(state ? state->receivedClicks : 0);
            TextOutW(dc, 20, 70, clickCount.c_str(), static_cast<int>(clickCount.size()));
            const std::wstring centerLabel = L"center gate";
            TextOutW(
                dc,
                centerX + 22,
                centerY + 8,
                centerLabel.c_str(),
                static_cast<int>(centerLabel.size()));

            EndPaint(window, &paint);
            return 0;
        }

        case WM_DESTROY:
            KillTimer(window, kAnimationTimer);
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(window, message, wParam, lParam);
    }
}

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR commandLine, int) {
    const wchar_t* className = L"ColorDetectionUniversityTargetFixture";
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = demoWindowProcedure;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return 1;
    }

    constexpr int width = 960;
    constexpr int height = 640;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    DemoState state;
    if (commandLine && *commandLine) {
        state.autoCloseSeconds = std::max(0, std::atoi(commandLine));
    }
    HWND window = CreateWindowExW(
        WS_EX_APPWINDOW | WS_EX_TOPMOST,
        className,
        L"University Tracking and Pinning Demonstration",
        WS_POPUP,
        x,
        y,
        width,
        height,
        nullptr,
        nullptr,
        instance,
        &state);
    if (!window) {
        return 1;
    }

    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    SetForegroundWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
