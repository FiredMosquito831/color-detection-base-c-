
/*
VERSION 1

#include <Windows.h>
#include <iostream>
#include <thread>

// Window procedure to handle messages for the overlay window
LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            // Set up for transparency with a NULL brush (fully transparent)
            HBRUSH hBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
            SelectObject(hdc, hBrush);
            SetBkMode(hdc, TRANSPARENT);

            // Only draw additional elements if needed (e.g., detection boxes)
            // Here you can call any drawing functions if needed, like drawDetectionBox

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            // Return 1 to indicate that we handled the background erase
            // This prevents flickering and keeps the background transparent
            return 1;

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}


// Function to create a transparent overlay window
HWND createOverlayWindow() {
    // Define a window class for the overlay
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = OverlayWndProc;  // Use custom window procedure
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"TransparentOverlay";
    RegisterClassW(&wc);

    // Create the overlay window with layered and transparent styles
    HWND overlayWnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
            wc.lpszClassName, L"Overlay",
            WS_POPUP,
            0, 0, 1920, 1080, NULL, NULL, wc.hInstance, NULL);

    if (!overlayWnd) {
        std::cerr << "Failed to create overlay window.\n";
        return NULL;
    }

    // Set the window to be transparent by specifying a color key
    COLORREF transparentColor = RGB(0, 0, 0);  // Use black as transparent color
    SetLayeredWindowAttributes(overlayWnd, transparentColor, 0, LWA_COLORKEY);

    return overlayWnd;
}

// Function to draw a detection box on the overlay window
void drawDetectionBox(HWND overlayWnd, int x, int y, int width, int height) {
    HDC hdc = GetDC(overlayWnd);
    if (hdc) {
        HPEN hPen = CreatePen(PS_SOLID, 2, RGB(255, 0, 0));  // Red detection box
        HBRUSH hBrush = CreateSolidBrush(RGB(0, 0, 0));  // Transparent brush
        SelectObject(hdc, hPen);
        SelectObject(hdc, hBrush);

        // Draw a rectangle for the detection area
        Rectangle(hdc, x, y, x + width, y + height);

        // Clean up
        DeleteObject(hPen);
        DeleteObject(hBrush);
        ReleaseDC(overlayWnd, hdc);
    }
}

// Main application entry point
int main() {
    std::cout << "Starting transparent overlay...\n";

    // Create the overlay window
    HWND overlayWnd = createOverlayWindow();
    if (!overlayWnd) {
        return -1;  // Exit if the overlay creation fails
    }

    // Show and update the overlay window
    ShowWindow(overlayWnd, SW_SHOW);
    UpdateWindow(overlayWnd);

    // Example: Draw a detection box at (960, 540) with size 50x50
    drawDetectionBox(overlayWnd, 960 - 25, 540 - 25, 50, 50);

    // Run a message loop to keep the window open
    MSG msg = { 0 };
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}

*/

