// Dear ImGui: standalone example application for Windows API + DirectX 11

// Learn about Dear ImGui:
// - FAQ                  https://dearimgui.com/faq
// - Getting Started      https://dearimgui.com/getting-started
// - Documentation        https://dearimgui.com/docs (same as your local docs/ folder).
// - Introduction, links and more at the top of imgui.cpp

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#include <tchar.h>
#include <iostream>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <filesystem>
#include <chrono>
#include <string>
#include <algorithm>

namespace fs = std::filesystem;

// Data
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static bool                     g_SwapChainOccluded = false;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

// Forward declarations of helper functions
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// --- GUIと裏側で通信するための「共有変数」 ---
std::atomic<bool> ui_is_scanning{false};
std::atomic<uintmax_t> ui_total_size{0};
std::atomic<uint64_t> ui_file_count{0};
std::atomic<long long> ui_scan_duration_ms{0};

struct TopFolder {
    std::string name;
    uintmax_t size;
};
std::vector<TopFolder> ui_top_folders;
std::mutex ui_results_mtx; // フォルダごとの結果を安全に更新・読み取るためのロック

// --- スキャンエンジン（バックグラウンド用） ---
void RunScanBackground(std::string target_dir) {
    ui_is_scanning = true;
    ui_total_size = 0;
    ui_file_count = 0;
    ui_scan_duration_ms = 0;
    
    // スキャン開始時にリストを空にする
    {
        std::lock_guard<std::mutex> lock(ui_results_mtx);
        ui_top_folders.clear();
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    // タスクに「どのトップフォルダに属しているか(root_idx)」を持たせる
    struct ScanTask {
        fs::path path;
        int root_idx;
    };
    std::vector<ScanTask> dirs_to_process;

    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<int> active_tasks{0};
    bool done = false;

    // 最初に直下のフォルダだけをリストアップし、名札を付けてキューに入れる
    try {
        int idx = 0;
        for (const auto& entry : fs::directory_iterator(target_dir)) {
            TopFolder tf;
            tf.name = entry.path().filename().string();
            tf.size = 0;
            
            if (entry.is_regular_file()) {
                tf.size = entry.file_size();
                ui_total_size += tf.size;
                ui_file_count++;
            } else if (entry.is_directory() && !entry.is_symlink()) {
                dirs_to_process.push_back({entry.path(), idx});
            }
            
            ui_top_folders.push_back(tf);
            idx++;
        }
    } catch (...) {}

    auto worker = [&]() {
        while (true) {
            ScanTask task;
            {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [&]() { return !dirs_to_process.empty() || done; });
                if (done && dirs_to_process.empty()) return;

                task = dirs_to_process.back();
                dirs_to_process.pop_back();
                active_tasks++;
            }

            std::error_code ec;
            uintmax_t local_size = 0;
            uint64_t local_count = 0;
            std::vector<ScanTask> new_tasks;

            for (auto it = fs::directory_iterator(task.path, ec); it != fs::directory_iterator(); it.increment(ec)) {
                if (ec) continue;
                if (it->is_directory(ec)) {
                    // 見つけた新しいフォルダにも、親と同じ名札を付けて引き継ぐ
                    if (!it->is_symlink(ec)) new_tasks.push_back({it->path(), task.root_idx});
                } else if (it->is_regular_file(ec)) {
                    local_size += it->file_size(ec);
                    local_count++;
                }
            }

            ui_total_size += local_size;
            ui_file_count += local_count;

            // 終わったら、自分の名札のフォルダ専用カウンターに容量を足す
            {
                std::lock_guard<std::mutex> res_lock(ui_results_mtx);
                ui_top_folders[task.root_idx].size += local_size;
            }

            {
                std::lock_guard<std::mutex> lock(mtx);
                for (auto& t : new_tasks) dirs_to_process.push_back(std::move(t));
                active_tasks--;
                if (dirs_to_process.empty() && active_tasks == 0) done = true;
            }
            cv.notify_all();
        }
    };

    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    
    std::vector<std::thread> threads;
    for (unsigned int i = 0; i < num_threads; ++i) threads.emplace_back(worker);
    for (auto& t : threads) t.join();

    auto end_time = std::chrono::high_resolution_clock::now();
    ui_scan_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    ui_is_scanning = false;
}

