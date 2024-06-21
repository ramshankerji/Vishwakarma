// HelloWindowsDesktop.cpp : The beginning of Vishwakarma Desktop Application.
// compile with: /D_UNICODE /DUNICODE /DWIN32 /D_WINDOWS /c

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <tchar.h>
#include "ft2build.h"
#include FT_FREETYPE_H
#include <iostream>
#include <vishwakarma-2D.h>
#include <random>

// Global variables

// The main window class name.
static TCHAR szWindowClass[] = _T("DesktopApp");

// The string that appears in the application's title bar.
static TCHAR szTitle[] = _T("Vishwakarma 0 :-) ");

// Stored instance handle for use in Win32 API calls such as FindResource
HINSTANCE hInst;

// Forward declarations of functions included in this code module:
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
// Forward declaration of the RenderText function
void RenderText(HDC hdc, FT_Face face, const char* text, int x, int y);


int WINAPI WinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPSTR     lpCmdLine,
    _In_ int       nCmdShow
)
{
    WNDCLASSEX wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(wcex.hInstance, IDI_APPLICATION);
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName = NULL;
    wcex.lpszClassName = szWindowClass;
    wcex.hIconSm = LoadIcon(wcex.hInstance, IDI_APPLICATION);

    if (!RegisterClassEx(&wcex))
    {
        MessageBox(NULL,
            _T("Call to RegisterClassEx failed!"),
            _T("Windows Desktop Guided Tour"),
            NULL);

        return 1;
    }

    // Define the window style without WS_CAPTION, but include WS_THICKFRAME and WS_SYSMENU
    DWORD windowStyle = WS_OVERLAPPED | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;

    // Store instance handle in our global variable
    hInst = hInstance;

    HWND hWnd = CreateWindowEx(
        WS_EX_OVERLAPPEDWINDOW,  //An optional extended window style.
        szWindowClass,           // Window class: The name of the application
        szTitle,       // The text that appears in the title bar
        // The type of window to create
        WS_OVERLAPPEDWINDOW | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
        // Size and position
        CW_USEDEFAULT, CW_USEDEFAULT, // Initial position (x, y)
        1280, 720, //Corresponds to 720p screen resolution. We expect at least this much. :)
        NULL,      // The parent of this window
        NULL,      // This application does not have a menu bar, we create our own Menu.
        hInstance, // Instance handle, the first parameter from WinMain
        NULL       // Additional application data, not used in this application
    );

    if (!hWnd)
    {
        MessageBox(NULL,
            _T("Call to CreateWindow failed!"),
            _T("Windows Desktop Guided Tour"),
            NULL);

        return 1;
    }

    // The parameters to ShowWindow explained:
    // hWnd: the value returned from CreateWindow
    // nCmdShow: the fourth parameter from WinMain
    ShowWindow(hWnd,
        nCmdShow);
    UpdateWindow(hWnd);

    FT_Library ft;
    FT_Face face;

    if (FT_Init_FreeType(&ft)) {
        std::cerr << "Could not initialize FreeType library" << std::endl;
        return -1;
    }

    if (FT_New_Face(ft, "C:\\Windows\\Fonts\\arial.ttf", 0, &face)) {
        std::cerr << "Could not open font" << std::endl;
        return -1;
    }

    FT_Set_Pixel_Sizes(face, 0, 48); // Set font size to 48 pixels high

    // Main message loop:
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    //Cleanup Freetype library.
    FT_Done_Face(face);
    FT_Done_FreeType(ft);

    return (int)msg.wParam;
}

