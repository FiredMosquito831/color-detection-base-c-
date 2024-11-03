/* VERSION 1 WORKING GREAT
#include <Windows.h>
#include <iostream>
#include <thread>
#include <vector>
#include <cmath>

// Target color and tolerance
const COLORREF targetColor = RGB(255, 255, 35); // The color to detect
const int tolerance = 60; // Tolerance for color matching

// Center detection area (8x8 pixels)
const int centerX = 1920 / 2;
const int centerY = 1080 / 2;
const int detectionAreaSize = 8; // Size of detection area

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
void simulateLeftClick() {
    // Simulate mouse down
    INPUT input = { 0 };
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &input, sizeof(INPUT));

    // Small delay for click hold
    Sleep(5); // ori 10

    // Simulate mouse up
    input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(INPUT));
}

// Function to detect color in the center area and trigger a click if detected
void detectColorAndClick() {
    HDC hdcScreen = GetDC(NULL); // Get the screen device context

    while (true) {
        bool foundColor = false; // Flag to track color detection

        // Check the 8x8 area at the center of the screen
        for (int y = centerY - detectionAreaSize / 2; y < centerY + detectionAreaSize / 2; y++) {
            for (int x = centerX - detectionAreaSize / 2; x < centerX + detectionAreaSize / 2; x++) {
                COLORREF color = GetPixel(hdcScreen, x, y);
                if (isColorMatch(color, targetColor, tolerance)) {
                    foundColor = true; // Color found, trigger click
                    break; // Exit loops once color is detected
                }
            }
            if (foundColor) break;
        }

        // Trigger click if color was detected in the area
        if (foundColor) {
            simulateLeftClick();
            Beep(1000, 100); // Beep to confirm detection (optional)
        }

        Sleep(5); // Check every 10 ms original 10
    }

    ReleaseDC(NULL, hdcScreen); // Release the device context
}

int main() {
    std::cout << "Starting color detection in the center 8x8 area...\n";

    // Start the color detection loop
    detectColorAndClick();

    return 0;
}


 */

/* VERSION 2 SUPER GOOD BUT NO PAUSE KEY

#include <Windows.h>
#include <iostream>
#include <thread>
#include <cmath>

 // Target color and tolerance
const COLORREF targetColor = RGB(255, 255, 35); // The color to detect
const int tolerance = 60; // Tolerance for color matching

// Center detection area (8x8 pixels)
const int centerX = 1920 / 2;
const int centerY = 1080 / 2;
const int detectionAreaSize = 8; // Size of detection area

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

// Function to simulate a left mouse click with a 100 ms hold
void simulateLeftClick() {
    INPUT inputs[2] = {};

    // Mouse down event
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;

    // Mouse up event
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;

    // Send mouse down event
    SendInput(1, &inputs[0], sizeof(INPUT));
    Sleep(25); // Hold down for 100 ms
    SendInput(1, &inputs[1], sizeof(INPUT)); // Send mouse up event
}

// Function to detect color in the center area and trigger a click if detected
void detectColorAndClick() {
    HDC hdcScreen = GetDC(NULL); // Get the screen device context

    while (true) {
        bool foundColor = false; // Flag to track color detection

        // Check the 8x8 area at the center of the screen
        for (int y = centerY - detectionAreaSize / 2; y < centerY + detectionAreaSize / 2; y++) {
            for (int x = centerX - detectionAreaSize / 2; x < centerX + detectionAreaSize / 2; x++) {
                COLORREF color = GetPixel(hdcScreen, x, y);
                if (isColorMatch(color, targetColor, tolerance)) {
                    foundColor = true; // Color found, trigger click
                    break; // Exit loops once color is detected
                }
            }
            if (foundColor) break;
        }

        // Trigger click if color was detected in the area
        if (foundColor) {
            simulateLeftClick();
            Beep(1000, 50); // Beep to confirm detection (optional)
        }

        Sleep(10); // Check every 10 ms
    }

    ReleaseDC(NULL, hdcScreen); // Release the device context
}

int main() {
    std::cout << "Starting color detection in the center 8x8 area...\n";

    // Start the color detection loop
    detectColorAndClick();

    return 0;
}



*/


