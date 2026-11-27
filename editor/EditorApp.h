// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//

#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <qray/qray.h>

#include "Assets.h"
#include "EditorLightSelection.h"
#include "EditorMaterial.h"
#include "EditorPlatform.h"
#include "EditorSceneRuntime.h"
#include "EditorSession.h"
#include "EditorSettings.h"
#include "EditorUi.h"
#include "EditorViewport.h"
#include "Scene.h"

struct ImDrawList;
struct EditorAiDialogState;

struct EditorAiDialogDeleter
{
    void operator()(EditorAiDialogState *ptr) const;
};

class EditorApp
{
public:
    explicit EditorApp(void *nativeInstance = nullptr);
    ~EditorApp();

    EditorApp(const EditorApp &) = delete;
    EditorApp &operator=(const EditorApp &) = delete;

    int Run(const std::vector<std::string> &args, int showCommand = 0);

    enum class EditorMode
    {
        Material,
        Light,
    };

    enum class EditorTextureSlot
    {
        Base,
        Normals,
        Height,
        Emissive,
        Overlay,
        Gloss,
        Subsurface,
    };

    static void AutoEnableMaterialSlot(EditorMaterialParams &params, EditorTextureSlot slot);

    enum class GizmoPart
    {
        None,
        Center,
        Axis,
        SpotArc,
    };

    enum class PendingSessionAction
    {
        None,
        Exit,
        CloseScene,
        ReloadScene,
        OpenScene,
        OpenMod,
        UnloadMod,
        PlayLevel,
    };

    struct MapCatalogItem
    {
        std::string levelKey;
        std::string path;
        std::string source;
        bool isBrushSubmodel = false;
        bool hasSavedSettings = false;
    };

    struct TextureBrowserItem
    {
        std::string name;
        std::string source;
        std::string tree;
        int32_t folderIndex = -1;
    };

    struct TextureBrowserFolder
    {
        std::string path;
        std::string label;
        int32_t parent = -1;
        std::vector<int32_t> children;
    };

private:
    static void PrintMessage(const char *message, void *userData);
    // Logs the last ImGui item's screen rectangle as "panel rect: <name> x0,y0 x1,y1" whenever it
    // moves, which is how the scripted GUI checks find a control.
    void LogRect(const char *name);
    static void OpenFile(const char *path, void *userData, const void **data, uint32_t *size,
                         void **fileHandle);
    static void CloseFile(void *fileHandle, void *userData);