/*  version 2
#include <Windows.h>
#include <iostream>
#include <thread>
#include <vector>

// Target color and tolerance
const COLORREF targetColor = RGB(255, 255, 35);
const int tolerance = 60;

// Coordinates for detection boxes (adjust as needed)
std::vector<RECT> detectionRects;

// Check if a color is within tolerance of the target color
bool isColorMatch(COLORREF color, COLORREF targetColor, int tolerance) {
    int r = GetRValue(color);
    int g = GetGValue(color);
    int b = GetBValue(color);

    int targetR = GetRValue(targetColor);
    int targetG = GetGValue(targetColor);
    int targetB = GetBValue(targetColor);

    return (abs(r - targetR) <= tolerance &&
            abs(g - targetG) <= tolerance &&
            abs(b - targetB) <= tolerance);
}

// Window procedure for overlay window to handle drawing
LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            HBRUSH hBrush = CreateSolidBrush(RGB(0, 255, 0));  // Green for detection boxes
            SelectObject(hdc, hBrush);

            for (const auto& rect : detectionRects) {
                FrameRect(hdc, &rect, hBrush);  // Draw green rectangle around detected area
            }

            DeleteObject(hBrush);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;  // Prevent background erasing to avoid flickering

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

// Function to create a transparent overlay window
HWND createOverlayWindow() {
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"TransparentOverlay";
    RegisterClassW(&wc);

    HWND overlayWnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
            wc.lpszClassName, L"Overlay",
            WS_POPUP,
            0, 0, 1920, 1080, NULL, NULL, wc.hInstance, NULL);

    if (!overlayWnd) {
        std::cerr << "Failed to create overlay window.\n";
        return NULL;
    }

    SetLayeredWindowAttributes(overlayWnd, RGB(0, 0, 0), 0, LWA_COLORKEY);

    return overlayWnd;
}

// Function to detect color and update detection rectangles
void detectColor(HWND overlayWnd) {
    HDC hdcScreen = GetDC(NULL);

    while (true) {
        detectionRects.clear();

        for (int y = 0; y < 1080; y += 5) {
            for (int x = 0; x < 1920; x += 5) {
                COLORREF color = GetPixel(hdcScreen, x, y);
                if (isColorMatch(color, targetColor, tolerance)) {
                    RECT rect = { x - 5, y - 5, x + 5, y + 5 };
                    detectionRects.push_back(rect);
                }
            }
        }

        // Request a redraw of the overlay window
        InvalidateRect(overlayWnd, NULL, TRUE);
        UpdateWindow(overlayWnd);

        Sleep(30);  // Adjust as needed to balance CPU usage
    }

    ReleaseDC(NULL, hdcScreen);
}

int main() {
    std::cout << "Starting transparent overlay with color detection...\n";

    HWND overlayWnd = createOverlayWindow();
    if (!overlayWnd) {
        return -1;
    }

    ShowWindow(overlayWnd, SW_SHOW);
    UpdateWindow(overlayWnd);

    // Run color detection in a separate thread
    std::thread detectionThread(detectColor, overlayWnd);

    // Main message loop
    MSG msg = { 0 };
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    detectionThread.join();
    return 0;
}

*/

/*
 python copy version


 #include <Windows.h>
#include <iostream>
#include <thread>
#include <vector>
#include <mmsystem.h> // For timing functions
#include <chrono> // For sleep

// Mouse input simulation
#include <ctypes.h>
#include <thread>

const int TARGET_COLOR_R = 255;
const int TARGET_COLOR_G = 255;
const int TARGET_COLOR_B = 35;
const int TOLERANCE = 60;

// Function to simulate a mouse click
void simulateClick(int x, int y) {
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dx = x;
    input.mi.dy = y;
    input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &input, sizeof(INPUT));
    Sleep(10); // short delay for click duration
    input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(INPUT));
}

// Check if a color is within tolerance
bool isColorMatch(COLORREF color) {
    int r = GetRValue(color);
    int g = GetGValue(color);
    int b = GetBValue(color);

    return abs(r - TARGET_COLOR_R) <= TOLERANCE &&
           abs(g - TARGET_COLOR_G) <= TOLERANCE &&
           abs(b - TARGET_COLOR_B) <= TOLERANCE;
}

// Overlay Window Procedure
LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            // Handle painting of the window here
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            // Draw detection boxes or any other overlay visualizations here

            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1; // Prevent background erasing to reduce flickering
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

// Function to create a semi-transparent overlay window
HWND createOverlayWindow() {
    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"TransparentOverlay";
    RegisterClass(&wc);

    HWND overlayWnd = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TOPMOST,
        wc.lpszClassName, L"Overlay",
        WS_POPUP,
        0, 0, 1920, 1080, NULL, NULL, wc.hInstance, NULL);

    if (!overlayWnd) {
        std::cerr << "Failed to create overlay window.\n";
        return NULL;
    }

    SetLayeredWindowAttributes(overlayWnd, RGB(0, 0, 0), 50, LWA_ALPHA); // Semi-transparent
    return overlayWnd;
}

// Function to detect color and update overlay
void detectColor(HWND overlayWnd) {
    while (true) {
        // Capture the screen
        HDC hdcScreen = GetDC(NULL);
        for (int y = 0; y < 1080; y += 10) {
            for (int x = 0; x < 1920; x += 10) {
                COLORREF color = GetPixel(hdcScreen, x, y);
                if (isColorMatch(color)) {
                    simulateClick(x, y); // Simulate a click at detected position

                    // Optionally add a rectangle to the overlay here to show where the color was detected
                }
            }
        }
        ReleaseDC(NULL, hdcScreen);
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Control loop speed
    }
}

int main() {
    std::cout << "Starting transparent overlay with color detection...\n";

    HWND overlayWnd = createOverlayWindow();
    if (!overlayWnd) {
        return -1;
    }

    ShowWindow(overlayWnd, SW_SHOW);
    UpdateWindow(overlayWnd);

    // Run color detection in a separate thread
    std::thread detectionThread(detectColor, overlayWnd);

    // Main message loop
    MSG msg = { 0 };
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    detectionThread.join();
    return 0;
}



 */



