// tiling_demo_colours.cpp : Defines the entry point for the application.
//

#include "framework.h"
#include "tiling_demo_colours.h"
#include "..\SimpleTiling\SimpleTiling.h"
#undef min
#undef max
#include <chrono>

#define MAX_LOADSTRING 100

// Global Variables:
HINSTANCE hInst;                                // current instance
WCHAR szTitle[MAX_LOADSTRING];                  // The title bar text
WCHAR szWindowClass[MAX_LOADSTRING];            // the main window class name

// Forward declarations of functions included in this code module:
ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);

static constexpr uint32_t window_width = 1920, window_height = 1080;

#define NUM_TILE_THREADS 8

static float thread_times[NUM_TILE_THREADS] = {};

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // Initialize global strings
    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_TILINGDEMOCOLOURS, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    // Perform application initialization:
    if (!InitInstance (hInstance, nCmdShow))
    {
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_TILINGDEMOCOLOURS));

    MSG msg;

    // Main message loop:
    bool frame_issued = false;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        simple_tiling::submit_update_work([](uint32_t tile_ndx)
        {
            thread_times[tile_ndx] += 0.001f;
        });

        simple_tiling::submit_draw_work([](__m256 pixels, uint32_t threadID, simple_tiling_utils::color_batch* colors_out)
        {
#define TEST_ANIMATION
//#define TEST_ANIMATION_MONOCHROME
//#define TEST_RGB
//#define TEST_PIXEL_XOR
#ifdef TEST_RGB
            colors_out->colors8bpc[0] = 0xff0000ff;
            colors_out->colors8bpc[1] = 0xff00ff00;
            colors_out->colors8bpc[2] = 0xffff0000;
            colors_out->colors8bpc[3] = 0xffffffff;
            colors_out->colors8bpc[4] = 0xff0000ff;
            colors_out->colors8bpc[5] = 0xf000ff00;
            colors_out->colors8bpc[6] = 0xffff0000;
            colors_out->colors8bpc[7] = 0xffffffff;
#elif defined (TEST_PIXEL_XOR)
            auto wvec = _mm256_set1_ps(window_width);
            auto yvec = _mm256_floor_ps(_mm256_div_ps(pixels, wvec));
            auto xvec = _mm256_sub_ps(pixels, _mm256_mul_ps(yvec, wvec));
            auto rgb_vec = _mm256_xor_ps(xvec, yvec);

            // Not super accurate, but very fast
            memcpy(colors_out, &rgb_vec, sizeof(__m256));

            // Memcpy doesn't really preserve pixel colors - should find a way to run this snippet
            // using AVX2 intrinsics for algorithms that need more precise control
            //for (uint32_t i = 0; i < NUM_VECTOR_LANES; i++)
            //{
            //    colors_out->colors8bpc[i] = v_access(rgb_vec)[i] * 255.5f;
            //}

            // slo but more predictable than straight memcpy, useful for debugging
            //rgb_vec = _mm256_mul_ps)(rgb_vec, _mm256_set1_ps)(256.0f));
            //for (uint32_t i = 0; i < NUM_VECTOR_LANES; i++)
            //{
            //    colors_out->colors8bpc[i] = v_access(rgb_vec)[i];
            //}

#elif defined (TEST_ANIMATION)
            // Load time
            const auto tvec = _mm256_set1_ps(thread_times[threadID]);

            // Load other useful constants
            const auto wvec = _mm256_set1_ps(window_width);
            const auto hvec = _mm256_set1_ps(window_height);

            // Compute pixel coordinates
            const auto yvec = _mm256_floor_ps(_mm256_div_ps(pixels, wvec));
            const auto xvec = _mm256_sub_ps(pixels, _mm256_mul_ps(yvec, wvec));
            const auto u_vec = _mm256_div_ps(xvec, wvec);
            const auto v_vec = _mm256_div_ps(yvec, hvec);

            // Colors :)
            // Higher performance is possible with cosine lookup tables and other tricks, but inevitably introduces screen-tearing as
            // the refresh rate outpaces the draw-rate of the monitor, even with the locked framerates I have below
            // I think the framerate I'm getting here is good enough to demo with ^_^'
            const auto point5_vec = _mm256_set1_ps(0.5f);
            auto red_vec = _mm256_mul_ps(point5_vec, _mm256_cos_ps(_mm256_add_ps(tvec, u_vec)));
            red_vec = _mm256_add_ps(red_vec, point5_vec);

            auto blue_vec = _mm256_mul_ps(point5_vec, _mm256_cos_ps(_mm256_add_ps(tvec, v_vec)));
            blue_vec = _mm256_add_ps(blue_vec, point5_vec);

            // Export
            for (uint32_t i = 0; i < NUM_VECTOR_LANES; i++)
            {
                colors_out->colors8bpc[i] = uint32_t(v_access(red_vec)[i] * 255.5f) | (uint32_t(v_access(blue_vec)[i] * 255.5f) << 8) |
                    (255 << 16) | (255 << 24);
            }
#elif defined(TEST_ANIMATION_MONOCHROME)
            const auto tvec = _mm256_set1_ps(thread_times[threadID]);
            const auto sinvec = _mm256_mul_ps(_mm256_add_ps(_mm256_sin_ps(tvec), _mm256_set1_ps(1.0f)), _mm256_set1_ps(0.5f));
            for (uint32_t i = 0; i < NUM_VECTOR_LANES; i++)
            {
                const uint32_t c = static_cast<uint32_t>(v_access(sinvec)[i] * 255.5f);
                colors_out->colors8bpc[i] = c | (c << 8) | (c << 16) | (255 << 24);
            }
#endif
        });

        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            InvalidateRect(msg.hwnd, NULL, false);
        }
    }
    simple_tiling::shutdown();
    return (int) msg.wParam;
}