    bool CreateWindowInstance(int showCommand);
    bool CreateRenderer();
    bool CreateMaterial();
    bool CreateTextureMaterials();
    bool CreateUi();
    bool LoadScene();
    bool UploadScene();
    bool UploadStaticGeometry();
    bool UploadWorldClusters();
    QrMaterial SurfaceMaterial(const EditorSurface &surface) const;
    // The image a material's texture_base names, or -1 when the value is empty or names a
    // texture this scene does not carry.
    int32_t TextureIndexByName(const std::string &name) const;
    // Resolves a scene texture or loads an external PNG/TGA/JPG on demand from m_assets.
    int32_t ResolveOrLoadTexture(const std::string &name);
    EditorMaterialEmission MaterialEmissionOf(QrMaterial material) const;
    void DestroyPoolMaterials();
    void UploadScrollingSky(double timeSeconds, float skyDistance);
    // The surface's own material, replaced by the one texture_base names where it resolves, or by
    // a bake of the parameters that only a packed material texture can carry.
    QrMaterial SurfaceMaterialWithOverride(const EditorSurface &surface,
                                           const EditorMaterialParams &params);
    static QrGeometryPassThroughType PassThroughTypeForSurface(EditorSurfaceKind kind);
    void ResetCameraToScene();
    void UpdateCamera(float deltaSeconds);
    void AdvanceCelestialClock(float deltaSeconds);
    void UploadSun();
    void SunDirection(float direction[3]) const;
    void UploadLights();
    // Builds one textured area light per polygon of every surface whose material casts light.
    // Called from the static geometry pass, which is where the material bakes a light reads its
    // mask from are alive.
    void BuildEmissiveLights();
    // Re-uploads that list, which the per-frame light registry asks for: LightManager forgets
    // every registered source at the start of a frame, exactly as RT_UploadAllWorldModelLights
    // uploads the game's own area lights anew each frame.
    void UploadEmissiveLights();
    // Uploads one light under the ID the light lists know it by: a spot when its cone is on,
    // a sphere otherwise, which is the branch RT_LIGHT_Emit takes.
    void UploadLight(const EditorLight &light, uint64_t uniqueID, QrResult &lastResult);
    void UploadClusterLightSources();
    const EditorEmitterLightOverride *FindEmitterOverride(const std::string &name,
                                                          uint64_t uniqueID) const;
    EditorEmitterLightOverride EffectiveEmitterOverride(int32_t lightIndex) const;
    bool HasOwnEmitterInstanceOverride(int32_t lightIndex) const;
    void ApplyEntityEmitterEdit(int32_t lightIndex, const EditorEmitterLightOverride &edited,
                                bool reapply = true);
    void SetEntityLightGroupEdit(int32_t lightIndex, bool groupEdit);
    void ResetEntityEmitterLight(int32_t lightIndex);
    void ReapplyGeneratedLights();
    void RestoreSceneLightsAfterSessionReload();
    void ClearDirtyFlagIfClean();
    void RefreshGeneratedLightsIfSettingsChanged();
    // Reports whether one light is listed by the cluster it resolved into.
    void LogLightCluster(const EditorLight &light, uint64_t uniqueID, const char *label);
    void DrawPanel(float deltaSeconds);
    void DrawGlobalSettings();
    void DrawMaterials();
    // The game editor's texture browser: the scene's and game archives' images as a searchable,
    // folder-browsed grid of previews, opened from any texture_* row of a material.
    void RebuildTextureBrowserCatalog();
    void OpenTextureBrowserIfRequested();
    int32_t DrawTextureBrowserGrid(const std::vector<int32_t> &visible, float height);
    void DrawTextureBrowser();
    // The session of the game editor: what the panel authored since the last Apply, the file it
    // writes and the prompt an exit with unsaved changes opens.
    void LoadMaterialsSession();
    // The level scope of the session: the `sky:`, `fog:` and `lights:` blocks of one level in
    // qray.lights.yaml, read into the sky controls, the fog settings and the custom lights, and
    // written back only for the blocks the live state no longer matches.
    void LoadLightsSession();
    // The global set the panel starts from: the `name "value"` lines of the game's config.cfg,
    // which is where the cvars the Global tab drives are archived. Reads the file once, before
    // the first level scope, because after that the panel's own values are the live state.
    void LoadGameConfigGlobals();
    EditorLevelFog LevelFog() const;
    // The `sky:` block the live state stands for: the level's own overrides while the panel edits
    // the level scope, and no block at all in the global scope, which is how
    // RT_SkySettings_CommitSession drops it.
    EditorLevelSky LevelSky() const;
    // Rewrites the active scope's text from the live controls. A level key whose text matches the
    // global one is dropped rather than written, the comparison RT_SkySettings_CaptureScope makes,
    // so a scope switch does not pin a copy of the config into the level.
    void CaptureSky();
    void SetSkyScope(bool global);
    // Takes the scope and the level's keys from a section read out of the file, which is what a
    // load and a save both have to leave the panel agreeing with.
    void AdoptSkyScope(const EditorLevelDocument::ReadResult &read);
    EditorLevelLight LevelLight(const EditorLight &light, bool isCustom) const;
    std::vector<EditorLevelLight> LevelLights() const;
    bool ShouldWriteEmptyLightsBlock() const;
    // Renders the section the live state stands for, which is both what a save writes and how an
    // edit is noticed: a frame whose section differs from the loaded one is unsaved level work.
    std::string LevelFingerprint() const;
    bool IsMaterialInActiveMod(const std::string &name) const;
    bool IsMaterialFromBaseMod(const std::string &name) const;
    bool IsModelCatalogEntryInActiveMod(const EditorAssetCatalogEntry &entry) const;
    bool IsModelCatalogEntryFromBaseMod(const EditorAssetCatalogEntry &entry) const;
    bool IsPreviewPresetInActiveMod(int32_t skinIndex) const;
    bool IsPreviewPresetFromBaseMod(int32_t skinIndex) const;
    std::string FormatMaterialListLabel(const std::string &name) const;
    void TouchMaterial(const std::string &name);
    bool SaveMaterialsSession();
    bool SaveLevelLightsSession();
    bool ApplySession();
    void DiscardSession();
    void RequestExit();
    // QRE_ResetAll for a standalone session: removes the files the session saved and puts every
    // panel back to its default, without touching anything the session did not write.
    void ResetSession();
    void DrawSessionBar();
    void DrawSessionPrompt();
    void DrawResetPrompt();
    // The game's regen_prompt: a question whose "Yes" marks every model in the list as a light
    // material and writes their entries, the way QRE_RegenerateLights does.
    void DrawRegenPrompt();
    void RegenerateLightModels();
    void DrawNotification();
    void Notify(const std::string &text);
    // The Entity tab of the game editor: every light the frame uploads, map lights first, and
    // the buttons that add, duplicate and remove the ones the level does not have.
    void DrawLightList();

