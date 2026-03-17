#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "gdi32.lib")

using Microsoft::WRL::ComPtr;

// Must match Renderer.h exactly
struct ExportControlBlock
{
    uint32_t SelectedWidgetIndex;
    uint32_t WidgetCount;

    struct WidgetInfo
    {
        HANDLE SharedTextureHandle;
        uint32_t Width;
        uint32_t Height;
        uint8_t Type;
        bool Active;
        uint8_t Padding[2];
    } Widgets[10];

    bool ExportEnabled;
};

const char *GetWidgetTypeName(uint8_t type)
{
    const char *names[] = {"Torso Crosshair", "Arms Crosshair", "Arms Target Crosshair", "Head Crosshair",
                           "Target Screen",   "Weapons Screen", "Owner Screen",          "Objectives Screen",
                           "Map Screen",      "Comms Screen"};
    return type < 10 ? names[type] : "Unknown";
}

class HUDDisplay
{
  private:
    HWND hwnd = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapChain;
    ComPtr<ID3D11Texture2D> sharedTextures[10];
    ComPtr<ID3D11ShaderResourceView> sharedSRVs[10];
    ComPtr<ID3D11Texture2D> cachedTextures[10]; // Local copies to prevent flicker
    ComPtr<ID3D11ShaderResourceView> cachedSRVs[10];
    ComPtr<ID3D11RenderTargetView> backBufferRTV;
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> cropBuffer; // Constant buffer for crop region

    ExportControlBlock controlBlock;
    HANDLE mmfHandle = nullptr;
    void *mmfBuffer = nullptr;
    bool running = true;
    uint32_t currentWidget = 4;       // Start with Target Screen
    HANDLE lastKnownHandles[10] = {}; // Track handles to detect changes

    // Store texture dimensions for consistent scaling
    uint32_t referenceTexWidth[10] = {};
    uint32_t referenceTexHeight[10] = {};

    // Hotkey modifier (0=none, MOD_CONTROL, MOD_ALT, MOD_SHIFT)
    UINT hotkeyModifier = 0;
    std::string modifierName = "None";

    // Window position/size config
    struct WindowConfig
    {
        int x = 100;
        int y = 100;
        int width = 777;
        int height = 809;
    } windowConfig;

    // Screen capture variables
    ComPtr<ID3D11Texture2D> screenCaptureTexture;
    ComPtr<ID3D11ShaderResourceView> screenCaptureSRV;
    bool isScreenCaptureMode = false;
    int screenCaptureX = 798;
    int screenCaptureY = 430;
    int screenCaptureWidth = 325;
    int screenCaptureHeight = 220;
    HDC hdcScreen = nullptr;
    HDC hdcMem = nullptr;
    HBITMAP hBitmap = nullptr;
    std::vector<uint8_t> pixelBuffer;

    // Helper function to find widget index by type
    int FindWidgetByType(uint8_t targetType)
    {
        if (!mmfBuffer)
            return -1;

        ExportControlBlock *control = static_cast<ExportControlBlock *>(mmfBuffer);

        for (int i = 0; i < 10; i++)
        {
            if (control->Widgets[i].Active && control->Widgets[i].Type == targetType)
            {
                return i;
            }
        }

        return -1; // Not found
    }

    // Crop region (UV coordinates 0-1)
    float cropLeft = 0.0f;
    float cropRight = 1.0f;
    float cropTop = 0.0f;
    float cropBottom = 1.0f;

    void Log(const std::string &msg)
    {
        std::cout << msg << std::endl;
    }

    std::string GetConfigPath()
    {
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        std::string exePath(path);
        size_t lastSlash = exePath.find_last_of("\\/");
        return exePath.substr(0, lastSlash + 1) + "HUDDisplay_" + modifierName + ".ini";
    }

    void LoadWindowConfig()
    {
        std::string configPath = GetConfigPath();
        std::ifstream file(configPath);
        if (!file.is_open())
        {
            Log("No config file found, using defaults");
            return;
        }

        std::string line;
        while (std::getline(file, line))
        {
            if (line.empty() || line[0] == '#')
                continue;

            size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;

            std::string key = line.substr(0, eq);
            std::string value = line.substr(eq + 1);

            if (key == "x")
                windowConfig.x = std::stoi(value);
            else if (key == "y")
                windowConfig.y = std::stoi(value);
            else if (key == "width")
                windowConfig.width = std::stoi(value);
            else if (key == "height")
                windowConfig.height = std::stoi(value);
        }

        Log("Loaded window config: " + std::to_string(windowConfig.x) + "," + std::to_string(windowConfig.y) + " " +
            std::to_string(windowConfig.width) + "x" + std::to_string(windowConfig.height));
    }

