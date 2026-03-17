#include "HUD.h"
#include "Offsets.h"
#include "Renderer.h"
#include <RE.h>

HUD::HUD()
{
    Instance = this;
    PluginExtension::Instance = this;
    Name = "HUD";
    Version = "2.1.0";
    VersionInt = 210;
    VersionCheckFnName = L"OnFetchHUDPluginData";
    VersionPropertyName = L"HUDVersion";
    Renderer = new ::Renderer();
}

HUD::~HUD()
{
    FWidget3DSceneProxy::GetDynamicMeshElements.Uninstall();
    FWidget3DSceneProxy::CanBeOccluded.Uninstall();
    FMotionBlurFilterPS::TRDGLambdaPass::ExecuteImpl.Uninstall();
    PostProcessMotionBlur::AddMotionBlurVelocityPass.Uninstall();
    FRendererModule::BeginRenderingViewFamily.Uninstall();
    SCanvas::OnPaint.Uninstall();

    // Clean up MMF
    if (ExportMMFBuffer)
    {
        UnmapViewOfFile(ExportMMFBuffer);
        ExportMMFBuffer = nullptr;
    }
    if (ExportMMF)
    {
        CloseHandle(ExportMMF);
        ExportMMF = nullptr;
    }
}

void HUD::OnInitialize()
{
    Offsets::FindAll();
    PostProcessMotionBlur::AddMotionBlurVelocityPass.DetourOffset(Offsets::AddMotionBlurVelocityPass_Offset,
                                                                  &PostProcessMotionBlur_AddMotionBlurVelocityPass);
    FMotionBlurFilterPS::TRDGLambdaPass::ExecuteImpl.DetourOffset(Offsets::ExecuteImpl_Offset,
                                                                  &FMotionBlurFilterPS_TRDGLambdaPass_ExecuteImpl);
    FRendererModule::BeginRenderingViewFamily.DetourOffset(Offsets::FRendererModule_BeginRenderingViewFamily_Offset,
                                                           &FRendererModule_BeginRenderingViewFamily);
    SCanvas::OnPaint.DetourOffset(Offsets::SCanvas_OnPaint_Offset, &SCanvas_OnPaint);

    SetupExportMMF();
}

int32_t HUD::SCanvas_OnPaint(SCanvas *self, void *args, void *allottedGeometry, void *myCullingRect,
                             void *outDrawElements, int32_t layerId, void *inWidgetStyle, bool bParentEnabled)
{
    if (Instance->CurrentlyInMech)
        return layerId;

    return SCanvas::OnPaint.OriginalFn(self, args, allottedGeometry, myCullingRect, outDrawElements, layerId,
                                       inWidgetStyle, bParentEnabled);
}

void HUD::FRendererModule_BeginRenderingViewFamily(FRendererModule *self, FCanvas *canvas, FSceneViewFamily *viewFamily)
{
    FRendererModule::BeginRenderingViewFamily.OriginalFn(self, canvas, viewFamily);

    const uint32_t frameIndex = viewFamily->FrameNumber % 3;

    Instance->CurrentlyInMech = Instance->ValidateMech();
    Instance->InMech[frameIndex] = Instance->CurrentlyInMech;
    Instance->ZoomLevel[frameIndex] =
        Instance->CurrentlyInMech && Instance->CurrentZoomLevel ? *Instance->CurrentZoomLevel : 1.0f;
    Instance->CurrentBrightness = Instance->CurrentlyInMech && Instance->Brightness ? *Instance->Brightness : 1.0f;
}

void HUD::Reset()
{
    CurrentlyInMech = false;
    Pawn = nullptr;
    TargetBoxId = {};

    // Reset MFD screen IDs
    TargetScreenId = {};
    WeaponsScreenId = {};
    OwnerScreenId = {};
    ObjectivesScreenId = {};
    MapScreenId = {};
    CommsScreenId = {};

    Brightness = nullptr;
    CurrentZoomLevel = nullptr;
    CurrentBrightness = 1.0f;

    for (auto &w : HUDWidgets)
    {
        w.ComponentId = FPrimitiveComponentId{};
        w.Texture = nullptr;
        w.SRV = nullptr;
        w.RenderTargetSizeX = 0;
        w.RenderTargetSizeY = 0;
        std::memset(w.MVPs, 0, sizeof(w.MVPs));
        std::memset(w.RequestDraw, 0, sizeof(w.RequestDraw));
    }

    for (auto &vec : MarkerWidgets)
    {
        vec.clear();
    }

    for (auto &z : ZoomLevel)
    {
        z = 1.0f;
    }

    for (auto &im : InMech)
    {
        im = false;
    }
}