    // Casts a ray through the cursor the way QRE_DoPick resolves a click against the world:
    // the nearest surface polygon along the ray wins, and a light in front of that surface wins.
    void Pick();
    void UpdateHoverPick();
    void SetCursorMode(bool panelOpen);
    void HandleFlyMouseButton(const EditorEvent &event);
    void HandleFocusEvent(bool focused);
    bool HasModalOpen() const;
    EditorCameraBasis CameraBasis() const;
    void ViewBasis(float side[3], float up[3], float forward[3]) const;
    // The vertical field of view the frame is drawn with, so the picking and the overlay
    // invert exactly the projection the renderer used instead of a constant of their own.
    float FovYRadians() const;
    int32_t PickLight(const float origin[3], const float direction[3], float &outDistance,
                      bool &outCustom) const;
    void DrawSelectionOverlay();
    void DrawCustomLightGizmoAndLog(ImDrawList *drawList, const EditorCameraBasis &cam);
    // The game's viewport overlay: the crosshair a look-mode click picks from, the hint block
    // that says what the keys do, and the line a drag in flight shows.
    void DrawViewportOverlay();
    void DrawViewportMarquee(ImDrawList *draw, float width, float height) const;
    void ClearSelection();

    // Projects a world point the way ViewBasis casts a ray, so an overlay marker or a gizmo grab
    // lands on the geometry the click resolved. False when the point is behind the camera.
    bool ProjectPoint(const float point[3], float &outX, float &outY) const;

    // The axis gizmo of the selected custom light. The press freezes the axis' screen direction,
    // the way QRE_GizmoAxisScreen does, so the drag slides the light along one axis instead of
    // re-aiming it at the cursor every frame.
    bool BeginGizmoDrag(float cursorX, float cursorY);
    void UpdateGizmoDrag(float cursorX, float cursorY);
    void FinishGizmoDrag(bool kept);

    // The direction of the ray the author aims with: the centre of the viewport while the camera
    // is captured and the cursor otherwise. Shared by picking and by every drag, so a drag lands
    // where a click would.
    bool CursorDirection(float outDirection[3]) const;

    // Grabbing the light itself, the way QRE_UpdateLightDrag moves one: the light follows the
    // surface under the ray, 8 units in front of it, and hangs 128 units ahead of the camera when
    // the ray hits nothing. A press that does not travel stays a selection.
    bool BeginLightGrab();
    bool IsCursorNearProjectedLight(const EditorLight &light) const;
    bool ResolveGrabTargetLight(int32_t &outIndex, bool &outCustom) const;
    void UpdateLightGrab();
    void FinishLightGrab(bool kept);

    // One luma block copied from the panel, the way the game keeps a block clipboard; valid only
    // while the flag is set, because a default block is a legitimate clipboard entry.
    EditorEmissiveBlock m_lumaClipboard;
    bool m_lumaClipboardValid = false;
    EditorGlossBlock m_glossClipboard;
    bool m_glossClipboardValid = false;
    EditorSssBlock m_sssClipboard;
    bool m_sssClipboardValid = false;

    // The light the panel edits: the map light or the custom one, whichever was picked last.
    // Null when the selection is a surface or nothing at all.
    EditorLight *SelectedLight();
    // "light 8" for one the level carries, "custom 0" for one the editor authored.
    std::string SelectedLightLabel() const;
    // The ID the cluster light lists know the selected light by.
    uint64_t SelectedLightUniqueID() const;
    // Selects one light and makes its origin the pick point, the way a click on its gizmo does.
    void SelectLight(int32_t index, bool custom);
    size_t TotalSelectedLights() const;
    bool HasMultiLightSelection() const;
    bool IsLightSelected(int32_t index, bool custom) const;
    LightProjectionCamera MarqueeCamera() const;
    void CancelMarqueeSelection();
    void CommitMarqueeSelection();
    void SyncPrimaryLightSelection();
    void ResetMultiSelectedLightsToBaseline();

    // How a light the editor invents is placed: the game editor's Add, Duplicate and Clone
    // differ only in where the copy starts.
    enum class LightSpawn
    {
        // In front of the camera, for Add light.
        Camera,
        // One step above the light being copied, so the two are told apart at once.
        Duplicate,
        // At the pick point, aimed down the pick ray: Clone drops it where the author looks.
        Clone,
    };

    // Appends `source`, or a default light when it is null, to the custom list and selects it:
    // the game editor's Add, Clone (from a map light) and Duplicate (from a custom one).
    void AddCustomLight(const EditorLight *source, LightSpawn spawn);
    void DuplicateSelectedLight();
    void CloneSelectedLight();
    void CopySelectedLights();
    void PasteCopiedLights();
    void RemoveSelectedLights();
    void RemoveSelectedCustomLight();
    void HandleUnmodifiedEditorKey(EditorKey key);