    void SaveWindowConfig()
    {
        if (!hwnd || !IsWindow(hwnd))
            return;

        RECT rect;
        if (!GetWindowRect(hwnd, &rect))
        {
            Log("Failed to get window rect");
            return;
        }

        // Validate values are reasonable (not garbage)
        if (rect.left < -10000 || rect.left > 10000 || rect.top < -10000 || rect.top > 10000 ||
            (rect.right - rect.left) < 100 || (rect.right - rect.left) > 5000 || (rect.bottom - rect.top) < 100 ||
            (rect.bottom - rect.top) > 5000)
        {
            Log("Window rect has invalid values, not saving");
            return;
        }

        windowConfig.x = rect.left;
        windowConfig.y = rect.top;
        windowConfig.width = rect.right - rect.left;
        windowConfig.height = rect.bottom - rect.top;

        std::string configPath = GetConfigPath();
        std::ofstream file(configPath);
        if (!file.is_open())
        {
            Log("Failed to save config");
            return;
        }

        file << "# HUDDisplay Window Configuration\n";
        file << "x=" << windowConfig.x << "\n";
        file << "y=" << windowConfig.y << "\n";
        file << "width=" << windowConfig.width << "\n";
        file << "height=" << windowConfig.height << "\n";

        Log("Saved window config to " + configPath);
    }