//  FUNCTION: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  PURPOSE:  Processes messages for the main window.
//
//  WM_PAINT    - Paint the main window
//  WM_DESTROY  - post a quit message and return
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    PAINTSTRUCT ps;
    HDC hdc;
    TCHAR greeting[] = _T("Hello, Vishwakarma!");

    static FT_Face face;
    static const char* text = "Hello, Vishwakarma!";
    static bool initialized = false;

    switch (message)
    {
     /*
    case WM_NCCALCSIZE: //Override the WM_NCCALCSIZE message to extend the client area into the title bar space.
        if (wParam == TRUE) {
            // Extend the client area to cover the title bar
            NCCALCSIZE_PARAMS* pncsp = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            pncsp->rgrc[0].top -= GetSystemMetrics(SM_CYCAPTION);
            return 0;
        }
        break;
        */
    case WM_LBUTTONDOWN: {
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);
        //MessageBox(hWnd, L"Left button clicked", L"Mouse Click", MB_OK);
        return 0;
    }
    case WM_RBUTTONDOWN: {
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);
        //MessageBox(hWnd, L"Right button clicked", L"Mouse Click", MB_OK);
        return 0;
    }
    case WM_MBUTTONDOWN: {
        MessageBox(hWnd, L"Middle button clicked", L"Mouse Click", MB_OK);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (zDelta > 0) {
            MessageBox(hWnd, L"Mouse wheel scrolled up", L"Mouse Scroll", MB_OK);
        }
        else {
            MessageBox(hWnd, L"Mouse wheel scrolled down", L"Mouse Scroll", MB_OK);
        }
        return 0;
    }
    case WM_PAINT:
    {
        hdc = BeginPaint(hWnd, &ps);

        // Here your application is laid out.
        // For this introduction, we just print out "Hello, Vishwakarma!"
        // in the top left corner.
        TextOut(hdc,
            5, 5,
            greeting, _tcslen(greeting));
        // End application-specific layout section.

        // Initialize FreeType face only once
        if (!initialized) {
            FT_Library ft;
            if (FT_Init_FreeType(&ft)) {
                std::cerr << "Could not initialize FreeType library" << std::endl;
                return -1;
            }

            if (FT_New_Face(ft, "C:\\Windows\\Fonts\\arial.ttf", 0, &face)) {
                std::cerr << "Could not open font" << std::endl;
                return -1;
            }

            FT_Set_Pixel_Sizes(face, 0, 48); // Set font size to 48 pixels high
            initialized = true;
        }

        // Get the dimensions of the client area
        RECT rect;
        GetClientRect(hWnd, &rect);

        // Calculate the position to start drawing the text to center it
        int x = (rect.right - rect.left) / 2;
        int y = (rect.bottom - rect.top) / 2;

        // Calculate width of the text to properly center it
        int text_width = 0;
        for (const char* p = text; *p; p++) {
            if (FT_Load_Char(face, *p, FT_LOAD_RENDER)) {
                continue; // Ignore errors
            }
            text_width += face->glyph->advance.x >> 6;
        }
        x -= text_width / 2; // Adjust x to start text from the center

        RenderText(hdc, face, text, x, y);

        // Create a random device and a random number generator
        std::random_device rd;  // Obtain a random number from hardware
        std::mt19937 gen(rd()); // Seed the generator

        // Define the distribution range
        std::uniform_int_distribution<> distr(100, 500);

        Draw2DLine(hdc, 100, 100, 500, 500); //The limits.
        for (int i = 0; i < 100; i++)
        {
            Draw2DLine(hdc, distr(gen), distr(gen), distr(gen), distr(gen));
        }
        
        EndPaint(hWnd, &ps);
        break;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
        break;
    }

    return 0;
}

void RenderText(HDC hdc, FT_Face face, const char* text, int x, int y) {
    FT_GlyphSlot g = face->glyph;

    for (const char* p = text; *p; p++) {
        if (FT_Load_Char(face, *p, FT_LOAD_RENDER)) {
            continue; // Ignore errors
        }

        // Draw the character here
        for (int row = 0; row < g->bitmap.rows; ++row) {
            for (int col = 0; col < g->bitmap.width; ++col) {
                if (g->bitmap.buffer[row * g->bitmap.width + col]) {
                    SetPixel(hdc, x + col + g->bitmap_left, y + row - g->bitmap_top, RGB(0, 0, 0));
                }
            }
        }

        x += g->advance.x >> 6; // Advance to the next character
    }
}
