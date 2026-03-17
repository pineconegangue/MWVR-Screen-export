#include "Renderer.h"

#include "Log.h"
#include "Util.h"
#include <atomic>
#include <dxgi.h>
#pragma comment(lib, "dxgi.lib")

bool Renderer::HUDTarget::Ensure(ID3D11Device *dvc, ID3D11Texture2D *backbuffer)
{
    {
        if (!dvc || !backbuffer)
            return false;

        D3D11_TEXTURE2D_DESC bb{};
        backbuffer->GetDesc(&bb);

        const bool needsRecreate = !Tex || Width != bb.Width || Height != bb.Height;

        if (!needsRecreate)
            return true;

        Tex.Reset();
        RTV.Reset();
        SRV.Reset();

        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11ShaderResourceView> srv;

        Width = bb.Width;
        Height = bb.Height;
        sampleCount = bb.SampleDesc.Count;
        sampleQuality = bb.SampleDesc.Quality;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = Width;
        td.Height = Height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        td.SampleDesc.Count = sampleCount;
        td.SampleDesc.Quality = sampleQuality;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = 0;
        td.MiscFlags = 0;

        if (FAILED(dvc->CreateTexture2D(&td, nullptr, tex.GetAddressOf())))
            return false;

        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rtvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        rtvDesc.Texture2D.MipSlice = 0;
        if (FAILED(dvc->CreateRenderTargetView(tex.Get(), &rtvDesc, rtv.GetAddressOf())))
            return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;

        if (FAILED(dvc->CreateShaderResourceView(tex.Get(), &srvDesc, srv.GetAddressOf())))
            return false;

        Tex = tex;
        SRV = srv;
        RTV = rtv;

        Log::LogInfo("HUD render target created: %ux%u", Width, Height);

        return true;
    }
}

bool Renderer::CompileAndCreateVertexShader(ID3D11Device *dvc, const wchar_t *filename, ID3D11VertexShader **vs,
                                            ID3DBlob **b)
{
    const auto modulePath = Util::GetModulePath();
    const auto path = modulePath / L"shaders" / filename;
    Log::LogInfo("Compiling Vertex Shader %ls...", path.c_str());

    ComPtr<ID3DBlob> err;

    auto hr =
        D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "Main", "vs_5_0", 0, 0, b, &err);
    if (FAILED(hr))
    {
        if (err)
            Log::LogError("VS compile error: %s", (const char *)err->GetBufferPointer());
        else
            Log::LogError("VS compile error: unknown (HRESULT 0x%08X)", hr);

        return false;
    }

    hr = dvc->CreateVertexShader((*b)->GetBufferPointer(), (*b)->GetBufferSize(), nullptr, vs);
    if (FAILED(hr))
    {
        Log::LogError("VS creation error: (HRESULT 0x%08X)", hr);
        return false;
    }

    Log::LogInfo("Vertex shader compiled!");
    return true;
}

bool Renderer::CompileAndCreatePixelShader(ID3D11Device *dvc, const wchar_t *filename, ID3D11PixelShader **ps,
                                           ID3DBlob **b)
{
    const auto modulePath = Util::GetModulePath();
    const auto path = modulePath / L"shaders" / filename;
    Log::LogInfo("Compiling Pixel Shader %ls...", path.c_str());

    ComPtr<ID3DBlob> err;

    auto hr =
        D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "Main", "ps_5_0", 0, 0, b, &err);
    if (FAILED(hr))
    {
        if (err)
            Log::LogError("PS compile error: %s", (const char *)err->GetBufferPointer());
        else
            Log::LogError("PS compile error: unknown (HRESULT 0x%08X)", hr);

        return false;
    }

    hr = dvc->CreatePixelShader((*b)->GetBufferPointer(), (*b)->GetBufferSize(), nullptr, ps);
    if (FAILED(hr))
    {
        Log::LogError("PS creation error: (HRESULT 0x%08X)", hr);
        return false;
    }

    Log::LogInfo("Pixel shader compiled!");
    return true;
}