/* super good detection speed

#include <Windows.h>
#include <iostream>
#include <thread>
#include <vector>

// Target color and tolerance
const COLORREF targetColor = RGB(255, 255, 35); // The color to detect
const int tolerance = 60; // Tolerance for color matching

// Store detected rectangles
std::vector<RECT> detectionRects;

// Check if a color is within tolerance of the target color
bool isColorMatch(COLORREF color, COLORREF targetColor, int tolerance) {
    int r = GetRValue(color);
    int g = GetGValue(color);
    int b = GetBValue(color);

    int targetR = GetRValue(targetColor);
    int targetG = GetGValue(targetColor);
    int targetB = GetBValue(targetColor);

    return (abs(r - targetR) <= tolerance &&
            abs(g - targetG) <= tolerance &&
            abs(b - targetB) <= tolerance);
}

// Function to simulate a mouse click
void simulateClick(int x, int y) {
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dx = x;
    input.mi.dy = y;
    input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &input, sizeof(INPUT));
    Sleep(10); // short delay for click duration
    input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(INPUT));
}

// Window procedure for overlay window to handle drawing
LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            // Use a solid brush with a translucent color for visibility
            HBRUSH hBrush = CreateSolidBrush(RGB(255, 0, 0)); // Red for detection boxes
            SelectObject(hdc, hBrush);

            // Draw rectangles for each detected area
            for (const auto& rect : detectionRects) {
                FrameRect(hdc, &rect, hBrush); // Draw the outline of the rectangle
            }

            DeleteObject(hBrush);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;  // Prevent background erasing to reduce flickering

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

// Function to create a semi-transparent overlay window
HWND createOverlayWindow() {
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"TransparentOverlay";
    RegisterClassW(&wc);

    HWND overlayWnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
            wc.lpszClassName, L"Overlay",
            WS_POPUP,
            0, 0, 1920, 1080, NULL, NULL, wc.hInstance, NULL);

    if (!overlayWnd) {
        std::cerr << "Failed to create overlay window.\n";
        return NULL;
    }

    // Set the window to be semi-transparent (alpha = 100 out of 255)
    SetLayeredWindowAttributes(overlayWnd, RGB(0, 0, 0), 100, LWA_ALPHA); // More opaque

    return overlayWnd;
}

// Function to detect color and update detection rectangles
void detectColor(HWND overlayWnd) {
    HDC hdcScreen = GetDC(NULL);

    // Define center of the screen
    const int centerX = 1920 / 2;
    const int centerY = 1080 / 2;

    while (true) {
        detectionRects.clear();

        for (int y = 0; y < 1080; y += 10) {
            for (int x = 0; x < 1920; x += 10) {
                COLORREF color = GetPixel(hdcScreen, x, y);
                if (isColorMatch(color, targetColor, tolerance)) {
                    RECT rect = { x - 10, y - 10, x + 10, y + 10 }; // Increase size of the box
                    detectionRects.push_back(rect);

                    // Debug output to verify detections
                    std::cout << "Detected color at (" << x << ", " << y << ")\n";
                }
            }
        }

        // Check if the center of the screen is within any detected rectangle
        for (const auto& rect : detectionRects) {
            if (centerX >= rect.left && centerX <= rect.right &&
                centerY >= rect.top && centerY <= rect.bottom) {
                simulateClick(centerX, centerY); // Simulate click at the center
                break; // Click only once if detected
            }
        }

        // Request a redraw of the overlay window
        InvalidateRect(overlayWnd, NULL, TRUE);
        UpdateWindow(overlayWnd);

        Sleep(10);  // Update every 10 ms
    }

    ReleaseDC(NULL, hdcScreen);
}

int main() {
    std::cout << "Starting transparent overlay with color detection...\n";

    HWND overlayWnd = createOverlayWindow();
    if (!overlayWnd) {
        return -1;
    }

    ShowWindow(overlayWnd, SW_SHOW);
    UpdateWindow(overlayWnd);

    // Run color detection in a separate thread
    std::thread detectionThread(detectColor, overlayWnd);

    // Main message loop
    MSG msg = { 0 };
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    detectionThread.join();
    return 0;
}

 */