/* single color working version perfectly
* 
* 
#include <Windows.h>
#include <iostream>
#include <thread>
#include <atomic>
#include <cmath>

// Target color and tolerance
const COLORREF targetColor = RGB(255, 255, 35); // The color to detect
const int tolerance = 60; // Tolerance for color matching

// Center detection area (8x8 pixels)
const int centerX = 1920 / 2;
const int centerY = 1080 / 2;
const int detectionAreaSize = 8; // Size of detection area

// Global atomic variable to control pause/resume
std::atomic<bool> detectionActive(true); // Start active by default

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

// Function to simulate a left mouse click with a 100 ms hold
void simulateLeftClick() {
    INPUT inputs[2] = {};

    // Mouse down event
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;

    // Mouse up event
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;

    // Send mouse down event
    SendInput(1, &inputs[0], sizeof(INPUT));
    Sleep(100); // Hold down for 100 ms
    SendInput(1, &inputs[1], sizeof(INPUT)); // Send mouse up event
}

// Function to detect color in the center area and trigger a click if detected
void detectColorAndClick() {
    HDC hdcScreen = GetDC(NULL); // Get the screen device context

    while (true) {
        if (detectionActive) { // Only detect color if detection is active
            bool foundColor = false; // Flag to track color detection

            // Check the 8x8 area at the center of the screen
            for (int y = centerY - detectionAreaSize / 2; y < centerY + detectionAreaSize / 2; y++) {
                for (int x = centerX - detectionAreaSize / 2; x < centerX + detectionAreaSize / 2; x++) {
                    COLORREF color = GetPixel(hdcScreen, x, y);
                    if (isColorMatch(color, targetColor, tolerance)) {
                        foundColor = true; // Color found, trigger click
                        break; // Exit loops once color is detected
                    }
                }
                if (foundColor) break;
            }

            // Trigger click if color was detected in the area
            if (foundColor) {
                simulateLeftClick();
                Beep(1000, 50); // Beep to confirm detection (optional)
            }
        }

        Sleep(10); // Check every 10 ms
    }

    ReleaseDC(NULL, hdcScreen); // Release the device context
}

// Function to handle key press detection
void monitorKeyPress() {
    while (true) {
        // Check if the '-' key is pressed
        if (GetAsyncKeyState(VK_OEM_MINUS) & 0x8000) { // VK_OEM_MINUS is the virtual key code for '-'
            detectionActive = !detectionActive; // Toggle the detection state
            Beep(detectionActive ? 750 : 500, 100); // Beep to indicate toggling (750 Hz for resume, 500 Hz for pause)
            Sleep(300); // Debounce delay to prevent rapid toggling
        }
        Sleep(50); // Check every 50 ms
    }
}

int main() {
    std::cout << "Starting color detection in the center 8x8 area...\n";
    std::cout << "Press '-' to toggle pause/resume.\n";

    // Start the color detection and key press monitor in separate threads
    std::thread detectionThread(detectColorAndClick);
    std::thread keyPressThread(monitorKeyPress);

    // Wait for threads to complete
    detectionThread.join();
    keyPressThread.join();

    return 0;
}


*/



#include <Windows.h>
#include <iostream>
#include <thread>
#include <atomic>
#include <cmath>

// Target colors and tolerance
const COLORREF targetColor1 = RGB(255, 255, 35); // First color to detect
const COLORREF targetColor2 = RGB(96, 50, 43);   // Second color to detect
const int tolerance = 40; // Tolerance for color matching

// Center detection area (8x8 pixels)
const int centerX = 1920 / 2;
const int centerY = 1080 / 2;
const int detectionAreaSize = 6; // Size of detection area

// Global atomic variable to control pause/resume
std::atomic<bool> detectionActive(true); // Start active by default

// Check if a color is within tolerance of the target colors
bool isColorMatch(COLORREF color, const COLORREF targetColor, int tolerance) {
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

// Function to simulate a left mouse click with a 100 ms hold
void simulateLeftClick() {
    INPUT inputs[2] = {};

    // Mouse down event
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;

    // Mouse up event
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;

    // Send mouse down event
    SendInput(1, &inputs[0], sizeof(INPUT));
    Sleep(100); // Hold down for 100 ms
    SendInput(1, &inputs[1], sizeof(INPUT)); // Send mouse up event
}

// Function to detect color in the center area and trigger a click if detected
void detectColorAndClick() {
    HDC hdcScreen = GetDC(NULL); // Get the screen device context

    while (true) {
        if (detectionActive) { // Only detect color if detection is active
            bool foundColor = false; // Flag to track color detection

            // Check the 6x6 area at the center of the screen
            for (int y = centerY - detectionAreaSize / 2; y < centerY + detectionAreaSize / 2; y++) {
                for (int x = centerX - detectionAreaSize / 2; x < centerX + detectionAreaSize / 2; x++) {
                    COLORREF color = GetPixel(hdcScreen, x, y);
                    // Check both colors for a match
                    if (isColorMatch(color, targetColor1, tolerance) ||
                        isColorMatch(color, targetColor2, tolerance)) {
                        foundColor = true; // Color found, trigger click
                        break; // Exit loops once color is detected
                    }
                }
                if (foundColor) break;
            }

            // Trigger click if color was detected in the area
            if (foundColor) {
                simulateLeftClick();
                Beep(1000, 50); // Beep to confirm detection (optional)
            }
        }

        Sleep(10); // Check every 10 ms
    }

    ReleaseDC(NULL, hdcScreen); // Release the device context
}

// Function to handle key press detection
void monitorKeyPress() {
    while (true) {
        // Check if the '-' key is pressed
        if (GetAsyncKeyState(VK_OEM_MINUS) & 0x8000) { // VK_OEM_MINUS is the virtual key code for '-'
            detectionActive = !detectionActive; // Toggle the detection state
            Beep(detectionActive ? 750 : 500, 100); // Beep to indicate toggling (750 Hz for resume, 500 Hz for pause)
            Sleep(300); // Debounce delay to prevent rapid toggling
        }
        Sleep(50); // Check every 50 ms
    }
}

int main() {
    printf("Starting color detection in the center %dx%d area...\n", detectionAreaSize, detectionAreaSize);
    std::cout << "Press '-' to toggle pause/resume.\n";

    // Start the color detection and key press monitor in separate threads
    std::thread detectionThread(detectColorAndClick);
    std::thread keyPressThread(monitorKeyPress);

    // Wait for threads to complete
    detectionThread.join();
    keyPressThread.join();

    return 0;
}
