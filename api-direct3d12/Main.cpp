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
#include <png.h>

#pragma comment(lib, "libpng16_staticd.lib")
#pragma comment(lib, "zlibd.lib")

std::wstring GetExecutablePath() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(NULL, buffer, MAX_PATH);
    std::wstring::size_type pos = std::wstring(buffer).find_last_of(L"\\/");

    return std::wstring(buffer).substr(0, pos);
}

void LoadPngImage(const char* filename, unsigned char** image_data, int* width, int* height)
{
    #pragma warning(disable: 4996)
    FILE* fp = fopen(filename, "rb");
    if (!fp) abort();

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) abort();

    png_infop info = png_create_info_struct(png);
    if (!info) abort();

    if (setjmp(png_jmpbuf(png))) abort();

    png_init_io(png, fp);

    png_read_info(png, info);

    *width = png_get_image_width(png, info);
    *height = png_get_image_height(png, info);
    png_byte color_type = png_get_color_type(png, info);
    png_byte bit_depth = png_get_bit_depth(png, info);

    if (bit_depth == 16)
        png_set_strip_16(png);

    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);

    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);

    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);

    if (color_type == PNG_COLOR_TYPE_RGB ||
        color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);

    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);

    png_read_update_info(png, info);

    *image_data = (unsigned char*)malloc(png_get_rowbytes(png, info) * (*height));
    png_bytep* row_pointers = (png_bytep*)malloc(sizeof(png_bytep) * (*height));
    for (int y = 0; y < *height; y++) {
        row_pointers[y] = (*image_data) + y * png_get_rowbytes(png, info);
    }

    png_read_image(png, row_pointers);

    fclose(fp);
    free(row_pointers);
    png_destroy_read_struct(&png, &info, NULL);
    
}