/* SUCCESFULL DETETION VERSION 1
 *
 *
#include <Windows.h>
#include <iostream>
#include <thread>
#include <vector>
#include <cmath>

// Target color and tolerance
const COLORREF targetColor = RGB(255, 255, 35); // The color to detect
const int tolerance = 60; // Tolerance for color matching

// Store detected rectangles
std::vector<RECT> detectionRects;

// Check if a color is within tolerance of the target color
bool isColorMatch(COLORREF color, COLORREF targetColor, int tolerance) {
    int r = GetRValue(color);
    int g = GetGValue(color);
    int b = GetBValue(color);

    int targetR = GetRValue(targetColor);
    int targetG = GetGValue(targetColor);
    int targetB = GetBValue(targetColor);

    return (abs(r - targetR) <= tolerance &&
            abs(g - targetG) <= tolerance &&
            abs(b - targetB) <= tolerance);
}

// Function to simulate a mouse click
void simulateClick(int x, int y) {
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dx = x;
    input.mi.dy = y;
    input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &input, sizeof(INPUT));
    Sleep(10); // short delay for click duration
    input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(INPUT));

    // Play a beep sound (you can replace this with a sound file if needed)
    Beep(1000, 200); // Frequency: 1000 Hz, Duration: 200 ms
}

// Window procedure for overlay window to handle drawing
LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            // Use a solid brush with a translucent color for visibility
            HBRUSH hBrush = CreateSolidBrush(RGB(255, 0, 0)); // Red for detection boxes
            SelectObject(hdc, hBrush);

            // Draw rectangles for each detected area
            for (const auto& rect : detectionRects) {
                FrameRect(hdc, &rect, hBrush); // Draw the outline of the rectangle
            }

            DeleteObject(hBrush);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;  // Prevent background erasing to reduce flickering

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

// Function to create a semi-transparent overlay window
HWND createOverlayWindow() {
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"TransparentOverlay";
    RegisterClassW(&wc);

    HWND overlayWnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
            wc.lpszClassName, L"Overlay",
            WS_POPUP,
            0, 0, 1920, 1080, NULL, NULL, wc.hInstance, NULL);

    if (!overlayWnd) {
        std::cerr << "Failed to create overlay window.\n";
        return NULL;
    }

    // Set the window to be semi-transparent (alpha = 100 out of 255)
    SetLayeredWindowAttributes(overlayWnd, RGB(0, 0, 0), 100, LWA_ALPHA); // More opaque

    return overlayWnd;
}

// Function to detect color and update detection rectangles
void detectColor(HWND overlayWnd) {
    HDC hdcScreen = GetDC(NULL);

    // Define center of the screen
    const int centerX = 1920 / 2;
    const int centerY = 1080 / 2;

    // Define the area to check for color detection (4x4 pixels)
    const int detectAreaSize = 4;

    while (true) {
        detectionRects.clear();

        // Check the 4x4 area at the center of the screen
        for (int y = centerY - detectAreaSize / 2; y < centerY + detectAreaSize / 2; y++) {
            for (int x = centerX - detectAreaSize / 2; x < centerX + detectAreaSize / 2; x++) {
                COLORREF color = GetPixel(hdcScreen, x, y);
                if (isColorMatch(color, targetColor, tolerance)) {
                    RECT rect = { centerX - detectAreaSize, centerY - detectAreaSize, centerX + detectAreaSize, centerY + detectAreaSize };
                    detectionRects.push_back(rect);

                    // Debug output to verify detections
                    std::cout << "Detected color at (" << x << ", " << y << ")\n";
                }
            }
        }

        // Check if the center of the screen sees the detected color
        if (!detectionRects.empty()) {
            simulateClick(centerX, centerY); // Simulate click at the center
        }

        // Request a redraw of the overlay window
        InvalidateRect(overlayWnd, NULL, TRUE);
        UpdateWindow(overlayWnd);

        Sleep(10);  // Update every 10 ms
    }

    ReleaseDC(NULL, hdcScreen);
}

int main() {
    std::cout << "Starting transparent overlay with color detection...\n";

    HWND overlayWnd = createOverlayWindow();
    if (!overlayWnd) {
        return -1;
    }

    ShowWindow(overlayWnd, SW_SHOW);
    UpdateWindow(overlayWnd);

    // Run color detection in a separate thread
    std::thread detectionThread(detectColor, overlayWnd);

    // Main message loop
    MSG msg = { 0 };
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    detectionThread.join();
    return 0;
}

*/


/*
 working v1 i have to close the overlay for unlimited detection
*/

#include <Windows.h>
#include <iostream>
#include <thread>
#include <vector>
#include <cmath>