bool Renderer::EnsurePipeline(ID3D11Device *dvc)
{
    if (TexturedQuadVS && HUDOverlayPS && FullScreenVS && PassthroughPS && InputLayout && VertexBuffer && Sampler &&
        BlendStateOverwrite && BlendStateHUDCompose && BlendStateHUDOverlay && DepthStencilState && RasterizerState &&
        QuadConstantsCB && PSConstantsCB && VSConstantsCB)
    {
        return true;
    }

    Log::LogInfo("Creating rendering pipeline...");

    ComPtr<ID3DBlob> vsBlob, psBlob;
    if (!CompileAndCreateVertexShader(dvc, L"TexturedQuadVS.hlsl", TexturedQuadVS.GetAddressOf(),
                                      vsBlob.GetAddressOf()) ||
        !CompileAndCreatePixelShader(dvc, L"HUDOverlayPS.hlsl", HUDOverlayPS.GetAddressOf(), psBlob.GetAddressOf()) ||
        !CompileAndCreateVertexShader(dvc, L"FullScreenVS.hlsl", FullScreenVS.GetAddressOf(), vsBlob.GetAddressOf()) ||
        !CompileAndCreatePixelShader(dvc, L"PassthroughPS.hlsl", PassthroughPS.GetAddressOf(), psBlob.GetAddressOf()))
    {
        return false;
    }

    if (!CompileAndCreateVertexShader(dvc, L"TexturedQuadVS.hlsl", TexturedQuadVS.GetAddressOf(),
                                      vsBlob.GetAddressOf()))
        return false;

    D3D11_INPUT_ELEMENT_DESC ied[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    if (FAILED(dvc->CreateInputLayout(ied, std::size(ied), vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                      InputLayout.GetAddressOf())))
        return false;

    constexpr Vertex verts[] = {
        {.Px = 1.f, .Py = -1.f, .U = 0.f, .V = 1.f}, {.Px = 1.f, .Py = 1.f, .U = 0.f, .V = 0.f},
        {.Px = -1.f, .Py = 1.f, .U = 1.f, .V = 0.f}, {.Px = 1.f, .Py = -1.f, .U = 0.f, .V = 1.f},
        {.Px = -1.f, .Py = 1.f, .U = 1.f, .V = 0.f}, {.Px = -1.f, .Py = -1.f, .U = 1.f, .V = 1.f},
    };
    D3D11_BUFFER_DESC vbd{};
    vbd.ByteWidth = sizeof(verts);
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbd.CPUAccessFlags = 0;
    vbd.MiscFlags = 0;

    D3D11_SUBRESOURCE_DATA vbData{};
    vbData.pSysMem = verts;
    if (FAILED(dvc->CreateBuffer(&vbd, &vbData, VertexBuffer.GetAddressOf())))
        return false;

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dvc->CreateSamplerState(&samplerDesc, Sampler.GetAddressOf())))
        return false;

    D3D11_BLEND_DESC overwriteDesc{};
    overwriteDesc.AlphaToCoverageEnable = FALSE;
    overwriteDesc.IndependentBlendEnable = FALSE;
    D3D11_RENDER_TARGET_BLEND_DESC &rtbd = overwriteDesc.RenderTarget[0];
    rtbd.BlendEnable = FALSE;
    rtbd.SrcBlend = D3D11_BLEND_ONE;
    rtbd.DestBlend = D3D11_BLEND_ZERO;
    rtbd.BlendOp = D3D11_BLEND_OP_ADD;
    rtbd.SrcBlendAlpha = D3D11_BLEND_ONE;
    rtbd.DestBlendAlpha = D3D11_BLEND_ZERO;
    rtbd.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rtbd.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dvc->CreateBlendState(&overwriteDesc, BlendStateOverwrite.GetAddressOf())))
        return false;

    D3D11_BLEND_DESC composeDesc{};
    composeDesc.AlphaToCoverageEnable = FALSE;
    composeDesc.IndependentBlendEnable = FALSE;
    D3D11_RENDER_TARGET_BLEND_DESC &crbd = composeDesc.RenderTarget[0];
    crbd.BlendEnable = TRUE;
    crbd.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    crbd.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    crbd.BlendOp = D3D11_BLEND_OP_ADD;
    crbd.SrcBlendAlpha = D3D11_BLEND_ONE;
    crbd.DestBlendAlpha = D3D11_BLEND_ZERO;
    crbd.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    crbd.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dvc->CreateBlendState(&composeDesc, BlendStateHUDCompose.GetAddressOf())))
        return false;

    D3D11_BLEND_DESC overlayDesc{};
    overlayDesc.AlphaToCoverageEnable = FALSE;
    overlayDesc.IndependentBlendEnable = FALSE;
    D3D11_RENDER_TARGET_BLEND_DESC &orbd = overlayDesc.RenderTarget[0];
    orbd.BlendEnable = TRUE;
    orbd.SrcBlend = D3D11_BLEND_ONE;
    orbd.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    orbd.BlendOp = D3D11_BLEND_OP_ADD;
    orbd.SrcBlendAlpha = D3D11_BLEND_ONE;
    orbd.DestBlendAlpha = D3D11_BLEND_ZERO;
    orbd.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    orbd.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dvc->CreateBlendState(&overlayDesc, BlendStateHUDOverlay.GetAddressOf())))
        return false;

    D3D11_DEPTH_STENCIL_DESC dsDesc{};
    dsDesc.DepthEnable = FALSE;
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dsDesc.StencilEnable = FALSE;
    dsDesc.FrontFace = {};
    dsDesc.BackFace = {};
    if (FAILED(dvc->CreateDepthStencilState(&dsDesc, DepthStencilState.GetAddressOf())))
        return false;

    D3D11_RASTERIZER_DESC rsDesc{};
    rsDesc.FillMode = D3D11_FILL_SOLID;
    rsDesc.CullMode = D3D11_CULL_NONE;
    rsDesc.FrontCounterClockwise = FALSE;
    rsDesc.DepthBias = 0;
    rsDesc.DepthBiasClamp = 0;
    rsDesc.SlopeScaledDepthBias = 0;
    rsDesc.DepthClipEnable = FALSE;
    rsDesc.ScissorEnable = FALSE;
    rsDesc.MultisampleEnable = FALSE;
    rsDesc.AntialiasedLineEnable = FALSE;
    if (FAILED(dvc->CreateRasterizerState(&rsDesc, RasterizerState.GetAddressOf())))
        return false;

    D3D11_BUFFER_DESC cbd{};
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    cbd.ByteWidth = sizeof(QuadConstants);
    if (FAILED(dvc->CreateBuffer(&cbd, nullptr, QuadConstantsCB.GetAddressOf())))
        return false;

    cbd.ByteWidth = sizeof(PSConstants);
    if (FAILED(dvc->CreateBuffer(&cbd, nullptr, PSConstantsCB.GetAddressOf())))
        return false;

    cbd.ByteWidth = sizeof(VSConstants);
    if (FAILED(dvc->CreateBuffer(&cbd, nullptr, VSConstantsCB.GetAddressOf())))
        return false;

    Log::LogInfo("Rendering pipeline created");
    return true;
}