bool HUD::OnNewPawn(API::UObject *activePawn)
{
    if (Pawn)
        Reset();

    Pawn = activePawn;
    if (!activePawn)
        return false;

    API::UObject *mechView, *mechMesh = nullptr;

    bool success =
        TryGetProperty(Pawn, L"MechViewComponent", mechView) && TryGetProperty(Pawn, L"MechMeshComponent", mechMesh);

    if (!success)
    {
        LogInfo("Failed to get Mech references - probably not in a Mech");
        return false;
    }

    API::UObject *cockpit, *mechCockpit, *torsoCrosshair, *armsCrosshair, *armsTargetCrosshair, *headCrosshair,
        *targetBox, *hudManager = nullptr;

    success = TryGetProperty(mechMesh, L"FirstPersonCockpitComponent", cockpit) &&
              TryGetProperty(cockpit, L"ChildActor", mechCockpit) &&
              TryGetProperty(mechCockpit, L"VR_TorsoCrosshair", torsoCrosshair) &&
              TryGetProperty(mechCockpit, L"VR_ArmsCrosshair", armsCrosshair) &&
              TryGetProperty(mechCockpit, L"VR_ArmsTargetCrosshair", armsTargetCrosshair) &&
              TryGetProperty(mechCockpit, L"VR_HeadCrosshair", headCrosshair) &&
              TryGetProperty(mechCockpit, L"VR_TargetBox", targetBox) &&
              TryGetProperty(mechCockpit, L"VR_HUDManager", hudManager) &&
              TryGetPropertyStruct(hudManager, L"Brightness", Brightness) &&
              TryGetPropertyStruct(hudManager, L"ZoomLevel", CurrentZoomLevel);

    if (!success)
    {
        LogInfo("Failed to get Mech HUD references, retrying next frame...");
        Pawn = nullptr;
        Reset();
        return false;
    }

    // ========== CAPTURE MFD SCREEN COMPONENT IDs ==========
    // Try multiple property name variations to support different mechs

    API::UObject *targetScreen = nullptr;
    API::UObject *weaponsScreen = nullptr;
    API::UObject *ownerScreen = nullptr;
    API::UObject *objectivesScreen = nullptr;
    API::UObject *mapScreen = nullptr;
    API::UObject *commsScreen = nullptr;

    // Target Screen - try multiple variations on MechCockpit
    if (!TryGetPropertyStruct(mechCockpit, L"VR_TargetScreenLoadoutSidesComponent", targetScreen))
    {
        // Warhammer and some other mechs use "Below" variant instead of "Sides"
        if (!TryGetPropertyStruct(mechCockpit, L"VR_TargetScreenLoadoutBelowComponent", targetScreen))
        {
            if (!TryGetPropertyStruct(mechCockpit, L"VR_TargetScreenComponent", targetScreen))
            {
                if (!TryGetPropertyStruct(mechCockpit, L"VR_TargetLoadoutScreenComponent", targetScreen))
                {
                    if (!TryGetPropertyStruct(mechCockpit, L"VR_TargetingScreenComponent", targetScreen))
                    {
                        if (!TryGetPropertyStruct(mechCockpit, L"VR_TargetScreenLoadoutComponent", targetScreen))
                        {
                            if (!TryGetPropertyStruct(mechCockpit, L"VR_LoadoutScreenComponent", targetScreen))
                            {
                                TryGetPropertyStruct(mechCockpit, L"VR_TargetScreenSidesComponent", targetScreen);
                            }
                        }
                    }
                }
            }
        }
    }

    TryGetPropertyStruct(mechCockpit, L"VR_WeaponsScreenComponent", weaponsScreen);
    TryGetPropertyStruct(mechCockpit, L"VR_OwnerScreenComponent", ownerScreen);
    TryGetPropertyStruct(mechCockpit, L"VR_ObjectivesScreenComponent", objectivesScreen);
    TryGetPropertyStruct(mechCockpit, L"VR_MapScreenComponent", mapScreen);
    TryGetPropertyStruct(mechCockpit, L"VR_LanceCommsScreenComponent", commsScreen);

    // Dereference to get actual object pointers
    API::UObject *actualTargetScreen = targetScreen ? *reinterpret_cast<API::UObject **>(targetScreen) : nullptr;
    API::UObject *actualWeaponsScreen = weaponsScreen ? *reinterpret_cast<API::UObject **>(weaponsScreen) : nullptr;
    API::UObject *actualOwnerScreen = ownerScreen ? *reinterpret_cast<API::UObject **>(ownerScreen) : nullptr;
    API::UObject *actualObjectivesScreen =
        objectivesScreen ? *reinterpret_cast<API::UObject **>(objectivesScreen) : nullptr;
    API::UObject *actualMapScreen = mapScreen ? *reinterpret_cast<API::UObject **>(mapScreen) : nullptr;
    API::UObject *actualCommsScreen = commsScreen ? *reinterpret_cast<API::UObject **>(commsScreen) : nullptr;

    // Capture component IDs for MFD screens
    if (actualTargetScreen)
    {
        TargetScreenId = ((UPrimitiveComponent *)actualTargetScreen)->ComponentId;
        LogInfo("Captured TargetScreen ComponentId: %u", TargetScreenId.PrimIdValue);
    }
    else
    {
        LogInfo("Target Screen not found - may not be available on this mech");
    }

    if (actualWeaponsScreen)
    {
        WeaponsScreenId = ((UPrimitiveComponent *)actualWeaponsScreen)->ComponentId;
        LogInfo("Captured WeaponsScreen ComponentId: %u", WeaponsScreenId.PrimIdValue);
    }
    if (actualOwnerScreen)
    {
        OwnerScreenId = ((UPrimitiveComponent *)actualOwnerScreen)->ComponentId;
        LogInfo("Captured OwnerScreen ComponentId: %u", OwnerScreenId.PrimIdValue);
    }
    if (actualObjectivesScreen)
    {
        ObjectivesScreenId = ((UPrimitiveComponent *)actualObjectivesScreen)->ComponentId;
        LogInfo("Captured ObjectivesScreen ComponentId: %u", ObjectivesScreenId.PrimIdValue);
    }
    if (actualMapScreen)
    {
        MapScreenId = ((UPrimitiveComponent *)actualMapScreen)->ComponentId;
        LogInfo("Captured MapScreen ComponentId: %u", MapScreenId.PrimIdValue);
    }
    if (actualCommsScreen)
    {
        CommsScreenId = ((UPrimitiveComponent *)actualCommsScreen)->ComponentId;
        LogInfo("Captured CommsScreen ComponentId: %u", CommsScreenId.PrimIdValue);
    }
    // ========== END MFD SCREEN CAPTURE ==========

    success =
        TryGetWidget3DRenderData("VR_TorsoCrosshair", WidgetType::TorsoCrosshair, torsoCrosshair) &&
        TryGetWidget3DRenderData("VR_ArmsCrosshair", WidgetType::ArmsCrosshair, armsCrosshair) &&
        TryGetWidget3DRenderData("VR_ArmsTargetCrosshair", WidgetType::ArmsTargetCrosshair, armsTargetCrosshair) &&
        TryGetWidget3DRenderData("VR_HeadCrosshair", WidgetType::HeadCrosshair, headCrosshair);

    if (!success)
    {
        LogInfo("Failed to get Mech HUD data, retrying next frame...");
        Pawn = nullptr;
        Reset();
        return false;
    }

    TargetBoxId = ((UPrimitiveComponent *)targetBox)->ComponentId;

    LogInfo("Mech HUD references acquired");
    return true;
}