// Target color and tolerance
const COLORREF targetColor = RGB(255, 255, 35); // The color to detect
const int tolerance = 60; // Tolerance for color matching

// Store detected rectangles
std::vector<RECT> detectionRects;

// Check if a color is within tolerance of the target color
bool isColorMatch(COLORREF color, COLORREF targetColor, int tolerance) {
    int r = GetRValue(color);
    int g = GetGValue(color);
    int b = GetBValue(color);

    int targetR = GetRValue(targetColor);
    int targetG = GetGValue(targetColor);
    int targetB = GetBValue(targetColor);

    return (abs(r - targetR) <= tolerance &&
            abs(g - targetG) <= tolerance &&
            abs(b - targetB) <= tolerance);
}

// Function to simulate a mouse click
void simulateClick(int x, int y) {
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dx = x;
    input.mi.dy = y;
    input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &input, sizeof(INPUT));
    Sleep(10); // short delay for click duration
    input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(INPUT));

    // Play a beep sound (you can replace this with a sound file if needed)
    Beep(1000, 200); // Frequency: 1000 Hz, Duration: 200 ms
}

// Window procedure for overlay window to handle drawing
LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            // Use a solid brush with a translucent color for visibility
            HBRUSH hBrush = CreateSolidBrush(RGB(255, 0, 0)); // Red for detection boxes
            SelectObject(hdc, hBrush);

            // Draw rectangles for each detected area
            for (const auto& rect : detectionRects) {
                FrameRect(hdc, &rect, hBrush); // Draw the outline of the rectangle
            }

            DeleteObject(hBrush);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;  // Prevent background erasing to reduce flickering

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

// Function to create a semi-transparent overlay window
HWND createOverlayWindow() {
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"TransparentOverlay";
    RegisterClassW(&wc);

    HWND overlayWnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
            wc.lpszClassName, L"Overlay",
            WS_POPUP,
            0, 0, 1920, 1080, NULL, NULL, wc.hInstance, NULL);

    if (!overlayWnd) {
        std::cerr << "Failed to create overlay window.\n";
        return NULL;
    }

    // Set the window to be semi-transparent (alpha = 100 out of 255)
    SetLayeredWindowAttributes(overlayWnd, RGB(0, 0, 0), 100, LWA_ALPHA); // More opaque

    return overlayWnd;
}

// Function to detect color and update detection rectangles
void detectColor(HWND overlayWnd) {
    HDC hdcScreen = GetDC(NULL);

    // Define center of the screen
    const int centerX = 1920 / 2;
    const int centerY = 1080 / 2;

    // Define the area to check for color detection (4x4 pixels)
    const int detectAreaSize = 5;

    while (true) {
        detectionRects.clear(); // Clear previous detections

        // Check the 4x4 area at the center of the screen
        bool foundDetection = false; // Flag to indicate if detection occurred

        for (int y = centerY - detectAreaSize / 2; y < centerY + detectAreaSize / 2; y++) {
            for (int x = centerX - detectAreaSize / 2; x < centerX + detectAreaSize / 2; x++) {
                COLORREF color = GetPixel(hdcScreen, x, y);
                if (isColorMatch(color, targetColor, tolerance)) {
                    foundDetection = true; // Set the detection flag
                    RECT rect = { centerX - detectAreaSize, centerY - detectAreaSize, centerX + detectAreaSize, centerY + detectAreaSize };
                    detectionRects.push_back(rect);

                    // Debug output to verify detections
                    std::cout << "Detected color at (" << x << ", " << y << ")\n";
                }
            }
        }

        // Check if the center of the screen sees the detected color
        if (foundDetection) {
            simulateClick(centerX, centerY); // Simulate click at the center
        }

        // Request a redraw of the overlay window
        InvalidateRect(overlayWnd, NULL, TRUE);
        UpdateWindow(overlayWnd);

        Sleep(5);  // Update every 10 ms
    }

    ReleaseDC(NULL, hdcScreen);
}

int main() {
    std::cout << "Starting transparent overlay with color detection...\n";

    HWND overlayWnd = createOverlayWindow();
    if (!overlayWnd) {
        return -1;
    }

    ShowWindow(overlayWnd, SW_SHOW);
    UpdateWindow(overlayWnd);

    // Run color detection in a separate thread
    std::thread detectionThread(detectColor, overlayWnd);

    // Main message loop
    MSG msg = { 0 };
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    detectionThread.join();
    return 0;
}