    bool InitWindow()
    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEX);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"HUDDisplayClass";

        if (!RegisterClassExW(&wc))
        {
            Log("Failed to register window class");
            return false;
        }

        // Create window title with modifier name
        std::wstring title = L"MW5 MFD Display";
        if (hotkeyModifier != 0)
        {
            std::wstring wModifier(modifierName.begin(), modifierName.end());
            title += L" (" + wModifier + L")";
        }

        // Use saved position/size from config
        hwnd = CreateWindowExW(WS_EX_TOPMOST, L"HUDDisplayClass", title.c_str(), WS_OVERLAPPEDWINDOW, windowConfig.x,
                               windowConfig.y, windowConfig.width, windowConfig.height, nullptr, nullptr,
                               GetModuleHandle(nullptr), this);

        if (!hwnd)
        {
            Log("Failed to create window");
            return false;
        }

        // Register global hotkeys (work even when window not focused)
        // MOD_NOREPEAT prevents key repeat
        RegisterHotKey(hwnd, 1, MOD_NOREPEAT | hotkeyModifier, VK_NUMPAD1); // Target Screen
        RegisterHotKey(hwnd, 2, MOD_NOREPEAT | hotkeyModifier, VK_NUMPAD2); // Weapons Screen
        RegisterHotKey(hwnd, 3, MOD_NOREPEAT | hotkeyModifier, VK_NUMPAD3); // Owner Screen
        RegisterHotKey(hwnd, 4, MOD_NOREPEAT | hotkeyModifier, VK_NUMPAD4); // Objectives Screen
        RegisterHotKey(hwnd, 5, MOD_NOREPEAT | hotkeyModifier, VK_NUMPAD5); // Map Screen
        RegisterHotKey(hwnd, 6, MOD_NOREPEAT | hotkeyModifier, VK_NUMPAD6); // Comms Screen
        RegisterHotKey(hwnd, 7, MOD_NOREPEAT | hotkeyModifier, VK_NUMPAD7); // Cycle forward
        RegisterHotKey(hwnd, 8, MOD_NOREPEAT | hotkeyModifier, VK_NUMPAD8); // Cycle backward
        RegisterHotKey(hwnd, 9, MOD_NOREPEAT | hotkeyModifier, VK_NUMPAD9); // Screen Capture

        Log("Registered global hotkeys (" + modifierName + "+Numpad 1-9)");

        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);

        Log("Window created successfully");
        return true;
    }

    bool InitD3D()
    {
        DXGI_SWAP_CHAIN_DESC scd = {};
        scd.BufferCount = 1;
        scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.OutputWindow = hwnd;
        scd.SampleDesc.Count = 1;
        scd.Windowed = TRUE;
        scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        D3D_FEATURE_LEVEL featureLevel;
        HRESULT hr =
            D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                          &scd, &swapChain, &device, &featureLevel, &context);

        if (FAILED(hr))
        {
            Log("Failed to create D3D11 device");
            return false;
        }

        ComPtr<ID3D11Texture2D> backBuffer;
        swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        device->CreateRenderTargetView(backBuffer.Get(), nullptr, &backBufferRTV);

        Log("D3D11 initialized");
        return true;
    }

    bool OpenMMF()
    {
        mmfHandle = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, TEXT("MechWarriorVR_HUDExport"));
        if (!mmfHandle)
        {
            Log("Failed to open memory-mapped file. Is the game running?");
            return false;
        }

        mmfBuffer = MapViewOfFile(mmfHandle, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (!mmfBuffer)
        {
            Log("Failed to map view of file");
            CloseHandle(mmfHandle);
            mmfHandle = nullptr;
            return false;
        }

        memcpy(&controlBlock, mmfBuffer, sizeof(ExportControlBlock));

        Log("MMF opened. WidgetCount=" + std::to_string(controlBlock.WidgetCount));
        return true;
    }

    bool OpenSharedTexture(int widgetIndex)
    {
        if (widgetIndex >= 10)
            return false;

        // Read latest control block
        if (mmfBuffer)
        {
            memcpy(&controlBlock, mmfBuffer, sizeof(ExportControlBlock));
        }

        auto &info = controlBlock.Widgets[widgetIndex];
        if (!info.Active)
        {
            Log("Widget " + std::to_string(widgetIndex) + " not active");
            return false;
        }

        if (!info.SharedTextureHandle)
        {
            Log("Widget " + std::to_string(widgetIndex) + " has no texture handle yet");
            return false;
        }

        // Check if handle changed (mission restart, etc.) - if so, close old texture
        if (lastKnownHandles[widgetIndex] != info.SharedTextureHandle && lastKnownHandles[widgetIndex] != nullptr)
        {
            Log("Widget " + std::to_string(widgetIndex) + " handle changed - reopening");
            sharedSRVs[widgetIndex].Reset();
            sharedTextures[widgetIndex].Reset();
        }

        // If we already have this exact texture open, we're done
        if (sharedTextures[widgetIndex] && lastKnownHandles[widgetIndex] == info.SharedTextureHandle)
        {
            return true;
        }

        // Close old texture if we're reopening
        if (sharedTextures[widgetIndex])
        {
            sharedSRVs[widgetIndex].Reset();
            sharedTextures[widgetIndex].Reset();
            // Also reset cached texture so it gets recreated with new dimensions
            cachedSRVs[widgetIndex].Reset();
            cachedTextures[widgetIndex].Reset();
        }

        ComPtr<ID3D11Texture2D> texture;
        HRESULT hr = device->OpenSharedResource(info.SharedTextureHandle, __uuidof(ID3D11Texture2D), (void **)&texture);

        if (FAILED(hr))
        {
            Log("Failed to open shared texture " + std::to_string(widgetIndex) + " (" + GetWidgetTypeName(info.Type) +
                "): HRESULT=0x" + std::to_string(hr));
            lastKnownHandles[widgetIndex] = nullptr;
            return false;
        }

        sharedTextures[widgetIndex] = texture;
        hr = device->CreateShaderResourceView(texture.Get(), nullptr, &sharedSRVs[widgetIndex]);

        if (FAILED(hr))
        {
            Log("Failed to create SRV for widget " + std::to_string(widgetIndex));
            sharedTextures[widgetIndex].Reset();
            lastKnownHandles[widgetIndex] = nullptr;
            return false;
        }

        lastKnownHandles[widgetIndex] = info.SharedTextureHandle;

        // Always update reference dimensions (in case texture size changed, e.g. different mech)
        referenceTexWidth[widgetIndex] = info.Width;
        referenceTexHeight[widgetIndex] = info.Height;
        Log("Updated reference dimensions for widget " + std::to_string(widgetIndex) + ": " +
            std::to_string(info.Width) + "x" + std::to_string(info.Height));

        Log("Opened texture " + std::to_string(widgetIndex) + ": " + GetWidgetTypeName(info.Type) + " (" +
            std::to_string(info.Width) + "x" + std::to_string(info.Height) + ")");
        return true;
    }

    bool InitScreenCapture()
    {
        // Get screen DC
        hdcScreen = GetDC(NULL);
        if (!hdcScreen)
        {
            Log("Failed to get screen DC");
            return false;
        }

        // Create memory DC
        hdcMem = CreateCompatibleDC(hdcScreen);
        if (!hdcMem)
        {
            Log("Failed to create memory DC");
            return false;
        }

        // Create bitmap
        hBitmap = CreateCompatibleBitmap(hdcScreen, screenCaptureWidth, screenCaptureHeight);
        if (!hBitmap)
        {
            Log("Failed to create bitmap");
            return false;
        }

        SelectObject(hdcMem, hBitmap);

        // Allocate pixel buffer
        pixelBuffer.resize(screenCaptureWidth * screenCaptureHeight * 4);

        // Create D3D11 texture
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = screenCaptureWidth;
        desc.Height = screenCaptureHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        if (FAILED(device->CreateTexture2D(&desc, nullptr, &screenCaptureTexture)))
        {
            Log("Failed to create screen capture texture");
            return false;
        }

        device->CreateShaderResourceView(screenCaptureTexture.Get(), nullptr, &screenCaptureSRV);

        Log("Screen Capture initialized (BitBlt method)");
        return true;
    }

    bool CaptureScreenRegion()
    {
        if (!hdcScreen || !hdcMem || !screenCaptureTexture)
            return false;

        // Capture screen region to memory DC
        if (!BitBlt(hdcMem, 0, 0, screenCaptureWidth, screenCaptureHeight, hdcScreen, screenCaptureX, screenCaptureY,
                    SRCCOPY))
        {
            return false;
        }

        // Get bitmap bits
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = screenCaptureWidth;
        bmi.bmiHeader.biHeight = -screenCaptureHeight; // Negative for top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        if (GetDIBits(hdcMem, hBitmap, 0, screenCaptureHeight, pixelBuffer.data(), &bmi, DIB_RGB_COLORS) == 0)
        {
            return false;
        }

        // Update D3D11 texture
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(context->Map(screenCaptureTexture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            // If row pitch matches, do single memcpy (fastest)
            if (mapped.RowPitch == screenCaptureWidth * 4)
            {
                memcpy(mapped.pData, pixelBuffer.data(), pixelBuffer.size());
            }
            else
            {
                // Otherwise copy row by row
                uint8_t *dst = (uint8_t *)mapped.pData;
                uint8_t *src = pixelBuffer.data();

                for (int y = 0; y < screenCaptureHeight; y++)
                {
                    memcpy(dst, src, screenCaptureWidth * 4);
                    dst += mapped.RowPitch;
                    src += screenCaptureWidth * 4;
                }
            }

            context->Unmap(screenCaptureTexture.Get(), 0);
            return true;
        }

        return false;
    }

    bool InitShaders()
    {
        const char *vsCode = R"(
            struct VSOut {
                float4 pos : SV_POSITION;
                float2 uv : TEXCOORD0;
            };
            
            VSOut main(uint id : SV_VertexID) {
                VSOut output;
                output.uv = float2((id << 1) & 2, id & 2);
                output.pos = float4(output.uv * float2(2, -2) + float2(-1, 1), 0, 1);
                return output;
            }
        )";

        const char *psCode = R"(
            Texture2D tex : register(t0);
            SamplerState samp : register(s0);
            
            float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
                return tex.Sample(samp, uv);
            }
        )";

        ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;

        D3DCompile(vsCode, strlen(vsCode), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &errorBlob);
        D3DCompile(psCode, strlen(psCode), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &errorBlob);

        device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vertexShader);
        device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &pixelShader);

        Log("Shaders compiled: Fullscreen stretch-to-fill active");

        // Create constant buffer for aspect ratio transform
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = 16; // 2 float2s (scale, offset)
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        device->CreateBuffer(&cbDesc, nullptr, &cropBuffer);

        D3D11_SAMPLER_DESC sampDesc = {};
        sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        device->CreateSamplerState(&sampDesc, &sampler);

        Log("Shaders created");
        return true;
    }

    void SelectWidget(uint8_t widgetType)
    {
        // Refresh control block
        if (mmfBuffer)
        {
            memcpy(&controlBlock, mmfBuffer, sizeof(ExportControlBlock));
        }

        // Find the widget with this type
        int widgetIndex = FindWidgetByType(widgetType);

        if (widgetIndex < 0)
        {
            Log("Widget type " + std::string(GetWidgetTypeName(widgetType)) + " not found!");
            return;
        }

        auto &info = controlBlock.Widgets[widgetIndex];
        if (!info.Active || !info.SharedTextureHandle)
        {
            Log("Widget " + std::to_string(widgetIndex) + " (" + GetWidgetTypeName(info.Type) + ") not available yet");
            return;
        }

        currentWidget = widgetIndex;

        // Update selection in MMF
        if (mmfBuffer)
        {
            ExportControlBlock *control = static_cast<ExportControlBlock *>(mmfBuffer);
            control->SelectedWidgetIndex = widgetIndex;
        }

        Log("Selected: " + std::string(GetWidgetTypeName(info.Type)) + " (index " + std::to_string(widgetIndex) + ", " +
            std::to_string(info.Width) + "x" + std::to_string(info.Height) + ")");

        // Open texture if not already open
        if (!sharedTextures[widgetIndex] || !sharedSRVs[widgetIndex])
        {
            OpenSharedTexture(widgetIndex);
        }
    }

    void CycleWidget(int direction)
    {
        // Refresh control block
        if (mmfBuffer)
        {
            memcpy(&controlBlock, mmfBuffer, sizeof(ExportControlBlock));
        }

        // Find next active MFD screen (widgets 4-9)
        int attempts = 0;
        int nextWidget = currentWidget;

        while (attempts < 6) // Max 6 MFD screens
        {
            nextWidget += direction;

            // Wrap around within MFD range (4-9)
            if (nextWidget < 4)
                nextWidget = 9;
            else if (nextWidget > 9)
                nextWidget = 4;

            // Check if this widget is active
            auto &info = controlBlock.Widgets[nextWidget];
            if (info.Active && info.SharedTextureHandle)
            {
                currentWidget = nextWidget;

                // Update selection in MMF
                if (mmfBuffer)
                {
                    ExportControlBlock *control = static_cast<ExportControlBlock *>(mmfBuffer);
                    control->SelectedWidgetIndex = nextWidget;
                }

                Log("Cycled to: " + std::string(GetWidgetTypeName(info.Type)) + " (index " +
                    std::to_string(nextWidget) + ")");

                // Open texture if not already open
                if (!sharedTextures[nextWidget] || !sharedSRVs[nextWidget])
                {
                    OpenSharedTexture(nextWidget);
                }

                return;
            }

            attempts++;
        }

        Log("No other active MFD screens found");
    }

    void ToggleScreenCapture()
    {
        isScreenCaptureMode = !isScreenCaptureMode;

        if (isScreenCaptureMode)
        {
            Log("Switched to Screen Capture mode (capturing " + std::to_string(screenCaptureWidth) + "x" +
                std::to_string(screenCaptureHeight) + " at " + std::to_string(screenCaptureX) + "," +
                std::to_string(screenCaptureY) + ")");
        }
        else
        {
            auto &info = controlBlock.Widgets[currentWidget];
            Log("Switched back to: " + std::string(GetWidgetTypeName(info.Type)));
        }
    }

    void Render()
    {
        // Handle screen capture mode separately
        if (isScreenCaptureMode)
        {
            if (CaptureScreenRegion() && screenCaptureSRV)
            {
                float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                context->ClearRenderTargetView(backBufferRTV.Get(), clearColor);
                context->OMSetRenderTargets(1, backBufferRTV.GetAddressOf(), nullptr);

                RECT rect;
                GetClientRect(hwnd, &rect);

                D3D11_VIEWPORT vp = {};
                vp.Width = (float)(rect.right - rect.left);
                vp.Height = (float)(rect.bottom - rect.top);
                vp.MinDepth = 0.0f;
                vp.MaxDepth = 1.0f;
                context->RSSetViewports(1, &vp);

                context->VSSetShader(vertexShader.Get(), nullptr, 0);
                context->PSSetShader(pixelShader.Get(), nullptr, 0);
                context->PSSetShaderResources(0, 1, screenCaptureSRV.GetAddressOf());
                context->PSSetSamplers(0, 1, sampler.GetAddressOf());
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                context->Draw(3, 0);
            }
            else
            {
                // Clear to black if capture failed
                float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                context->ClearRenderTargetView(backBufferRTV.Get(), clearColor);
            }

            swapChain->Present(0, 0); // No vsync for faster updates
            return;
        }

        // Normal MFD widget rendering
        // Refresh control block to get latest data
        if (mmfBuffer)
        {
            memcpy(&controlBlock, mmfBuffer, sizeof(ExportControlBlock));
        }

        // Check if current widget's handle changed or texture is missing
        auto &info = controlBlock.Widgets[currentWidget];

        // If widget is not active, clear cached resources and show black screen
        if (!info.Active)
        {
            sharedTextures[currentWidget].Reset();
            sharedSRVs[currentWidget].Reset();
            cachedTextures[currentWidget].Reset();
            cachedSRVs[currentWidget].Reset();
            lastKnownHandles[currentWidget] = nullptr;

            // Clear to black
            float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            context->ClearRenderTargetView(backBufferRTV.Get(), clearColor);
            swapChain->Present(1, 0);
            return;
        }

        bool needsReopen = !sharedTextures[currentWidget] || !sharedSRVs[currentWidget] ||
                           (info.SharedTextureHandle && lastKnownHandles[currentWidget] != info.SharedTextureHandle);

        if (needsReopen)
        {
            // Try to open/reopen it
            OpenSharedTexture(currentWidget);
            if (!sharedTextures[currentWidget])
            {
                // Clear to black if no texture available
                float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                context->ClearRenderTargetView(backBufferRTV.Get(), clearColor);
                swapChain->Present(1, 0);
                return;
            }
        }

        ComPtr<IDXGIKeyedMutex> mutex;
        bool updatedCache = false;

        if (SUCCEEDED(sharedTextures[currentWidget].As(&mutex)))
        {
            // Try to acquire with longer timeout (33ms = ~2 frames at 60fps)
            HRESULT hr = mutex->AcquireSync(1, 33);

            if (SUCCEEDED(hr))
            {
                // Successfully got the mutex - copy to our local cache

                // Get shared texture descriptor
                D3D11_TEXTURE2D_DESC sharedDesc;
                sharedTextures[currentWidget]->GetDesc(&sharedDesc);

                // Check if cached texture exists and matches size
                bool needsRecreate = !cachedTextures[currentWidget];
                if (cachedTextures[currentWidget])
                {
                    D3D11_TEXTURE2D_DESC cachedDesc;
                    cachedTextures[currentWidget]->GetDesc(&cachedDesc);

                    // Recreate if size changed
                    if (cachedDesc.Width != sharedDesc.Width || cachedDesc.Height != sharedDesc.Height)
                    {
                        Log("Cached texture size mismatch for widget " + std::to_string(currentWidget) + " (" +
                            std::to_string(cachedDesc.Width) + "x" + std::to_string(cachedDesc.Height) + " -> " +
                            std::to_string(sharedDesc.Width) + "x" + std::to_string(sharedDesc.Height) +
                            "), recreating");
                        cachedTextures[currentWidget].Reset();
                        cachedSRVs[currentWidget].Reset();
                        needsRecreate = true;
                    }
                }

                if (needsRecreate)
                {
                    // Create cached texture matching shared texture size
                    D3D11_TEXTURE2D_DESC desc = sharedDesc;
                    desc.MiscFlags = 0; // Remove shared flag for local copy

                    if (SUCCEEDED(
                            device->CreateTexture2D(&desc, nullptr, cachedTextures[currentWidget].GetAddressOf())))
                    {
                        device->CreateShaderResourceView(cachedTextures[currentWidget].Get(), nullptr,
                                                         cachedSRVs[currentWidget].GetAddressOf());
                        Log("Created cached texture for widget " + std::to_string(currentWidget) + " (" +
                            std::to_string(desc.Width) + "x" + std::to_string(desc.Height) + ")");
                    }
                }

                if (cachedTextures[currentWidget])
                {
                    context->CopyResource(cachedTextures[currentWidget].Get(), sharedTextures[currentWidget].Get());
                    updatedCache = true;
                }

                mutex->ReleaseSync(0);
            }
        }

        // Render from cache (whether we just updated it or using old frame)
        if (cachedSRVs[currentWidget])
        {
            float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            context->ClearRenderTargetView(backBufferRTV.Get(), clearColor);
            context->OMSetRenderTargets(1, backBufferRTV.GetAddressOf(), nullptr);

            // Get window dimensions and set simple fullscreen viewport
            RECT rect;
            GetClientRect(hwnd, &rect);

            D3D11_VIEWPORT vp = {};
            vp.Width = (float)(rect.right - rect.left);
            vp.Height = (float)(rect.bottom - rect.top);
            vp.MinDepth = 0.0f;
            vp.MaxDepth = 1.0f;
            context->RSSetViewports(1, &vp);

            context->VSSetShader(vertexShader.Get(), nullptr, 0);
            context->PSSetShader(pixelShader.Get(), nullptr, 0);
            context->PSSetShaderResources(0, 1, cachedSRVs[currentWidget].GetAddressOf());
            context->PSSetSamplers(0, 1, sampler.GetAddressOf());
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->Draw(3, 0);
        }

        swapChain->Present(1, 0);
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        HUDDisplay *display = nullptr;

        if (msg == WM_CREATE)
        {
            CREATESTRUCT *cs = (CREATESTRUCT *)lParam;
            display = (HUDDisplay *)cs->lpCreateParams;
            SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)display);
        }
        else
        {
            display = (HUDDisplay *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        }

        if (display)
        {
            switch (msg)
            {
            case WM_CLOSE:
                // Save window position/size before closing
                display->SaveWindowConfig();
                // Let default handler destroy the window
                break;
            case WM_DESTROY:
                display->running = false;
                PostQuitMessage(0);
                return 0;
            case WM_SIZE:
                if (display->swapChain && display->device && wParam != SIZE_MINIMIZED)
                {
                    // Release old render target view
                    display->backBufferRTV.Reset();

                    // Resize swap chain buffers to new window size
                    display->swapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);

                    // Recreate render target view
                    ComPtr<ID3D11Texture2D> backBuffer;
                    display->swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&backBuffer);
                    display->device->CreateRenderTargetView(backBuffer.Get(), nullptr, &display->backBufferRTV);
                }
                return 0;
            case WM_HOTKEY:
                // Global hotkeys (work even when window not focused)
                switch (wParam)
                {
                case 1:
                    display->SelectWidget(4);
                    break; // Numpad 1 - Target Screen
                case 2:
                    display->SelectWidget(5);
                    break; // Numpad 2 - Weapons Screen
                case 3:
                    display->SelectWidget(6);
                    break; // Numpad 3 - Owner Screen
                case 4:
                    display->SelectWidget(7);
                    break; // Numpad 4 - Objectives Screen
                case 5:
                    display->SelectWidget(8);
                    break; // Numpad 5 - Map Screen
                case 6:
                    display->SelectWidget(9);
                    break; // Numpad 6 - Comms Screen
                case 7:
                    display->CycleWidget(1);
                    break; // Numpad 7 - Cycle forward
                case 8:
                    display->CycleWidget(-1);
                    break; // Numpad 8 - Cycle backward
                case 9:
                    display->ToggleScreenCapture();
                    break; // Numpad 9 - Screen Capture
                }
                return 0;
            case WM_KEYDOWN:
                switch (wParam)
                {
                case VK_F1:
                    display->SelectWidget(4);
                    break; // Target Screen
                case VK_F2:
                    display->SelectWidget(5);
                    break; // Weapons Screen
                case VK_F3:
                    display->SelectWidget(6);
                    break; // Owner Screen
                case VK_F4:
                    display->SelectWidget(7);
                    break; // Objectives Screen
                case VK_F5:
                    display->SelectWidget(8);
                    break; // Map Screen
                case VK_F6:
                    display->SelectWidget(9);
                    break; // Comms Screen
                case VK_ESCAPE:
                    PostQuitMessage(0);
                    break;
                }
                return 0;
            }
        }

        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

  public:
    void SetModifier(const std::string &modifier)
    {
        if (modifier == "ctrl")
        {
            hotkeyModifier = MOD_CONTROL;
            modifierName = "Ctrl";
        }
        else if (modifier == "alt")
        {
            hotkeyModifier = MOD_ALT;
            modifierName = "Alt";
        }
        else if (modifier == "shift")
        {
            hotkeyModifier = MOD_SHIFT;
            modifierName = "Shift";
        }
        else
        {
            hotkeyModifier = 0;
            modifierName = "None";
        }
    }

    void SetScreenCaptureRegion(int x, int y, int width, int height)
    {
        screenCaptureX = x;
        screenCaptureY = y;
        screenCaptureWidth = width;
        screenCaptureHeight = height;
    }

    bool Initialize()
    {
        AllocConsole();
        FILE *fp;
        freopen_s(&fp, "CONOUT$", "w", stdout);

        Log("=== MW5 MFD Display Starting ===");
        Log("Hotkey Modifier: " + modifierName);
        Log("Screen Capture Region: (" + std::to_string(screenCaptureX) + "," + std::to_string(screenCaptureY) + ") " +
            std::to_string(screenCaptureWidth) + "x" + std::to_string(screenCaptureHeight));
        Log("Controls (work even when window not focused):");
        if (hotkeyModifier == 0)
        {
            Log("  Numpad 1=Target, 2=Weapons, 3=Owner, 4=Objectives, 5=Map, 6=Comms");
            Log("  Numpad 7=Cycle Forward, 8=Cycle Backward, 9=Screen Capture, ESC=Exit");
        }
        else
        {
            Log("  " + modifierName + "+Numpad 1=Target, 2=Weapons, 3=Owner, 4=Objectives, 5=Map, 6=Comms");
            Log("  " + modifierName + "+Numpad 7=Cycle Forward, 8=Cycle Backward, 9=Screen Capture, ESC=Exit");
        }

        // Load saved window position/size
        LoadWindowConfig();

        if (!InitWindow())
            return false;
        if (!InitD3D())
            return false;
        if (!InitShaders())
            return false;

        // Initialize screen capture
        if (!InitScreenCapture())
        {
            Log("Screen capture unavailable - Numpad 9 will not work");
        }

        // Try to open MMF (wait for game)
        for (int i = 0; i < 10; i++)
        {
            if (OpenMMF())
            {
                // Set the selected widget index in the control block so game knows what to export
                if (mmfBuffer)
                {
                    ExportControlBlock *control = static_cast<ExportControlBlock *>(mmfBuffer);
                    control->SelectedWidgetIndex = currentWidget;
                }

                Log("MMF opened, waiting for widget to be ready...");

                // Give the game a moment to create the texture
                Sleep(100);

                // Try to open the default widget (Target Screen)
                for (int attempt = 0; attempt < 50; attempt++)
                {
                    if (OpenSharedTexture(currentWidget))
                    {
                        Log("Initial widget opened successfully");
                        return true;
                    }
                    Sleep(100);
                }

                Log("MMF opened but widget not ready yet - will retry during render");
                return true;
            }
            Log("Retrying in 1 second...");
            Sleep(1000);
        }

        Log("Failed to open MMF after retries");
        return false;
    }

    void Run()
    {
        Log("Entering render loop");

        MSG msg = {};
        while (running)
        {
            // Process all pending messages without blocking
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }

            // Render immediately without sleeping - capture as fast as possible
            Render();

            // In screen capture mode, don't wait - capture at maximum rate
            if (!isScreenCaptureMode)
            {
                // For MFD screens, small sleep to avoid hammering the CPU
                Sleep(1);
            }
        }

        Log("Exiting");

        // Cleanup screen capture resources
        if (hBitmap)
        {
            DeleteObject(hBitmap);
            hBitmap = nullptr;
        }
        if (hdcMem)
        {
            DeleteDC(hdcMem);
            hdcMem = nullptr;
        }
        if (hdcScreen)
        {
            ReleaseDC(NULL, hdcScreen);
            hdcScreen = nullptr;
        }

        // Cleanup hotkeys
        if (hwnd)
        {
            for (int i = 1; i <= 9; i++)
            {
                UnregisterHotKey(hwnd, i);
            }
        }

        // Cleanup
        if (mmfBuffer)
        {
            UnmapViewOfFile(mmfBuffer);
            mmfBuffer = nullptr;
        }
        if (mmfHandle)
        {
            CloseHandle(mmfHandle);
            mmfHandle = nullptr;
        }
    }
};

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int)
{
    HUDDisplay display;

    // Parse command line for modifier (e.g., "ctrl", "alt", "shift")
    std::string cmdLine(lpCmdLine);

    // Convert to lowercase for case-insensitive comparison
    for (char &c : cmdLine)
        c = tolower(c);

    // Set modifier based on command line
    if (cmdLine.find("ctrl") != std::string::npos)
        display.SetModifier("ctrl");
    else if (cmdLine.find("alt") != std::string::npos)
        display.SetModifier("alt");
    else if (cmdLine.find("shift") != std::string::npos)
        display.SetModifier("shift");
    else
        display.SetModifier("none");

    // Parse screen capture coordinates if provided
    // Format: "screen X Y WIDTH HEIGHT" or "ctrl screen X Y WIDTH HEIGHT"
    size_t screenPos = cmdLine.find("screen");
    if (screenPos != std::string::npos)
    {
        int x, y, width, height;
        std::string screenArgs = cmdLine.substr(screenPos + 6); // Skip "screen"

        if (sscanf(screenArgs.c_str(), "%d %d %d %d", &x, &y, &width, &height) == 4)
        {
            display.SetScreenCaptureRegion(x, y, width, height);
        }
    }

    if (display.Initialize())
    {
        display.Run();
    }

    return 0;
}