//
//  FUNCTION: MyRegisterClass()
//
//  PURPOSE: Registers the window class.
//
ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style          = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wcex.lpfnWndProc    = WndProc;
    wcex.cbClsExtra     = 0;
    wcex.cbWndExtra     = 0;
    wcex.hInstance      = hInstance;
    wcex.hIcon          = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_TILINGDEMOCOLOURS));
    wcex.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground  = (HBRUSH)(COLOR_WINDOW+1);
    wcex.lpszMenuName   = MAKEINTRESOURCEW(IDC_TILINGDEMOCOLOURS);
    wcex.lpszClassName  = szWindowClass;
    wcex.hIconSm        = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
}

//
//   FUNCTION: InitInstance(HINSTANCE, int)
//
//   PURPOSE: Saves instance handle and creates main window
//
//   COMMENTS:
//
//        In this function, we save the instance handle in a global variable and
//        create and display the main program window.
//
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
   hInst = hInstance; // Store instance handle in our global variable

   HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW,
      0, 0, window_width, window_height, nullptr, nullptr, hInstance, nullptr);

   if (!hWnd)
   {
      return FALSE;
   };

   // Required to be initialized early, since [ShowWindow] will invoke WM_PAINT -> ::win_paint, which depeends on
   // a valid BITMAPINFO being defined for copy-outs
   simple_tiling::setup(NUM_TILE_THREADS, window_width, window_height, true);

   ShowWindow(hWnd, nCmdShow);
   UpdateWindow(hWnd);

   return TRUE;
}

//  FUNCTION: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  PURPOSE: Processes messages for the main window.
//
//  WM_COMMAND  - process the application menu
//  WM_PAINT    - Paint the main window
//  WM_DESTROY  - post a quit message and return
//
//
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_COMMAND:
        {
            int wmId = LOWORD(wParam);
            // Parse the menu selections:
            switch (wmId)
            {
            case IDM_ABOUT:
                DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
                break;
            case IDM_EXIT:
                DestroyWindow(hWnd);
                break;
            default:
                return DefWindowProc(hWnd, message, wParam, lParam);
            }
        }
        break;
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            simple_tiling::win_paint(hdc, 16);
            EndPaint(hWnd, &ps);
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// Message handler for about box.
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}