// Main code
int main(int, char**)
{
    // Make process DPI aware and obtain main monitor scale
    ImGui_ImplWin32_EnableDpiAwareness();
    float main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

    // Create application window
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"ImGui Example", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"Dear ImGui DirectX11 Example", WS_OVERLAPPEDWINDOW, 100, 100, (int)(1280 * main_scale), (int)(800 * main_scale), nullptr, nullptr, wc.hInstance, nullptr);

    // Initialize Direct3D
    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    // Show the window
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

    // --- ここから日本語フォント設定を追加 ---
    ImFontConfig config;
    config.MergeMode = false;
    // Windows標準の「メイリオ」をサイズ18で読み込み、日本語の文字コード範囲を指定する
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\meiryo.ttc", 18.0f, &config, io.Fonts->GetGlyphRangesJapanese());
    // --- ここまで ---

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    //ImGui::StyleColorsLight();

    // Setup scaling
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);        // Bake a fixed style scale. (until we have a solution for dynamic style scaling, changing this requires resetting Style + calling this again)
    style.FontScaleDpi = main_scale;        // Set initial font scale. (in docking branch: using io.ConfigDpiScaleFonts=true automatically overrides this for every window depending on the current monitor)

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Load Fonts
    // - If fonts are not explicitly loaded, Dear ImGui will select an embedded font: either AddFontDefaultVector() or AddFontDefaultBitmap().
    //   This selection is based on (style.FontSizeBase * style.FontScaleMain * style.FontScaleDpi) reaching a small threshold.
    // - You can load multiple fonts and use ImGui::PushFont()/PopFont() to select them.
    // - If a file cannot be loaded, AddFont functions will return a nullptr. Please handle those errors in your code (e.g. use an assertion, display an error and quit).
    // - Read 'docs/FONTS.md' for more instructions and details.
    // - Use '#define IMGUI_ENABLE_FREETYPE' in your imconfig file to use FreeType for higher quality font rendering.
    // - Remember that in C/C++ if you want to include a backslash \ in a string literal you need to write a double backslash \\ !
    //style.FontSizeBase = 20.0f;
    //io.Fonts->AddFontDefaultVector();
    //io.Fonts->AddFontDefaultBitmap();
    //io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf");
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf");
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf");
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf");
    //ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\ArialUni.ttf");
    //IM_ASSERT(font != nullptr);

    // Our state
    bool show_demo_window = true;
    bool show_another_window = false;
    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

    // Main loop
    bool done = false;
    while (!done)
    {
        // Poll and handle messages (inputs, window resize, etc.)
        // See the WndProc() function below for our to dispatch events to the Win32 backend.
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        // Handle window being minimized or screen locked
        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        // Handle window resize (we don't resize directly in the WM_SIZE handler)
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        // Start the Dear ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // 1. Show the big demo window (Most of the sample code is in ImGui::ShowDemoWindow()! You can browse its code to learn more about Dear ImGui!).
        // --- ここから自作UI ---
        ImGui::Begin(u8"Storage Capacity Visualization");

        ImGui::Text(u8"超高速ストレージ可視化ツール");
        ImGui::Spacing();

        static char target_path[256] = "C:\\Windows";
        
        // 修正ポイント：フレームの最初で状態を「1回だけ」読み取って変数に保存する
        bool current_is_scanning = ui_is_scanning.load();

        // 保存した変数を使って判定する
        if (current_is_scanning) {
            ImGui::BeginDisabled();
        }

        ImGui::InputText(u8"スキャン対象", target_path, sizeof(target_path));

        if (ImGui::Button(u8"スキャン開始", ImVec2(120, 30))) {
            std::string path_str = target_path;
            // 新しいスレッドを立ち上げて、エンジンを裏側で走らせる
            std::thread(RunScanBackground, path_str).detach();
        }

        // ここも「保存した変数」を使うことで、BeginとEndのペアが絶対に崩れない
        if (current_is_scanning) {
            ImGui::EndDisabled(); // 無効化ここまで
            
            // スキャン中：リアルタイムに増えていく数値を表示
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), u8"[実行中]爆速スキャン実行中...");
            ImGui::Text(u8"見つけたファイル数: %llu 個", ui_file_count.load());
        } 
        else if (ui_file_count.load() > 0) {
            // スキャン完了：最終結果を表示
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), u8"[完了]スキャン完了！");
            
            double mb = static_cast<double>(ui_total_size.load()) / (1024.0 * 1024.0);
            double gb = mb / 1024.0;
            
            ImGui::Text(u8"合計ファイル数: %llu 個", ui_file_count.load());
            ImGui::Text(u8"合計サイズ: %.2f MB (%.2f GB)", mb, gb);
            ImGui::Text(u8"処理時間: %lld ミリ秒", ui_scan_duration_ms.load());


            // 画面に図形を描くための「ペン」を取得

            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos(); 
            
            float box_width = 700.0f; // 見やすいように少し横幅を広げます
            float box_height = 100.0f;
            float current_x = p.x;
            uintmax_t total = ui_total_size.load();
            
            if (total > 0) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Text(u8"【ストレージ容量マップ（2D版）】");

            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos(); 
            
            // 2Dなので高さをしっかり取ります（700x400のキャンバス）
            float box_width = 700.0f; 
            float box_height = 400.0f;
            uintmax_t total = ui_total_size.load();
            
            if (total > 0) {
                // 1. スキャン結果をコピーして「容量が大きい順」に並び替える（2Dマップの鉄則）
                std::vector<TopFolder> sorted_folders;
                {
                    std::lock_guard<std::mutex> res_lock(ui_results_mtx);
                    sorted_folders = ui_top_folders;
                }
                std::sort(sorted_folders.begin(), sorted_folders.end(), [](const TopFolder& a, const TopFolder& b) {
                    return a.size > b.size; // 大きい順（降順）
                });

                // 2. 描画用に「残っているキャンバスの領域」と「残っている容量」を管理する
                float current_x = p.x;
                float current_y = p.y;
                float remain_w = box_width;
                float remain_h = box_height;
                uintmax_t remain_total = total;
                int color_idx = 0;

                for (const auto& folder : sorted_folders) {
                    if (folder.size == 0 || remain_total == 0) continue; 
                    
                    // 残りの容量に対する、このフォルダの割合
                    float ratio = static_cast<float>(folder.size) / remain_total;
                    
                    float rect_w, rect_h;
                    // 2Dマップのアルゴリズム：残りの領域が「横長」なら縦に割る、「縦長」なら横に割る
                    if (remain_w > remain_h) {
                        rect_w = remain_w * ratio;
                        rect_h = remain_h;
                    } else {
                        rect_w = remain_w;
                        rect_h = remain_h * ratio;
                    }
                    
                    // 面積が小さすぎる（1ピクセル未満）場合は描画をスキップ
                    if (rect_w < 1.0f || rect_h < 1.0f) {
                        remain_total -= folder.size;
                        continue;
                    }

                    // 色の生成と座標の決定
                    ImU32 color = IM_COL32(50 + (color_idx * 40) % 150, 100 + (color_idx * 70) % 155, 200 - (color_idx * 20) % 100, 255);
                    ImVec2 rect_min = ImVec2(current_x, current_y);
                    ImVec2 rect_max = ImVec2(current_x + rect_w, current_y + rect_h);

                    // 四角形と枠線の描画
                    draw_list->AddRectFilled(rect_min, rect_max, color);
                    draw_list->AddRect(rect_min, rect_max, IM_COL32(255, 255, 255, 150));
                    
                    // ツールチップ機能（文字化け修正済み）
                    if (ImGui::IsMouseHoveringRect(rect_min, rect_max)) {
                        ImGui::BeginTooltip();
                        ImGui::Text(u8"[ %s ]", folder.name.c_str());
                        
                        double folder_mb = static_cast<double>(folder.size) / (1024.0 * 1024.0);
                        double folder_gb = folder_mb / 1024.0;
                        if (folder_gb >= 1.0) {
                            ImGui::Text(u8"サイズ: %.2f GB", folder_gb);
                        } else {
                            ImGui::Text(u8"サイズ: %.2f MB", folder_mb);
                        }
                        
                        // 全体に対する割合を計算
                        ImGui::Text(u8"割合: %.1f %%", (static_cast<float>(folder.size) / total) * 100.0f);
                        ImGui::EndTooltip();
                    }

                    // 次の図形のために、キャンバスの残り領域を更新する
                    if (remain_w > remain_h) {
                        current_x += rect_w;  // 開始位置を右へ
                        remain_w -= rect_w;   // 残りの幅を減らす
                    } else {
                        current_y += rect_h;  // 開始位置を下へ
                        remain_h -= rect_h;   // 残りの高さを減らす
                    }
                    remain_total -= folder.size;
                    color_idx++;
                }
            }

            ImGui::Dummy(ImVec2(box_width, box_height));
            }
        }

        ImGui::End();

        // 3. Show another simple window.
        if (show_another_window)
        {
            ImGui::Begin("Another Window", &show_another_window);   // Pass a pointer to our bool variable (the window will have a closing button that will clear the bool when clicked)
            ImGui::Text("Hello from another window!");
            if (ImGui::Button("Close Me"))
                show_another_window = false;
            ImGui::End();
        }

        // Rendering
        ImGui::Render();
        const float clear_color_with_alpha[4] = { clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Present
        HRESULT hr = g_pSwapChain->Present(1, 0);   // Present with vsync
        //HRESULT hr = g_pSwapChain->Present(0, 0); // Present without vsync
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    // Cleanup
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}

// Helper functions

bool CreateDeviceD3D(HWND hWnd)
{
    // Setup swap chain
    // This is a basic setup. Optimally could use e.g. DXGI_SWAP_EFFECT_FLIP_DISCARD and handle fullscreen mode differently. See #8979 for suggestions.
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    //createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED) // Try high-performance WARP software driver if hardware is not available.
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Win32 message handler
// You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
// - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
// - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
// Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam); // Queue resize
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