bool HUD::TryGetWidget3DRenderData(const char *name, WidgetType type, API::UObject *component, const bool logFailures)
{
    auto widget = &HUDWidgets[(int)type];

    widget->Type = type;
    const auto *primitiveComponent = (UPrimitiveComponent *)component;
    widget->ComponentId = primitiveComponent->ComponentId;

    if (!FWidget3DSceneProxy::GetDynamicMeshElements.Installed)
    {
        auto *proxy = (FWidget3DSceneProxy *)primitiveComponent->SceneProxy;
        if (!proxy)
        {
            if (logFailures)
                LogError("Failed to get %s Widget3DSceneProxy, retrying next frame...", name);
            return false;
        }
        FWidget3DSceneProxy::GetDynamicMeshElements.DetourFromInstance(proxy,
                                                                       &FWidget3DSceneProxy_GetDynamicMeshElements);
        FWidget3DSceneProxy::CanBeOccluded.DetourFromInstance(proxy, &FWidget3DSceneProxy_CanBeOccluded);
    }

    return true;
}

void HUD::FWidget3DSceneProxy_GetDynamicMeshElements(FWidget3DSceneProxy *self, const TArray<const FSceneView *> &views,
                                                     const FSceneViewFamily &viewFamily, uint32_t visibilityMap,
                                                     void *collector)
{
    if (!self || views.Count == 0)
        return;

    const uint32_t frameIndex = viewFamily.FrameNumber % 3;
    const uint32_t componentId = self->PrimitiveComponentId.PrimIdValue;

    // ========== CHECK FOR MFD SCREENS FIRST ==========
    int screenWidgetIndex = -1;
    // Only match if the screen ID is actually set (not zero)
    if (Instance->TargetScreenId.PrimIdValue != 0 && componentId == Instance->TargetScreenId.PrimIdValue)
        screenWidgetIndex = 4;
    else if (Instance->WeaponsScreenId.PrimIdValue != 0 && componentId == Instance->WeaponsScreenId.PrimIdValue)
        screenWidgetIndex = 5;
    else if (Instance->OwnerScreenId.PrimIdValue != 0 && componentId == Instance->OwnerScreenId.PrimIdValue)
        screenWidgetIndex = 6;
    else if (Instance->ObjectivesScreenId.PrimIdValue != 0 && componentId == Instance->ObjectivesScreenId.PrimIdValue)
        screenWidgetIndex = 7;
    else if (Instance->MapScreenId.PrimIdValue != 0 && componentId == Instance->MapScreenId.PrimIdValue)
        screenWidgetIndex = 8;
    else if (Instance->CommsScreenId.PrimIdValue != 0 && componentId == Instance->CommsScreenId.PrimIdValue)
        screenWidgetIndex = 9;

    if (screenWidgetIndex >= 0)
    {
        // This is an MFD screen - capture it
        auto &screenWidget = Instance->HUDWidgets[screenWidgetIndex];
        screenWidget.Type = (WidgetType)screenWidgetIndex;
        Instance->ProcessHUDWidget(self, views, frameIndex, &screenWidget);

        // Still render normally in VR
        FWidget3DSceneProxy::GetDynamicMeshElements.OriginalFn(self, views, viewFamily, visibilityMap, collector);
        return;
    }
    // ========== END MFD SCREEN CHECK ==========

    // Handle crosshairs
    if (self->BlendMode == EWidgetBlendMode::Opaque || !Instance->InMech[frameIndex])
    {
        FWidget3DSceneProxy::GetDynamicMeshElements.OriginalFn(self, views, viewFamily, visibilityMap, collector);
        return;
    }

    auto &widgets = Instance->HUDWidgets;
    HUDWidgetRenderData *widget = nullptr;

    for (int i = 0; i < 4; ++i)
    {
        if (widgets[i].ComponentId.PrimIdValue == self->PrimitiveComponentId.PrimIdValue)
        {
            widget = &widgets[i];
            break;
        }
    }

    const auto view = views.Data[0];
    const bool isZoomCamera = views.Count == 1 && view->bIsSceneCapture;
    const bool isMainPass = views.Count == 2 || view->StereoPass == eSSP_FULL && !isZoomCamera;

    if (widget)
    {
        if (isMainPass)
            ProcessHUDWidget(self, views, frameIndex, widget);
    }
    else if (isMainPass || isZoomCamera)
        ProcessMarkerWidget(self, views, isZoomCamera, Instance->ZoomLevel[frameIndex], Instance->TargetBoxId,
                            Instance->MarkerWidgets[frameIndex]);
}