    void Frame();
    void RenderSceneFrameContents(float deltaSeconds);
    void SubmitSceneFrame();
    void TryRequestScreenshot();
    void TryCompleteScreenshot();
    void AdvanceRenderedFrameCount();
    void Shutdown();
    QrMaterial GetCutoutBackingMaterial();
    QrMaterial GetOverlayStageMaterial(const std::string &imageName, bool clampMap,
                                       bool cleanAdditiveBlack, bool addScreenEmission = false);
    void DestroyOverlayMaterials();
    void UploadOverlayGeometry(float timeSeconds);

    void FlushPendingSceneUpdates();
    void MaintainFlyLookCapture();
    bool HandlePlayProcessEvent(const EditorEvent &event);
    void RestoreFocusFromInputEvent(const EditorEvent &event);
    void HandleEvent(const EditorEvent &event);
    void HandleMouseMoveEvent(const EditorEvent &event);
    void HandleMouseButtonEvent(const EditorEvent &event);
    void HandleViewportLeftPress(const EditorEvent &event);
    void HandleViewportLeftRelease(const EditorEvent &event);
    void HandleMouseWheelEvent(const EditorEvent &event);
    void UpdatePreviewOrbitDrag(int32_t x, int32_t y);
    void HandlePreviewOrbitMouseButton(const EditorEvent &event);
    void HandleKeyEvent(const EditorEvent &event);
    void HandleModeCtrlShortcut(EditorKey key);
    void HandleCtrlShortcut(EditorKey key);
    void HandleFocusLost();

    EditorWindow m_window;
    void *m_nativeInstance = nullptr;
    QrInstance m_instance = nullptr;
    QrMaterial m_material = QR_NO_MATERIAL;
    EditorSceneRuntime m_runtime;
    EditorScene &m_scene;
    EditorMaterialTable &m_materials;
    EditorMaterialBakes &m_materialBakes;
    EditorRenderSettings &m_settings;
    EditorSunState &m_sun;
    std::vector<EditorLight> &m_customLights;
    std::vector<EditorEmissiveLight> &m_emissiveLights;
    std::vector<EditorGeneratedLightBase> &m_generatedLightBases;
    std::vector<EditorEmitterLightOverride> &m_emitterLightOverrides;
    std::vector<QrMaterial> &m_textureMaterials;
    std::vector<EditorMaterialEmission> &m_textureEmissions;
    std::unordered_map<std::string, int32_t> &m_textureIndexByName;
    std::unordered_set<std::string> &m_missingExternalTextures;
    std::vector<uint8_t> m_palette;
    EditorAssetSource m_assets;
    EditorUi m_ui;

    bool HasLoadedScene() const { return m_sceneLoaded; }
    void RebuildAssetMounts();
    void RebuildMapCatalog();
    void RefreshMapCatalogSavedFlags();
    void RefreshDiscoveredMods();
    void UnloadSceneResources();
    bool OpenSceneNow(const std::string &path);
    void CloseSceneNow();
    bool ReloadSceneNow();
    bool OpenModNow(const std::string &modPath, bool useModYamlPaths);
    void UnloadModNow();
    void RequestSessionAction(PendingSessionAction action, const std::string &path = {},
                              bool flag = false);
    void ExecutePendingSessionAction();
    void ProcessDeferredSessionAction();
    void OpenLevelBrowserDialog();
    void OpenModBrowserDialog();
    void DrawMainMenuBar();
    void DrawFileMenu();
    void DrawLevelsInModSubmenu();
    void DrawSwitchModSubmenu();
    void DrawModeMenu();
    void DrawViewMenu();
    void DrawMenuBarStatus();
    void DrawOpenLevelDialog();
    void DrawOpenLevelModTab(std::string &chosenPath);
    void DrawOpenLevelFsTab(std::string &chosenPath);
    void DrawOpenModDialog();
    void DrawDetectedModsList(std::string &modToLoad);
    void DrawModDirectoryList();
    void DrawEmptySceneOverlay();