void Renderer::RenderHUD(HUDWidgetRenderData widgets[10], // Changed from [4] to [10]
                         std::vector<MarkerWidgetRenderData> markerWidgets[3], float brightness, float zoomLevel,
                         EStereoscopicPass pass, uint64_t frameNumber,
                         const FScreenPassTextureViewportParameters &viewport, FRDGTexture *sceneColor,
                         FRDGTexture *renderTarget, bool inMech)
{
    ID3D11Device *rawDevice = nullptr;
    ComPtr<ID3D11Device> dvc;
    FRHICommandList::GetNativeDevice(&rawDevice);
    dvc = rawDevice;

    if (!dvc)
    {
        Log::LogError("Failed to get RHI device");
        return;
    }

    ComPtr<ID3D11DeviceContext> ctx = nullptr;
    dvc->GetImmediateContext(ctx.GetAddressOf());
    if (!ctx)
    {
        Log::LogError("Failed to get context");
        return;
    }

    StateBackup stateBackup(ctx.Get());

    if (!sceneColor || !sceneColor->TextureRHI || !renderTarget || !renderTarget->TextureRHI)
    {
        Log::LogError("Invalid sceneColor or renderTarget");
        return;
    }

    ID3D11Texture2D *outRT = renderTarget->TextureRHI->GetNativeResource();
    if (!outRT)
    {
        Log::LogError("Failed to get render target texture");
        return;
    }

    auto *sceneSRV = sceneColor->TextureRHI->GetNativeShaderResourceView();

    if (!sceneSRV)
    {
        Log::LogError("Failed to get scene color SRV");
        return;
    }

    if (!EnsurePipeline(dvc.Get()))
    {
        Log::LogError("Failed to build pipeline");
        return;
    }

    if (!HUDTarget.Ensure(dvc.Get(), outRT))
    {
        Log::LogError("Failed to build hud target");
        return;
    }

    D3D11_TEXTURE2D_DESC rtDesc{};
    outRT->GetDesc(&rtDesc);

    ComPtr<ID3D11RenderTargetView> outRTV;
    D3D11_RENDER_TARGET_VIEW_DESC outRTVDesc{};
    outRTVDesc.Format = rtDesc.Format;
    outRTVDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    outRTVDesc.Texture2D.MipSlice = 0;
    if (HRESULT hr = dvc->CreateRenderTargetView(outRT, &outRTVDesc, outRTV.GetAddressOf()); FAILED(hr))
    {
        Log::LogError("Failed to create RTV for output texture (0x%08X)", hr);
        return;
    }

    const uint32_t frameIndex = frameNumber % 3;

    auto eye = pass == eSSP_RIGHT_EYE || pass == eSSP_RIGHT_EYE_SIDE ? 1 : 0;

    ctx->VSSetShader(TexturedQuadVS.Get(), nullptr, 0);
    ctx->PSSetShader(PassthroughPS.Get(), nullptr, 0);
    ID3D11Buffer *vsCbs[] = {QuadConstantsCB.Get()};
    ctx->VSSetConstantBuffers(13, 1, vsCbs);

    constexpr FLOAT blendFactor[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(BlendStateHUDCompose.Get(), blendFactor, 0xffffffff);
    ctx->OMSetDepthStencilState(DepthStencilState.Get(), 0);
    ctx->RSSetState(RasterizerState.Get());
    ID3D11SamplerState *samplers[] = {Sampler.Get()};
    ctx->PSSetSamplers(0, 1, samplers);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(InputLayout.Get());
    constexpr UINT stride = sizeof(Vertex), offset = 0;
    ID3D11Buffer *vbs[] = {VertexBuffer.Get()};
    ctx->IASetVertexBuffers(0, 1, vbs, &stride, &offset);

    auto viewPort = D3D11_VIEWPORT{};
    viewPort.TopLeftX = (float)viewport.ViewportMin.X;
    viewPort.TopLeftY = (float)viewport.ViewportMin.Y;
    viewPort.Width = viewport.ViewportSize.X;
    viewPort.Height = viewport.ViewportSize.Y;
    viewPort.MinDepth = 0.0f;
    viewPort.MaxDepth = 1.0f;

    constexpr FLOAT clearColor[4] = {0, 0, 0, 0};
    ctx->ClearRenderTargetView(HUDTarget.RTV.Get(), clearColor);

    if (inMech)
    {
        auto &markers = markerWidgets[frameIndex];

        if (zoomLevel > 1.02f)
        {
            const auto torsoCrosshair = widgets[(int32_t)WidgetType::TorsoCrosshair];

            if (eye == 0)
            {
                if (torsoCrosshair.RenderTargetSizeX == 0 || torsoCrosshair.RenderTargetSizeY == 0)
                    return;

                if (!ZoomCameraMarkerRTV || !ZoomCameraMarkerSRV)
                {
                    CreateZoomCameraMarkerRT(dvc.Get(), torsoCrosshair.RenderTargetSizeX,
                                             torsoCrosshair.RenderTargetSizeY, ZoomCameraMarkerTexture,
                                             ZoomCameraMarkerRTV, ZoomCameraMarkerSRV);
                    if (!ZoomCameraMarkerRTV || !ZoomCameraMarkerSRV)
                        return;
                }

                auto zoomCamViewport = D3D11_VIEWPORT{};
                zoomCamViewport.TopLeftX = 0.0f;
                zoomCamViewport.TopLeftY = 0.0f;
                zoomCamViewport.Width = (float)torsoCrosshair.RenderTargetSizeX;
                zoomCamViewport.Height = (float)torsoCrosshair.RenderTargetSizeY;
                zoomCamViewport.MinDepth = 0.0f;
                zoomCamViewport.MaxDepth = 1.0f;

                ctx->RSSetViewports(1, &zoomCamViewport);
                ctx->OMSetRenderTargets(1, ZoomCameraMarkerRTV.GetAddressOf(), nullptr);
                ctx->ClearRenderTargetView(ZoomCameraMarkerRTV.Get(), clearColor);
                ctx->PSSetShader(PassthroughPS.Get(), nullptr, 0);

                RenderMarkersToCurrentRT(ctx.Get(), &markers, 0);
            }

            if (!ZoomCameraMarkerRTV || !ZoomCameraMarkerSRV)
                return;

            ctx->RSSetViewports(1, &viewPort);
            ctx->OMSetRenderTargets(1, HUDTarget.RTV.GetAddressOf(), nullptr);
            ctx->PSSetShader(PassthroughPS.Get(), nullptr, 0);
            auto zoomSrv = ZoomCameraMarkerSRV.Get();
            ctx->PSSetShaderResources(0, 1, &zoomSrv);

            if (D3D11_MAPPED_SUBRESOURCE m;
                SUCCEEDED(ctx->Map(QuadConstantsCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
            {
                auto *vs = (QuadConstants *)m.pData;
                vs->MVP = torsoCrosshair.MVPs[frameIndex][eye];
                ctx->Unmap(QuadConstantsCB.Get(), 0);
            }

            ctx->Draw(6, 0);

            ID3D11ShaderResourceView *nullSrv[1] = {nullptr};
            ctx->PSSetShaderResources(0, 1, nullSrv);
        }
        else
        {
            ctx->RSSetViewports(1, &viewPort);
            ctx->OMSetRenderTargets(1, HUDTarget.RTV.GetAddressOf(), nullptr);
            RenderMarkersToCurrentRT(ctx.Get(), &markers, eye);
        }

        // Render crosshairs ONLY (0-3) in VR - MFD screens render normally by the game
        for (int i = 0; i < 4; ++i)
        {
            auto &widget = widgets[i];

            if (!widget.RequestDraw[frameIndex][eye] || !widget.SRV)
                continue;

            ctx->PSSetShaderResources(0, 1, widget.SRV.GetAddressOf());

            if (D3D11_MAPPED_SUBRESOURCE m;
                SUCCEEDED(ctx->Map(QuadConstantsCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
            {
                auto *vs = (QuadConstants *)m.pData;
                vs->MVP = widget.MVPs[frameIndex][eye];
                ctx->Unmap(QuadConstantsCB.Get(), 0);
            }

            ctx->Draw(6, 0);

            ID3D11ShaderResourceView *nullSrv[1] = {nullptr};
            ctx->PSSetShaderResources(0, 1, nullSrv);

            widget.RequestDraw[frameIndex][eye] = false;
        }

        if (eye == 1 || pass == eSSP_FULL)
            markers.clear();
    }

    // ========== CRITICAL FIX: Render scene passthrough OUTSIDE inMech check ==========
    // This was moved inside the if(inMech) block, causing the environment to disappear
    // Must be BEFORE the HUD overlay so the scene renders first, then HUD on top
    ctx->OMSetRenderTargets(1, outRTV.GetAddressOf(), nullptr);
    ID3D11SamplerState *samps[] = {Sampler.Get()};
    ctx->PSSetSamplers(0, 1, samps);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(FullScreenVS.Get(), nullptr, 0);
    ctx->RSSetViewports(1, &viewPort);

    float2 uvScale = {viewPort.Width / ((float)rtDesc.Width * 0.5f), 1.0f};
    float2 uvOffset = {viewPort.TopLeftX != 0.0f ? 0.5f : 0.0f, 0.0f};

    ID3D11Buffer *vsCBs[] = {VSConstantsCB.Get()};
    ctx->VSSetConstantBuffers(13, 1, vsCBs);

    ID3D11Buffer *psCBs[] = {PSConstantsCB.Get()};
    ctx->PSSetConstantBuffers(13, 1, psCBs);

    if (D3D11_MAPPED_SUBRESOURCE m; SUCCEEDED(ctx->Map(VSConstantsCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
    {
        auto *vs = (VSConstants *)m.pData;
        vs->UVOffset = uvOffset;
        vs->UVScale = uvScale;
        ctx->Unmap(VSConstantsCB.Get(), 0);
    }

    if (D3D11_MAPPED_SUBRESOURCE m; SUCCEEDED(ctx->Map(PSConstantsCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
    {
        auto *ps = (PSConstants *)m.pData;
        ps->Brightness = brightness;
        ctx->Unmap(PSConstantsCB.Get(), 0);
    }

    // Render scene color first (passthrough)
    ctx->PSSetShaderResources(0, 1, &sceneSRV);
    ctx->OMSetBlendState(BlendStateOverwrite.Get(), blendFactor, 0xffffffff);
    ctx->PSSetShader(PassthroughPS.Get(), nullptr, 0);
    ctx->Draw(3, 0);

    // Then overlay HUD if in mech
    if (inMech)
    {
        ctx->PSSetShaderResources(0, 1, HUDTarget.SRV.GetAddressOf());
        ctx->OMSetBlendState(BlendStateHUDOverlay.Get(), blendFactor, 0xffffffff);
        ctx->PSSetShader(HUDOverlayPS.Get(), nullptr, 0);
        ctx->Draw(3, 0);
    }

    // ========== EXPORT ALL ACTIVE WIDGETS ==========
    if (ExportEnabled.load() && ExportControl && eye == 0) // Only export once per frame (left eye)
    {
        // Update active widget count first
        uint32_t activeCount = 0;
        for (int i = 0; i < 10; i++)
        {
            if (widgets[i].Texture)
                activeCount++;
        }
        ExportControl->WidgetCount = activeCount;

        // Export each active widget to its own shared texture
        for (int i = 0; i < 10; i++)
        {
            auto &widgetInfo = ExportControl->Widgets[i];

            if (!widgets[i].Texture)
            {
                // Widget not active - clear its info
                widgetInfo.Active = false;
                widgetInfo.SharedTextureHandle = nullptr;
                continue;
            }

            // Widget is active - export it
            const uint32_t width = widgets[i].RenderTargetSizeX;
            const uint32_t height = widgets[i].RenderTargetSizeY;

            widgetInfo.Active = true;
            widgetInfo.Width = width;
            widgetInfo.Height = height;
            widgetInfo.Type = (uint8_t)widgets[i].Type;

            // Ensure this widget has a shared texture
            if (EnsureSharedTextureForWidget(dvc.Get(), i, width, height))
            {
                CopyToSharedTextureForWidget(ctx.Get(), i, widgets[i].Texture.Get());
                widgetInfo.SharedTextureHandle = SharedBuffers[i].SharedHandle;
            }
        }
    }
    // ========== END EXPORT ==========
}

void Renderer::RenderMarkersToCurrentRT(ID3D11DeviceContext *ctx, const std::vector<MarkerWidgetRenderData> *markers,
                                        const int32_t eye) const
{
    for (auto &marker : *markers)
    {
        if (!marker.SRV)
            continue;

        ctx->PSSetShaderResources(0, 1, marker.SRV.GetAddressOf());

        if (D3D11_MAPPED_SUBRESOURCE m; SUCCEEDED(ctx->Map(QuadConstantsCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            auto *vs = (QuadConstants *)m.pData;
            vs->MVP = marker.MVPs[eye];
            ctx->Unmap(QuadConstantsCB.Get(), 0);
        }

        ctx->Draw(6, 0);

        ID3D11ShaderResourceView *nullSrv[1] = {nullptr};
        ctx->PSSetShaderResources(0, 1, nullSrv);
    }
}

void Renderer::CreateZoomCameraMarkerRT(ID3D11Device *dvc, const int32_t width, const int32_t height,
                                        ComPtr<ID3D11Texture2D> &texture, ComPtr<ID3D11RenderTargetView> &rtv,
                                        ComPtr<ID3D11ShaderResourceView> &srv)
{
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = 0;
    td.MiscFlags = 0;

    if (FAILED(dvc->CreateTexture2D(&td, nullptr, texture.GetAddressOf())))
        return;

    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rtvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    rtvDesc.Texture2D.MipSlice = 0;
    if (FAILED(dvc->CreateRenderTargetView(texture.Get(), &rtvDesc, rtv.GetAddressOf())))
        return;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;

    if (FAILED(dvc->CreateShaderResourceView(texture.Get(), &srvDesc, srv.GetAddressOf())))
        return;

    Log::LogInfo("Zoom camera marker RT created: %dx%d", width, height);
}

bool Renderer::EnsureSharedTextureForWidget(ID3D11Device *device, int widgetIndex, uint32_t width, uint32_t height)
{
    if (widgetIndex < 0 || widgetIndex >= 10)
        return false;

    auto &buffer = SharedBuffers[widgetIndex];

    if (buffer.Texture && buffer.Width == width && buffer.Height == height)
    {
        return true;
    }

    if (buffer.Texture)
    {
        buffer.Texture.Reset(); // This automatically closes the shared handle
        // Don't manually close the handle - D3D11 manages it
        buffer.SharedHandle = nullptr;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, texture.GetAddressOf())))
    {
        Log::LogError("Failed to create shared export texture for widget %d", widgetIndex);
        return false;
    }

    ComPtr<IDXGIResource> dxgiResource;
    if (FAILED(texture.As(&dxgiResource)))
    {
        Log::LogError("Failed to get DXGI resource for widget %d", widgetIndex);
        return false;
    }

    HANDLE sharedHandle;
    if (FAILED(dxgiResource->GetSharedHandle(&sharedHandle)))
    {
        Log::LogError("Failed to get shared handle for widget %d", widgetIndex);
        return false;
    }

    buffer.Texture = texture;
    buffer.SharedHandle = sharedHandle;
    buffer.Width = width;
    buffer.Height = height;

    Log::LogInfo("Created shared export texture for widget %d: %ux%u, handle: 0x%p", widgetIndex, width, height,
                 sharedHandle);
    return true;
}

void Renderer::CopyToSharedTextureForWidget(ID3D11DeviceContext *ctx, int widgetIndex, ID3D11Texture2D *source)
{
    if (widgetIndex < 0 || widgetIndex >= 10)
        return;

    auto &buffer = SharedBuffers[widgetIndex];

    if (!buffer.Texture || !source)
    {
        return;
    }

    ComPtr<IDXGIKeyedMutex> mutex;
    if (FAILED(buffer.Texture.As(&mutex)))
    {
        return;
    }

    HRESULT hr = mutex->AcquireSync(0, 0);
    if (hr == WAIT_TIMEOUT)
    {
        return;
    }
    if (FAILED(hr))
    {
        return;
    }

    ctx->CopyResource(buffer.Texture.Get(), source);

    mutex->ReleaseSync(1);
}