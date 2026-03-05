#pragma once

#include "WidgetRenderData.h"
#include <MW5.h>
#include <PluginExtension.h>

class Renderer;

class HUD final : public PluginExtension
{
  public:
    static inline HUD *Instance;

    HUD();
    virtual ~HUD() override;

  private:
    Renderer *Renderer = nullptr;
    HANDLE ExportMMF = nullptr;
    LPVOID ExportMMFBuffer = nullptr;

    API::UObject *Pawn = nullptr;
    FPrimitiveComponentId TargetBoxId = {};

    // MFD Screen Component IDs
    FPrimitiveComponentId TargetScreenId = {};
    FPrimitiveComponentId WeaponsScreenId = {};
    FPrimitiveComponentId OwnerScreenId = {};
    FPrimitiveComponentId ObjectivesScreenId = {};
    FPrimitiveComponentId MapScreenId = {};
    FPrimitiveComponentId CommsScreenId = {};

    float *CurrentZoomLevel = nullptr;
    float *Brightness = nullptr;
    float CurrentBrightness = 1.0f;
    HUDWidgetRenderData HUDWidgets[10]{}; // 4 crosshairs + 6 screens
    std::vector<MarkerWidgetRenderData> MarkerWidgets[3]{};
    float ZoomLevel[3]{};
    bool InMech[3]{};
    bool CurrentlyInMech = false;

    void Reset();
    void SetupExportMMF();

    virtual void OnInitialize() override;

    bool OnNewPawn(API::UObject *activePawn);

    bool TryGetWidget3DRenderData(const char *name, WidgetType type, API::UObject *component, bool logFailures = true);

    static int32_t SCanvas_OnPaint(SCanvas *self, void *args, void *allottedGeometry, void *myCullingRect,
                                   void *outDrawElements, int32_t layerId, void *inWidgetStyle, bool bParentEnabled);

    static void FRendererModule_BeginRenderingViewFamily(FRendererModule *self, FCanvas *canvas,
                                                         FSceneViewFamily *viewFamily);

    static void FWidget3DSceneProxy_GetDynamicMeshElements(FWidget3DSceneProxy *self,
                                                           const TArray<const FSceneView *> &views,
                                                           const FSceneViewFamily &viewFamily, uint32_t visibilityMap,
                                                           void *collector);

    static bool FWidget3DSceneProxy_CanBeOccluded(FWidget3DSceneProxy *self);

    static void ProcessHUDWidget(const FWidget3DSceneProxy *proxy, const TArray<const FSceneView *> &views,
                                 uint32_t frameIndex, HUDWidgetRenderData *widget);

    static void ProcessMarkerWidget(FWidget3DSceneProxy *proxy, const TArray<const FSceneView *> &views,
                                    bool isZoomCamera, float zoomLevel, FPrimitiveComponentId targetBoxId,
                                    std::vector<MarkerWidgetRenderData> &markers);

    static mat4 GetModelMatrix(const mat4 &localToWorld, int32_t sizeX, int32_t sizeY, float zoomScale);

    bool ValidateMech();

    static void PostProcessMotionBlur_AddMotionBlurVelocityPass(FRDGBuilder *graphBuilder, const FViewInfo &view,
                                                                const FMotionBlurViewports &viewports,
                                                                FRDGTexture *colorTexture, FRDGTexture *depthTexture,
                                                                FRDGTexture *velocityTexture,
                                                                FRDGTexture **velocityFlatTextureOutput,
                                                                FRDGTexture **velocityTileTextureOutput);

    static void FMotionBlurFilterPS_TRDGLambdaPass_ExecuteImpl(FMotionBlurFilterPS::TRDGLambdaPass *self,
                                                               FRHICommandList *rhiCmdList);
};