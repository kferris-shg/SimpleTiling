// tiling_demo_colours.cpp : Defines the entry point for the application.
//

#include "framework.h"
#include "tiling_demo_raymarching.h"
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

static constexpr uint32_t window_width = 640, window_height = 480;

#define NUM_TILE_THREADS 32

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
    LoadStringW(hInstance, IDC_TILINGDEMORAYMARCHING, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    // Perform application initialization:
    if (!InitInstance(hInstance, nCmdShow))
    {
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_TILINGDEMORAYMARCHING));

    MSG msg;

    // Main message loop:
    bool frame_issued = false;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        simple_tiling::submit_draw_work([](__m256 pixels, uint32_t threadID, simple_tiling_utils::color_batch* colors_out)
        {
            // Minimal raymarcher
            /////////////////////

            // Useful numbers
            auto wvec = _mm256_set1_ps(window_width);
            auto hvec = _mm256_set1_ps(window_height);
            
            // Camera ray directions
            auto yvec = _mm256_floor_ps(_mm256_div_ps(pixels, wvec));
            auto xvec = _mm256_sub_ps(pixels, _mm256_mul_ps(yvec, wvec));

            yvec = _mm256_sub_ps(yvec, _mm256_mul_ps(yvec, _mm256_set1_ps(0.5f)));
            xvec = _mm256_sub_ps(xvec, _mm256_mul_ps(wvec, _mm256_set1_ps(0.5f)));
            auto zvec = _mm256_div_ps(wvec, _mm256_tan_ps(_mm256_set1_ps(1.62f * 0.5f)));

            // Ray-marching utility variables
            auto eps = _mm256_set1_ps(0.001f);
            auto maxDist = _mm256_set1_ps(0.001f);
            auto traceDistX = _mm256_set1_ps(0.0f);
            auto traceDistY = _mm256_set1_ps(0.0f);
            auto traceDistZ = _mm256_set1_ps(0.0f);

            auto laneState = _mm256_set1_epi32(INT_MAX);   
            const auto laneLiveState = _mm256_set1_epi32(0);

            // Camera position
            // Starting at the origin for now, keeping things simple
            auto camPosX = _mm256_set1_ps(0.0f);
            auto camPosY = _mm256_set1_ps(0.0f);
            auto camPosZ = _mm256_set1_ps(0.0f);

            auto vlen = [](__m256 xv, __m256 yv, __m256 zv)
            {
                auto xSqr = _mm256_mul_ps(xv, xv);
                auto ySqr = _mm256_mul_ps(yv, yv);
                auto zSqr = _mm256_mul_ps(zv, zv);

                return _mm256_sqrt_ps(_mm256_add_ps(_mm256_add_ps(xSqr, ySqr), zSqr));
            };

            auto normalize = [vlen](__m256* xv, __m256* yv, __m256* zv)
            {
                auto len = vlen(*xv, *yv, *zv);
                *xv = _mm256_div_ps(*xv, len);
                *yv = _mm256_div_ps(*yv, len);
                *zv = _mm256_div_ps(*zv, len);
            };

            // Normalize camera ray directions
            normalize(&xvec, &yvec, &zvec);

            // Some lambdas to keep things readable
            auto sphereSDF = [](__m256* xv, __m256* yv, __m256* zv, float radius, float posx, float posy, float posz, __m256* outDistX, __m256* outDistY, __m256* outDistZ)
            {
                // Position offset
                // We're using manhattan (per-axis) distance instead of euclidean, so this is also our initial distance function
                auto rayPosDiffX = _mm256_sub_ps(*xv, _mm256_set1_ps(posx));
                auto rayPosDiffY = _mm256_sub_ps(*yv, _mm256_set1_ps(posy));
                auto rayPosDiffZ = _mm256_sub_ps(*zv, _mm256_set1_ps(posz));

                // Offset again to account for sphere radius
                // That's it! xv/rv/zv are out values
                // Unconventional but I think this avoids the messy transformation problem of going separate x/y/z -> scalar distance per-lane -> separate x/y/z
                // (maybe that's not as complex as I thought, but whatever, this is easier to think about and also less maths)
                const auto rv = _mm256_set1_ps(radius);
                *outDistX = _mm256_sub_ps(rayPosDiffX, rv);
                *outDistY = _mm256_sub_ps(rayPosDiffY, rv);
                *outDistZ = _mm256_sub_ps(rayPosDiffZ, rv);
            };

            while (memcmp(&laneState, &laneLiveState, sizeof(__m256)) != 0)
            {
                // SDF distance test
                __m256 distX, distY, distZ;
                sphereSDF(&camPosX, &camPosY, &camPosZ, 4.0f, 0.0f, 0.0f, -10.0f, &distX, &distY, &distZ);
                
                // For each lane; compare manhattan distance to eps (per-axis) and overwrite existing 
                laneState = _mm256_and_epi32(laneState, _mm256_castps_si256(_mm256_cmp_ps(distX, eps, _CMP_GT_OQ)));
                laneState = _mm256_and_epi32(laneState, _mm256_castps_si256(_mm256_cmp_ps(distY, eps, _CMP_GT_OQ)));
                laneState = _mm256_and_epi32(laneState, _mm256_castps_si256(_mm256_cmp_ps(distZ, eps, _CMP_GT_OQ)));

                // Zero-out distance changes for inactive lanes
                const auto laneMask = _mm256_castsi256_ps(_mm256_div_epi32(laneState, laneLiveState));
                distX = _mm256_mul_ps(distX, laneMask);
                distY = _mm256_mul_ps(distY, laneMask);
                distZ = _mm256_mul_ps(distZ, laneMask);

                // Shift ray forward
                camPosX = _mm256_add_ps(camPosX, distX);
                camPosY = _mm256_add_ps(camPosY, distY);
                camPosZ = _mm256_add_ps(camPosZ, distZ);

                // Accumulate trace distance
                traceDistX = _mm256_add_ps(traceDistX, distX);
                traceDistY = _mm256_add_ps(traceDistY, distX);
                traceDistZ = _mm256_add_ps(traceDistZ, distX);

                // For each lane; compare manhattan distance to eps (per-axis) and merge with existing 
                auto skyHit = _mm256_castps_si256(_mm256_cmp_ps(traceDistX, maxDist, _CMP_GT_OQ));
                skyHit = _mm256_and_epi32(skyHit, _mm256_castps_si256(_mm256_cmp_ps(traceDistY, maxDist, _CMP_GT_OQ)));
                skyHit = _mm256_and_epi32(skyHit, _mm256_castps_si256(_mm256_cmp_ps(traceDistZ, maxDist, _CMP_GT_OQ)));
                laneState = _mm256_or_epi32(skyHit, laneState);
            }

            // Shading
            // Hex colors are in ARGB order
            ///////////////////////////////

            // Bithackery on floats...hm :p
            // Need to do some type juggling here I think

            // Sky
            auto skyMask = _mm256_cmp_ps(traceDistX, maxDist, _CMP_EQ_OQ);
            skyMask = _mm256_and_ps(_mm256_cmp_ps(traceDistY, maxDist, _CMP_EQ_OQ), skyMask);
            skyMask = _mm256_and_ps(_mm256_cmp_ps(traceDistZ, maxDist, _CMP_EQ_OQ), skyMask);
            skyMask = _mm256_div_ps(skyMask, _mm256_set1_ps(INT_MAX));
            
            auto skyMaskInteger = _mm256_castps_si256(skyMask);
            auto skyRGB = _mm256_mul_epi32(skyMaskInteger, _mm256_set1_epi32(0x000000ff)); // Blue
            
            // Surface
            auto surfRGB = _mm256_set1_epi32(0x00ffa500); // Orange (yes I googled it)

            // Final color, naive non-blending color mix
            auto rgb = _mm256_xor_epi32(skyRGB, surfRGB);
            rgb = _mm256_or_epi32(rgb, _mm256_set1_epi32(0xff000000)); // OR in alpha here
            memcpy(colors_out, &rgb, sizeof(__m256));
        });

        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            InvalidateRect(msg.hwnd, NULL, false);
        }
    }
    simple_tiling::shutdown();
    return (int)msg.wParam;
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

    wcex.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_TILINGDEMORAYMARCHING));
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName = MAKEINTRESOURCEW(IDC_TILINGDEMORAYMARCHING);
    wcex.lpszClassName = szWindowClass;
    wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

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
