// tiling_demo_colours.cpp : Defines the entry point for the application.
//

#include "framework.h"
#include "tiling_demo_colours.h"
#include "..\SimpleTiling\SimpleTiling.h"
#undef min
#undef max
#include "..\ThirdParty\tracy-0.8\Tracy.hpp"
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

static float lastTime = 0.0;

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
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        const auto clock = std::chrono::steady_clock::now();
        double t = static_cast<double>(clock.time_since_epoch().count());
        t *= 1.0e-9;
        lastTime = static_cast<float>(t);

        {
            ZoneNamed(drawZone, "Draw submission");
            simple_tiling::submit_draw_work([](simple_tiling_utils::v_type pixels, simple_tiling_utils::color_batch* colors_out)
                {
#define TEST_ANIMATION
//#define TEST_RGB
#ifdef TEST_RGB
#if (NUM_VECTOR_LANES == 4)
                    colors_out->colors8bpc[0] = 0xff0000ff;
                    colors_out->colors8bpc[1] = 0xff00ff00;
                    colors_out->colors8bpc[2] = 0xffff0000;
                    colors_out->colors8bpc[3] = 0xffffffff;
#elif (NUM_VECTOR_LANES == 8)
                    colors_out->colors8bpc[0] = 0xff0000ff;
                    colors_out->colors8bpc[1] = 0xff00ff00;
                    colors_out->colors8bpc[2] = 0xffff0000;
                    colors_out->colors8bpc[3] = 0xffffffff;
                    colors_out->colors8bpc[4] = 0xff0000ff;
                    colors_out->colors8bpc[5] = 0xf000ff00;
                    colors_out->colors8bpc[6] = 0xffff0000;
                    colors_out->colors8bpc[7] = 0xffffffff;
#endif
#elif defined (TEST_PIXEL_XOR)
                    auto wvec = v_op(set1_ps)(window_width);
                    auto yvec = v_op(floor_ps)(v_op(div_ps)(pixels, wvec));
                    auto xvec = v_op(sub_ps)(pixels, v_op(mul_ps)(yvec, wvec));
                    auto rgb_vec = v_op(xor_ps)(xvec, yvec);
                    //v_op(div_ps)(xvec, wvec);

      // Not super accurate, but very fast
                    memcpy(colors_out, &rgb_vec, sizeof(simple_tiling_utils::v_type));

                    // Memcpy doesn't really preserve pixel colors - should find a way to run this snippet
                    // using AVX2 intrinsics for algorithms that need more precise control
                    //for (uint32_t i = 0; i < NUM_VECTOR_LANES; i++)
                    //{
                    //    colors_out->colors8bpc[i] = v_access(rgb_vec)[i] * 255.5f;
                    //}

                    // slo but more predictable than straight memcpy, useful for debugging
                    //rgb_vec = v_op(mul_ps)(rgb_vec, v_op(set1_ps)(256.0f));
                    //for (uint32_t i = 0; i < NUM_VECTOR_LANES; i++)
                    //{
                    //    colors_out->colors8bpc[i] = v_access(rgb_vec)[i];
                    //}

#elif defined (TEST_ANIMATION)
                    // Load time
                    const auto tvec = v_op(set1_ps)(lastTime);

                    // Load other useful constants
                    const auto wvec = v_op(set1_ps)(window_width);
                    const auto hvec = v_op(set1_ps)(window_height);

                    // Compute pixel coordinates
                    const auto yvec = v_op(floor_ps)(v_op(div_ps)(pixels, wvec));
                    const auto xvec = v_op(sub_ps)(pixels, v_op(mul_ps)(yvec, wvec));
                    const auto u_vec = v_op(div_ps)(xvec, wvec);
                    const auto v_vec = v_op(div_ps)(yvec, hvec);

                    // Colors :)
                    // vec3 col = 0.5 + 0.5 * cos(iTime + uv.xy);
                    const auto point5_vec = v_op(set1_ps)(0.5f);
                    auto red_vec = v_op(mul_ps)(point5_vec, v_op(cos_ps)(v_op(add_ps)(tvec, u_vec)));
                    red_vec = v_op(add_ps)(red_vec, point5_vec);

                    auto blue_vec = v_op(mul_ps)(point5_vec, v_op(cos_ps)(v_op(add_ps)(tvec, v_vec)));
                    blue_vec = v_op(add_ps)(blue_vec, point5_vec);

                    // Export
                    for (uint32_t i = 0; i < NUM_VECTOR_LANES; i++)
                    {
                        colors_out->colors8bpc[i] = uint32_t(v_access(red_vec)[i] * 255.5f) | (uint32_t(v_access(blue_vec)[i] * 255.5f) << 8) |
                            (255 << 16) | (255 << 24);
                    }
#endif
                });
        }
        {
            ZoneNamed(swapZone, "Tile swaps", true);
            simple_tiling::swap_tile_buffers();
        }
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            InvalidateRect(msg.hwnd, NULL, false);
        }
        FrameMark;
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
   simple_tiling::setup(8, window_width, window_height);

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
            ZoneScoped;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() % 15 == 0) // 15 millisecond vsync
            {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hWnd, &ps);
                simple_tiling::win_paint(hdc);
                EndPaint(hWnd, &ps);
            }
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