void DisplayImage(HDC hdc, const unsigned char* image_data, int width, int height)
{
    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    StretchDIBits(hdc, 0, 0, width, height, 0, 0, width, height, image_data, &bmi, DIB_RGB_COLORS, SRCCOPY);
}

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
    static const char* uiText001 = "Vishwakarma!";
    static const char* uiText002 = "Name";
    static const char* uiText003 = "Level 21";
    static const char* uiText004 = "Create New";
    static const char* uiText005 = "2D Drawing";
    static const char* uiText006 = "2D P&ID";
    static const char* uiText007 = "2D SLD";
    static const char* uiText008 = "3D Pipes";
    static const char* uiText009 = "3D Structure";
    static const char* uiText010 = "Pressure Vessel";
    static const char* uiText011 = "Heat Exchanger";
    static const char* uiText016 = "3D Gen. Eq.";
    static const char* uiText017 = "Filter";
    static const char* uiText018 = "Air Cooler";
    static const char* uiText019 = "Compressor";
    static const char* uiText020 = "Pump";

    static const char* uiText012 = "Recently Opened files";
    static const char* uiText013 = "Extensions";
    static const char* uiText014 = "Training";
    static const char* uiText015 = "Medals";

    static bool initialized = false;

    static unsigned char* image_data = NULL;
    static int width, height;
    std::wstring exePath = GetExecutablePath();
    int size_needed = WideCharToMultiByte(
        CP_UTF8, 0, &exePath[0], static_cast<int>(exePath.size()), NULL, 0, NULL, NULL);
    std::string str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &exePath[0], static_cast<int>(exePath.size()), &str[0], size_needed, NULL, NULL);

    std::string fullPath = str + "\\logo.png";

    switch (message)
    {
     /*
    case WM_NCCALCSIZE: //Override the WM_NCCALCSIZE message to extend the client area into the title bar space.
        if (wParam == TRUE) {
            // Extend the client area to cover the title bar
            NCCALCSIZE_PARAMS* pncsp = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            pncsp->rgrc[0].top -= GetSystemMetrics(SM_CYCAPTION);
            return 0;
        },,,,
        break;
        */
    case WM_CREATE:
        
        std::cout << "Full path to logo.png: " << fullPath << std::endl;

        LoadPngImage(fullPath.c_str(), &image_data, &width, &height);
        //LoadPngImage("C:\\RAM\\CODE\\Vishwakarma\\api-direct3d12\\x64\\Debug\\logo.png", &image_data, &width, &height);
        break;

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
        // For this introduction, we just print out "Hello, Vishwakarma!" in the top left corner.
        // TextOut(hdc, 5, 5, greeting, _tcslen(greeting));
        // We are no longer using TextOut function to render text, instead we use Freetype.

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

            FT_Set_Pixel_Sizes(face, 0, 24); // Set font size to 48 pixels high
            initialized = true;
        }

        // Get the dimensions of the client area
        RECT rect;
        GetClientRect(hWnd, &rect);

        // Calculate the position to start drawing the text to center it
        int x = (rect.right - rect.left) / 2;
        int y = (rect.bottom - rect.top) / 2;

        RenderText(hdc, face, uiText001, 10, 20);

        if (1) // Homepage of the application.
        {
            RenderText(hdc, face, uiText002, 10, 60);
            RenderText(hdc, face, uiText003, 10, 80);
            RenderText(hdc, face, uiText015, 10, 120);

            RenderText(hdc, face, uiText004, 200, 80);

            RenderText(hdc, face, uiText005, 200, 120);
            RenderText(hdc, face, uiText006, 400, 120);
            RenderText(hdc, face, uiText007, 600, 120);

            RenderText(hdc, face, uiText008, 200, 220);
            RenderText(hdc, face, uiText009, 400, 220);
            RenderText(hdc, face, uiText016, 600, 220);
            RenderText(hdc, face, uiText010, 200, 320);
            RenderText(hdc, face, uiText011, 400, 320);
            RenderText(hdc, face, uiText017, 600, 320);
            RenderText(hdc, face, uiText018, 200, 420);
            RenderText(hdc, face, uiText019, 400, 420);
            RenderText(hdc, face, uiText020, 600, 420);

            RenderText(hdc, face, uiText012, 200, 520);

            RenderText(hdc, face, uiText013, 1000, 60);
            RenderText(hdc, face, uiText014, 1000, 320);
        }

        if (0) //2D Drafting module
        {
            RenderText(hdc, face, "I am 2D Drafting Module", 200, 80);

            RenderText(hdc, face, "Line", 200, 320);
            Draw2DLine(hdc, 150, 250, 250, 150);

            RenderText(hdc, face, "Polyline", 400, 320);
            Draw2DLine(hdc, 350, 250, 400, 200);
            Draw2DLine(hdc, 400, 200, 450, 250);
            Draw2DLine(hdc, 450, 250, 500, 150);

            RenderText(hdc, face, "Triangle", 600, 320);
            Draw2DLine(hdc, 650, 250, 700, 150);
            Draw2DLine(hdc, 700, 150, 750, 250);
            Draw2DLine(hdc, 750, 250, 650, 250);

            RenderText(hdc, face, "Rectangle", 200, 620);
            Draw2DLine(hdc, 150, 550, 150, 450);
            Draw2DLine(hdc, 150, 450, 300, 450);
            Draw2DLine(hdc, 300, 450, 300, 550);
            Draw2DLine(hdc, 300, 550, 150, 550);

            RenderText(hdc, face, "Circle", 400, 620);

            RenderText(hdc, face, "Text", 600, 620);
            RenderText(hdc, face, "EIL :-)", 600, 520);
            
        }

        // Create a random device and a random number generator
        std::random_device rd;  // Obtain a random number from hardware
        std::mt19937 gen(rd()); // Seed the generator

        // Define the distribution range
        std::uniform_int_distribution<> distr(100, 500);
        
        if (image_data)
        {
            //DisplayImage(hdc, image_data, width, height);
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