bool HUD::FWidget3DSceneProxy_CanBeOccluded(FWidget3DSceneProxy *)
{
    return false;
}

void HUD::ProcessHUDWidget(const FWidget3DSceneProxy *proxy, const TArray<const FSceneView *> &views,
                           const uint32_t frameIndex, HUDWidgetRenderData *widget)
{
    widget->Texture = nullptr;
    widget->SRV = nullptr;

    if (!proxy || !proxy->RenderTarget || !proxy->RenderTarget->Resource ||
        !proxy->RenderTarget->Resource->Texture2DRHI)
        return;

    const ComPtr<ID3D11Texture2D> tex = proxy->RenderTarget->Resource->Texture2DRHI->GetNativeResource();
    const ComPtr<ID3D11ShaderResourceView> srv =
        proxy->RenderTarget->Resource->Texture2DRHI->GetNativeShaderResourceView();

    if (!tex || !srv)
        return;

    widget->Texture = tex;
    widget->SRV = srv;
    widget->RenderTargetSizeX = proxy->RenderTarget->SizeX;
    widget->RenderTargetSizeY = proxy->RenderTarget->SizeY;

    // Log MFD screen sizes to debug cropping
    //if ((int)widget->Type >= 4 && (int)widget->Type <= 9)
    //{
        //Instance->LogInfo("MFD Widget %d: Captured texture size %dx%d", (int)widget->Type, proxy->RenderTarget->SizeX,
                          //proxy->RenderTarget->SizeY);
    //}

    const auto &m = GetModelMatrix(proxy->LocalToWorld, widget->RenderTargetSizeX, widget->RenderTargetSizeY, 1.0f);

    for (auto eye = 0; eye < views.Count; eye++)
    {
        const auto &matrices = views.Data[eye]->ViewMatrices;

        widget->MVPs[frameIndex][eye] = matrices.ProjectionNoAAMatrix * matrices.TranslatedViewMatrix *
                                        translate(mat4(1), matrices.PreViewTranslation) * m;

        // Only set RequestDraw for crosshairs (0-3), not MFD screens (4-9)
        // MFD screens are rendered normally by the game, only crosshairs get HUD overlay
        if ((int)widget->Type < 4)
        {
            widget->RequestDraw[frameIndex][eye] = true;
        }
    }
}