    void InitEditorSessionMode();
    void StartEditorMode(EditorMode mode);
    void BackToChooser();
    void DrawEditorOverlay(float deltaSeconds);
    void DrawModeChooser();
    void DrawMaterialEditorPanel();
    void DrawMaterialEditorBody(int32_t materialIndex, bool showFaceHeader);
    void DrawMaterialPbrSectionsTail(const std::string &matName, EditorMaterialParams &params,
                                     const EditorMaterialParams &base, int32_t materialIndex);
    void DrawMaterialEditorMaterialsTab();
    void DrawMaterialEditorModelsTab();
    void DrawModelTabSkinSelector();
    void DrawModelTabSurfaceSelector();
    void DrawMaterialEditorLumaBlocks(const std::string &name, EditorMaterialParams &params,
                                     int32_t materialIndex);
    void DrawMaterialEditorGlossBlocks(const std::string &name, EditorMaterialParams &params,
                                      int32_t materialIndex);
    bool DrawMaterialEditorShaderStages(const std::string &matName, EditorMaterialParams &params);
    void DrawMaterialEditorSssBlocks(const std::string &name, EditorMaterialParams &params,
                                     int32_t materialIndex);
    void DrawMaterialEditorEntitiesTab();
    void DrawMaterialEditorSystemTab();
    void DrawSystemTab();
    void ResetSystemTab();
    bool HasUnsavedSystemSettings() const;
    std::string ResolveSystemConfigPath() const;
    bool SaveSystemConfigSession();
    void ResetActiveMaterialTab();
    struct MultiLightEditTargets
    {
        std::vector<const EditorLight *> constPtrs;
        std::vector<bool> isCustomFlags;
        std::vector<EditorLight *> directLightPtrs;
        std::vector<EditorLight *> directSceneLightPtrs;
        std::vector<EditorLight *> customLightPtrs;
        std::vector<int32_t> emitterIndices;
    };

    void DrawLightEditorPanel();
    void DrawLightEditorEntityTab();
    void DrawEmitterControlsForSelected();
    void CollectMultiLightEditTargets(MultiLightEditTargets &outTargets);
    void ApplyEmitterFieldBatch(const std::vector<int32_t> &emitterIndices,
                                const std::function<void(EditorEmitterLightOverride &,
                                                         const EditorGeneratedLightBase *)> &mutator);
    void ApplyMultiEmitterOverrideBatch(const std::vector<int32_t> &emitterIndices,
                                      std::vector<EditorEmitterLightOverride> &ovs,
                                      const std::function<void(EditorEmitterLightOverride &)> &mutator);
    bool DrawMultiEmitterBasicFields(const std::vector<int32_t> &emitterIndices,
                                     std::vector<EditorEmitterLightOverride> &ovs,
                                     const MultiEmitterAggregate &agg);
    bool DrawMultiEmitterSpotAndRasterFields(const std::vector<int32_t> &emitterIndices,
                                             std::vector<EditorEmitterLightOverride> &ovs,
                                             const MultiEmitterAggregate &agg);
    bool DrawMultiLightBasicFields(const MultiLightAggregate &agg,
                                   const MultiLightEditTargets &targets);
    bool DrawMultiLightColorAndStyleFields(const MultiLightAggregate &agg,
                                           const MultiLightEditTargets &targets);
    bool DrawMultiLightSpotAndShaftFields(const MultiLightAggregate &agg,
                                          const MultiLightEditTargets &targets);
    void DrawMultiSelectedLightsInspector();
    void DrawMultiSelectedEmitterFields(const std::vector<int32_t> &emitterIndices);
    void DrawLightEditorCustomTab();
    void DrawLightEditorGlobalTab();
    void DrawPanelActionBar(bool isLightEditor);
    void DrawLightEditorViewportExtras();
    void UploadTorchLight(QrResult &lastResult);
    void BuildPreviewLight(EditorLight &outLight) const;
    void UploadPreviewLight(QrResult &lastResult);
    bool CancelActivePlacementOrDialog();
    bool HandleEditorModeKeyDown(int key);
    void PlaceLightOrSunFromPick(const float direction[3], float surfaceDistance, bool hitSurface);
    void ToggleAdditiveLightSelection(int32_t light, bool lightCustom);
    void SetSingleLightSelection(int32_t light, bool lightCustom);
    void HandleLightModePick(const float direction[3], float surfaceDistance, bool hitSurface);
    void OpenTextureBrowserForSlot(const std::string &materialName, EditorTextureSlot slot);
    void ApplyTextureBrowserSelection();
    void OpenAiTextureGeneratorDialog(int32_t materialIndex, EditorTextureSlot slot);
    void DrawAiTextureGeneratorDialog();
    bool IsAiTextureDialogOpen() const;
    bool CancelAiTextureGeneratorDialog();
    void ShutdownAiTextureGeneratorDialog();
    int32_t RegisterOrUpdateGeneratedTexture(const std::string &relPath, uint32_t width,
                                             uint32_t height, const std::vector<uint8_t> &rgba);
    void MarkMaterialsEdited();
    void MarkMaterialEdited(int32_t materialIndex);

    bool IsModelPreviewActive() const;
    bool IsModelOrbitMode() const;
    void RebuildModelCatalog();
    int32_t EnsurePreviewSkinTexture(const EditorTexture &skin);
    int32_t EnsureModelSkinMaterial(const std::string &skinName);
    bool SelectPreviewModel(int32_t catalogIndex);
    void SelectPreviewModelSkin(int32_t skinIndex);
    void SelectPreviewModelSection(int32_t sectionIndex);
    std::string ActivePreviewSectionMaterialName(const EditorPreviewModelSection &section) const;
    int32_t ActivePreviewSkinCount() const;
    const char *ActivePreviewSkinName(int32_t index) const;
    void ResetPreviewView();
    void ComputePreviewPose(float outCenter[3], float &outScale, float outRotation[3][3]) const;
    QrMaterial ResolvePreviewMaterial(const std::string &skinName, const EditorMaterialParams &params,
                                      bool forceEnvBase = false);
    void UploadPreviewModel(float timeSeconds = 0.0f);
    bool SaveSessionToDisk();
    void CancelSessionChanges();
    void RequestEditorExit();
    void DrawFilePlayMenuItem();
    bool CanPlayCurrentLevel() const;
    void RequestPlayLevel();
    bool PlayLevelNow();
    bool PollPlayProcess();
    void ApplyBaselineCapture();
    bool HasUnsavedEditorChanges() const;

    bool m_sceneLoaded = false;
    PendingSessionAction m_pendingAction = PendingSessionAction::None;
    std::string m_pendingActionPath;
    bool m_pendingActionFlag = false;
    PendingSessionAction m_deferredAction = PendingSessionAction::None;
    std::string m_deferredActionPath;
    bool m_deferredActionFlag = false;
    bool m_openLevelDialog = false;
    bool m_openModDialog = false;
    int32_t m_openLevelTab = 0;
    char m_openLevelSearch[64] = {};
    bool m_showBrushSubmodels = false;
    int32_t m_selectedCatalogMap = -1;
    std::vector<MapCatalogItem> m_mapCatalog;
    std::string m_fileBrowserDir;
    char m_fileBrowserPathInput[512] = {};
    std::string m_selectedBrowserFile;
    std::string m_modBrowserDir;
    char m_modBrowserPathInput[512] = {};
    std::string m_selectedModSubdir;
    bool m_modUseOwnYaml = true;
    bool m_multiplayerMode = false;
    std::vector<std::string> m_discoveredMods;

    bool m_choosingMode = true;
    EditorMode m_editorMode = EditorMode::Material;
    int32_t m_materialTab = 0;
    int32_t m_lightTab = 0;
    bool m_torchMode = false;
    bool m_customPlacing = false;
    int32_t m_customCloning = -1;
    bool m_placingSun = false;
    EditorTextureSlot m_textureBrowserSlot = EditorTextureSlot::Base;
    EditorMaterialParams m_materialClipboard;
    bool m_hasMaterialClipboard = false;
    EditorMaterialTable m_baselineMaterials;
    EditorRenderSettings m_baselineSettings;
    EditorSunState m_baselineSun;
    std::vector<EditorLight> m_baselineCustomLights;
    std::vector<EditorLight> m_baselineMapLights;
    std::vector<EditorEmitterLightOverride> m_baselineEmitterOverrides;
    EditorRenderSettings m_lastLightBuildSettings;
    bool m_hasLightBuildSettings = false;
    std::string m_scenePath;
    std::string m_gamePath;
    std::string m_initialGamePath;
    std::string m_gameExecutableOverride;
    std::string m_shaderFolder;
    std::string m_blueNoisePath;
    std::string m_waterNormalPath;
    std::string m_fontPath;
    EditorChildProcess m_playProcess;
    // --tab opens one panel tab on the first frame. A script cannot click a tab without knowing
    // where ImGui laid it out, so it names the tab it wants instead.
    std::string m_initialTab;
    std::string m_requestedTab;
    bool m_initialTabApplied = false;
    std::chrono::steady_clock::time_point m_startTime;
    std::chrono::steady_clock::time_point m_lastFrameTime;
    bool m_running = true;

    float m_cameraPosition[3] = { 4.0f, -6.0f, 3.0f };
    float m_cameraYaw = 1.0f;
    float m_cameraPitch = 0.0f;
    float m_cameraSpeed = 450.0f;
    bool m_looking = false;
    bool m_hasFocus = true;
    int32_t m_lastMouseX = 0;
    int32_t m_lastMouseY = 0;
    bool m_keys[size_t(EditorKey::Count)] = {};
    bool m_showPanel = true;
    uint32_t m_maxFrames = 0;
    uint32_t m_renderedFrames = 0;
    std::string m_screenshotPath;
    bool m_noPanel = false;
    bool m_screenshotRequested = false;
    bool m_screenshotWritten = false;
    uint64_t m_screenshotRequestId = 0;
    uint32_t m_screenshotWidth = 0;
    uint32_t m_screenshotHeight = 0;
    uint32_t m_screenshotRowPitch = 0;
    std::vector<uint8_t> m_screenshotPixels;