void HUD::ProcessMarkerWidget(FWidget3DSceneProxy *proxy, const TArray<const FSceneView *> &views,
                              const bool isZoomCamera, const float zoomLevel, const FPrimitiveComponentId targetBoxId,
                              std::vector<MarkerWidgetRenderData> &markers)
{
    if (zoomLevel > 1.02f && !isZoomCamera || zoomLevel <= 1.02f && isZoomCamera)
    {
        return;
    }

    if (!proxy || !proxy->RenderTarget || !proxy->RenderTarget->Resource ||
        !proxy->RenderTarget->Resource->Texture2DRHI)
        return;

    const ComPtr<ID3D11Texture2D> tex = proxy->RenderTarget->Resource->Texture2DRHI->GetNativeResource();
    const ComPtr<ID3D11ShaderResourceView> srv =
        proxy->RenderTarget->Resource->Texture2DRHI->GetNativeShaderResourceView();

    if (!tex || !srv)
        return;

    MarkerWidgetRenderData widget;
    widget.ComponentId = proxy->PrimitiveComponentId;
    widget.Texture = tex;
    widget.SRV = srv;
    widget.RenderTargetSizeX = proxy->RenderTarget->SizeX;
    widget.RenderTargetSizeY = proxy->RenderTarget->SizeY;

    const float zoomScale = targetBoxId.PrimIdValue == widget.ComponentId.PrimIdValue ? 1.0f : 1.0f / zoomLevel;

    const auto &m = GetModelMatrix(proxy->LocalToWorld, widget.RenderTargetSizeX, widget.RenderTargetSizeY, zoomScale);
    for (auto eye = 0; eye < views.Count; eye++)
    {
        const auto &matrices = views.Data[eye]->ViewMatrices;
        widget.MVPs[eye] = matrices.ProjectionNoAAMatrix * matrices.TranslatedViewMatrix *
                           translate(mat4(1), matrices.PreViewTranslation) * m;
    }

    markers.push_back(widget);
}