    // Left edge of the right-docked panel and bottom edge of menu bar so overlays stay inside the 3D view.
    bool m_panelVisible = false;
    float m_panelLeft = 0.0f;
    float m_menuBarBottom = 0.0f;

    // The vertical field of view is rebuilt from m_settings each frame.
    // The sun of the level sits in m_runtime.sun (referenced via m_sun).

    // The two scopes of the sky block. `m_skyGlobals` is the text of the file-wide sky a level
    // without a block inherits, kept only as the baseline the level scope compares against, and
    // `m_skyLevel` carries the keys this level overrides. `m_skyGlobal` picks which one the
    // panel edits: the level scope writes the block, the global one removes it.
    EditorLevelSky m_skyGlobals;
    EditorLevelSky m_skyLevel;
    bool m_skyGlobal = true;
    // The game config has already been read, so a scene reload keeps whatever the panel holds
    // instead of re-asserting the file over edits made since.
    bool m_gameConfigApplied = false;

    // The text the panel's own values render to, taken when the session last matched the file. A
    // key whose text still matches it was not edited and writes back the file's own spelling.
    EditorLevelSky m_skyRendered;

    // The Global tab's frame params live in m_frameParams (populated by m_runtime).
    EditorFrameParams m_frameParams;

    // One material per texture name (in m_runtime.materials); selection indices stay on EditorApp.
    int32_t m_selectedMaterial = -1;
    int32_t m_selectedLumaBlock = 0;
    int32_t m_selectedGlossBlock = 0;
    int32_t m_selectedShaderStage = -1;
    int32_t m_selectedStageAnimFrame = 0;
    int32_t m_selectedSssBlock = 0;
    int32_t m_lastInspectedMaterial = -1;

    std::vector<EditorAssetCatalogEntry> m_modelCatalog;
    int32_t m_selectedModelCatalog = -1;
    std::string m_previewModelPath;
    EditorPreviewModel m_previewModel;
    bool m_previewModelLoaded = false;
    int32_t m_previewModelSkin = 0;
    int32_t m_previewModelFrame = 0;
    int32_t m_previewModelSection = 0;
    int32_t m_previewMaterialIndex = -1;
    float m_previewYaw = -0.35f;
    float m_previewPitch = 0.12f;
    float m_previewZoom = 1.0f;
    bool m_previewModelDragging = false;
    bool m_previewLight = true;
    bool m_previewAutoRotate = false;
    EditorMaterialBakes m_previewMaterialBakes;

    // The texture browser: the request the Browse button leaves for the window, which opens on
    // the next frame. The material is named rather than indexed because the list stays clickable
    // while the browser is open, so the target is the one the button was pressed on.
    bool m_textureBrowserRequested = false;
    bool m_textureBrowserFocus = false;
    bool m_textureBrowserCatalogBuilt = false;
    std::string m_textureBrowserMaterial;
    int32_t m_textureBrowserSelection = -1;
    int32_t m_textureBrowserFolder = -1;
    char m_textureBrowserSearch[64] = {};
    std::vector<TextureBrowserItem> m_textureBrowserCatalog;
    std::vector<TextureBrowserFolder> m_textureBrowserFolders;
    std::unique_ptr<EditorAiDialogState, EditorAiDialogDeleter> m_aiDialog;
    // Set by an edit, cleared once the scene has been re-uploaded with the new values. The
    // rebuild waits for the pointer to leave the control so a drag does not rebuild per frame.
    bool m_materialsChanged = false;

    // The session. --materials names the target, or the scene's own folder is used; the target is
    // qray.materials.yaml and its previous generation is qray.backup_materials.yaml, the two names
    // the game editor saves under. --lights names qray.lights.yaml the same way, whose level
    // section is qray.backup_lights.yaml's previous generation.
    std::string m_materialsOverride;
    std::string m_lightsOverride;
    EditorSessionPaths m_sessionPaths;
    EditorMaterialsDocument m_materialsDocument;
    EditorMaterialsDocument m_baseMaterialsDocument;
    std::string m_baseMaterialsPath;
    EditorLevelDocument m_levelDocument;
    // The level section as the file holds it after the last read or save. A frame whose live fog
    // and custom lights render a different section has unsaved level work.
    std::string m_levelFingerprint;
    // Latched by the first frame that renders a different section, so the comparison runs until
    // the level is edited and not once per frame after that.
    bool m_levelTouched = false;
    // The materials the panel authored since the last Apply, which are the blocks a save
    // rewrites. Everything else in the file is left exactly as it was.
    std::vector<std::string> m_touchedMaterials;
    bool m_sessionDirty = false;
    // An exit request that found unsaved changes: 1 shows the prompt, 0 means no prompt. Escape
    // and "No" both return to editing, as QRE_RequestExit's prompt does.
    int32_t m_sessionPrompt = 0;
    // The reset question the game's tab bar opens when the open tab is clicked again: 1 shows the
    // prompt, 0 means no prompt, and Escape returns to editing like the game's own dialog.
    int32_t m_resetPrompt = 0;
    // The regenerate question: 1 shows it, 0 means none. Restores standard predefined lights from
    // the map into `m_scene.lights` and writes them to qray.lights.yaml.
    int32_t m_regenPrompt = 0;
    // The panel tab that was open last frame: a click that lands on it again is the reset
    // gesture rather than a switch to another tab.
    std::string m_activePanelTab;

    // The toast of the game editor: one line in the corner, gone a few seconds later.
    std::string m_notifyText;
    std::chrono::steady_clock::time_point m_notifyTime;
    bool m_notifyActive = false;

    float m_frameTimeMs = 0.0f;
    uint32_t m_clientWidth = 0;
    uint32_t m_clientHeight = 0;
    uint32_t m_framesSinceSceneLoad = 0;
    bool m_lightStatsLogged = false;

    // Selection and live hover state, mirroring the game editor's whole-face and light picks.
    int32_t m_selectedSurface = -1;
    int32_t m_selectedFace = -1;
    int32_t m_selectedLight = -1;
    std::vector<int32_t> m_selectedLights;
    int32_t m_hoverSurface = -1;
    int32_t m_hoverFace = -1;
    int32_t m_hoverLight = -1;
    int32_t m_hoverCustomLight = -1;
    int32_t m_selectedCustomLight = -1;
    std::vector<int32_t> m_selectedCustomLights;
    EditorLightClipboard m_lightClipboard;
    bool m_marqueePending = false;
    bool m_marqueeDragging = false;
    bool m_marqueeAdditive = false;
    bool m_pendingAdditivePick = false;
    float m_marqueeStart[2] = {};
    float m_marqueeEnd[2] = {};
    float m_pickPoint[3] = {};
    bool m_hasPickPoint = false;
    uint32_t m_pickCluster = 0;

    // Hash of the light field rectangles the panel last logged, so the log carries one line per
    // layout change - including the fields the spot checkbox adds or removes - instead of one per
    // frame. Zero means nothing has been logged yet.
    uint64_t m_loggedLightLayout = 0;
    uint64_t m_loggedLightListLayout = 0;
    // Rectangles the settings panel has already logged, keyed by control name, so a scripted
    // session can click a control without a pixel guess and the log stays one line per change.
    std::unordered_map<std::string, uint64_t> m_loggedRects;
    uint64_t m_loggedTabLayout = 0;
    uint64_t m_loggedSessionLayout = 0;
    bool m_loggedPromptLayout = false;
    // Hash of the reset dialog's rectangles, so a re-opened dialog logs again when its fitted
    // height changes with a longer path.
    uint64_t m_loggedResetLayout = 0;
    uint64_t m_loggedRegenLayout = 0;
    uint64_t m_loggedStyleLayout = 0;
    uint64_t m_loggedGizmoLayout = 0;

    // The axis gizmo drag. The light's position at the press, the axis it moves along, the mouse
    // position and the axis' screen direction are all frozen there: an axis whose screen
    // direction is re-measured per frame turns a straight drag into a curve as the light moves.
    GizmoPart m_hoverGizmoPart = GizmoPart::None;
    int32_t m_hoverGizmoAxis = -1;
    bool m_gizmoDragging = false;
    bool m_gizmoDragDir = false;
    int32_t m_gizmoAxis = 0;
    float m_gizmoOrigin[3] = {};
    float m_gizmoAxisVector[3] = {};
    float m_gizmoDirStart[3] = {};
    EditorSunState m_placingSunBackup;
    float m_gizmoStart[2] = {};
    float m_gizmoDirection[2] = {};
    float m_gizmoPixelsPerUnit = 0.0f;

    // The light grab. The cursor position is frozen at the press so a click that only selects
    // does not also fly the light to the pick point, and the origin is kept so Escape can put the
    // light back the way QRE_CancelLightDrag does.
    bool m_grabDragging = false;
    bool m_grabMoved = false;
    bool m_grabCustom = false;
    int32_t m_grabIndex = -1;
    float m_grabOrigin[3] = {};
    float m_grabStart[2] = {};
    float m_grabLoggedPosition[3] = {};
};