mat4 HUD::GetModelMatrix(const mat4 &localToWorld, const int32_t sizeX, const int32_t sizeY, const float zoomScale)
{
    const float scaleMult = (float)sizeX / 2.0f;
    const float scaleRatio = (float)sizeY / (float)sizeX;

    auto scale = vec3{1, 1, 1} * zoomScale;
    scale *= scaleMult;
    scale.y *= scaleRatio;

    auto m = localToWorld;
    m = rotate(m, glm::half_pi<float>(), vec3(0, 1, 0));
    m = rotate(m, glm::half_pi<float>(), vec3(0, 0, 1));
    m[0] *= scale.x;
    m[1] *= scale.y;
    m[2] *= scale.z;
    return m;
}

void HUD::SetupExportMMF()
{
    LogInfo("SetupExportMMF - Starting...");

    constexpr size_t MMF_SIZE = sizeof(ExportControlBlock);
    LogInfo("  MMF size: %zu bytes", MMF_SIZE);

    ExportMMF = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(MMF_SIZE),
                                   L"MechWarriorVR_HUDExport");

    if (!ExportMMF)
    {
        LogError("  Failed to create MMF: error %lu", GetLastError());
        return;
    }

    LogInfo("  MMF created successfully");

    ExportMMFBuffer = MapViewOfFile(ExportMMF, FILE_MAP_ALL_ACCESS, 0, 0, MMF_SIZE);

    if (!ExportMMFBuffer)
    {
        DWORD mapError = GetLastError();
        LogError("  Failed to map view: error %lu", mapError);
        CloseHandle(ExportMMF);
        ExportMMF = nullptr;
        return;
    }

    LogInfo("  MMF view mapped successfully at %p", ExportMMFBuffer);

    // Initialize control block
    auto *control = static_cast<ExportControlBlock *>(ExportMMFBuffer);
    ZeroMemory(control, MMF_SIZE);

    control->ExportEnabled = true;
    control->WidgetCount = 0;
    control->SelectedWidgetIndex = 0;

    LogInfo("SetupExportMMF - Complete!");

    // Pass control block pointer to renderer
    Renderer->SetExportControl(control);
    Renderer->EnableExport(true);
}

void HUD::PostProcessMotionBlur_AddMotionBlurVelocityPass(FRDGBuilder *, const FViewInfo &,
                                                          const FMotionBlurViewports &, FRDGTexture *, FRDGTexture *,
                                                          FRDGTexture *, FRDGTexture **, FRDGTexture **)
{
}

void HUD::FMotionBlurFilterPS_TRDGLambdaPass_ExecuteImpl(FMotionBlurFilterPS::TRDGLambdaPass *self, FRHICommandList *)
{
    const auto &lambda = self->ExecuteLambda;
    const auto view = lambda.View;
    const auto &params = lambda.PixelShaderParameters;
    const auto pass = view->StereoPass;
    const auto frameNumber = lambda.View->Family->FrameNumber;
    const auto &viewport = params->Filter.Color;
    const auto sceneColor = params->Filter.ColorTexture;
    const auto &renderTarget = params->RenderTargets.Outputs[0].Texture;
    const float zoomLevel = Instance->ZoomLevel[frameNumber % 3];
    const bool inMech = Instance->InMech[frameNumber % 3] && Instance->CurrentlyInMech;

    Instance->Renderer->RenderHUD(Instance->HUDWidgets, Instance->MarkerWidgets, Instance->CurrentBrightness, zoomLevel,
                                  pass, frameNumber, viewport, sceneColor, renderTarget, inMech);
}

bool HUD::ValidateMech()
{
    if (const auto activePawn = API::get()->get_local_pawn(0))
    {
        if (activePawn != Pawn)
            return OnNewPawn(activePawn);
        return CurrentlyInMech;
    }

    if (Pawn != nullptr)
        Reset();

    return false;
}

std::unique_ptr<HUD> g_plugin{new HUD()};