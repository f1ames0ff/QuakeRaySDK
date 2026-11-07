#pragma once

#include <windows.h>

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

#include <qray/qray.h>

#include "Assets.h"
#include "EditorMaterial.h"
#include "EditorSession.h"
#include "EditorSettings.h"
#include "EditorUi.h"
#include "Scene.h"

class EditorApp
{
public:
    explicit EditorApp(HINSTANCE instance);
    ~EditorApp();

    EditorApp(const EditorApp &) = delete;
    EditorApp &operator=(const EditorApp &) = delete;

    int Run(int showCommand);

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
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
    // The surface's own material, replaced by the one texture_base names where it resolves, or by
    // a bake of the parameters that only a packed material texture can carry.
    QrMaterial SurfaceMaterialWithOverride(const EditorSurface &surface,
                                           const EditorMaterialParams &params);
    static QrGeometryPassThroughType PassThroughTypeForSurface(EditorSurfaceKind kind);
    void ResetCameraToScene();
    void UpdateCamera(float deltaSeconds);
    void UploadSun();
    void SunDirection(float direction[3]) const;
    void UploadLights();
    // Uploads one light under the ID the light lists know it by: a spot when its cone is on,
    // a sphere otherwise, which is the branch RT_LIGHT_Emit takes.
    void UploadLight(const EditorLight &light, uint64_t uniqueID, QrResult &lastResult);
    void UploadClusterLightSources();
    // Reports whether one light is listed by the cluster it resolved into.
    void LogLightCluster(const EditorLight &light, uint64_t uniqueID, const char *label);
    void DrawPanel(float deltaSeconds);
    void DrawGlobalSettings();
    void DrawMaterials();
    // The game editor's texture browser: the scene's own images as a searchable grid of
    // previews, opened from the Base row of a material and writing its texture_base.
    void DrawTextureBrowser();
    // The session of the game editor: what the panel authored since the last Apply, the file it
    // writes and the prompt an exit with unsaved changes opens.
    void LoadMaterialsSession();
    // The level scope of the session: the `sky:`, `fog:` and `lights:` blocks of one level in
    // qray.lights.yaml, read into the sky controls, the fog settings and the custom lights, and
    // written back only for the blocks the live state no longer matches.
    void LoadLightsSession();
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
    EditorLevelLight LevelLight(const EditorLight &light) const;
    std::vector<EditorLevelLight> LevelLights() const;
    // Renders the section the live state stands for, which is both what a save writes and how an
    // edit is noticed: a frame whose section differs from the loaded one is unsaved level work.
    std::string LevelFingerprint() const;
    void TouchMaterial(const std::string &name);
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
    // the nearest surface along the ray wins, and a light in front of that surface wins over it.
    void Pick();
    void ViewBasis(float side[3], float up[3], float forward[3]) const;
    int32_t PickSurface(const float origin[3], const float direction[3], int32_t &outTriangle,
                        float &outDistance) const;
    int32_t PickLight(const float origin[3], const float direction[3], float &outDistance,
                      bool &outCustom) const;
    void DrawSelectionOverlay();
    // The game's viewport overlay: the crosshair a look-mode click picks from, the hint block
    // that says what the keys do, and the line a drag in flight shows.
    void DrawViewportOverlay();
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
    void UpdateLightGrab();
    void FinishLightGrab(bool kept);

    // One luma block copied from the panel, the way the game keeps a block clipboard; valid only
    // while the flag is set, because a default block is a legitimate clipboard entry.
    EditorEmissiveBlock m_lumaClipboard;
    bool m_lumaClipboardValid = false;

    // The light the panel edits: the map light or the custom one, whichever was picked last.
    // Null when the selection is a surface or nothing at all.
    EditorLight *SelectedLight();
    // "light 8" for one the level carries, "custom 0" for one the editor authored.
    std::string SelectedLightLabel() const;
    // The ID the cluster light lists know the selected light by.
    uint64_t SelectedLightUniqueID() const;
    // Selects one light and makes its origin the pick point, the way a click on its gizmo does.
    void SelectLight(int32_t index, bool custom);

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
    void RemoveSelectedCustomLight();

    void Frame();
    void Shutdown();

    LRESULT HandleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam);

    HWND m_window = nullptr;
    HINSTANCE m_instanceHandle = nullptr;
    QrInstance m_instance = nullptr;
    QrMaterial m_material = QR_NO_MATERIAL;
    std::vector<QrMaterial> m_textureMaterials;
    // The materials the authored keys that need a packed texture set produce, kept alive for as
    // long as a geometry still points at them.
    EditorMaterialBakes m_materialBakes;
    // Lowercased scene texture name to its index, the lookup a texture_base override and the
    // browser both need.
    std::unordered_map<std::string, int32_t> m_textureIndexByName;
    std::vector<uint8_t> m_palette;
    EditorAssetSource m_assets;
    EditorUi m_ui;
    EditorScene m_scene;
    std::string m_scenePath;
    std::string m_gamePath;
    std::string m_shaderFolder;
    std::string m_blueNoisePath;
    std::string m_fontPath;
    // --tab opens one panel tab on the first frame. A script cannot click a tab without knowing
    // where ImGui laid it out, so it names the tab it wants instead.
    std::string m_initialTab;
    bool m_initialTabApplied = false;
    std::chrono::steady_clock::time_point m_startTime;
    std::chrono::steady_clock::time_point m_lastFrameTime;
    bool m_running = true;

    float m_cameraPosition[3] = { 4.0f, -6.0f, 3.0f };
    float m_cameraYaw = 1.0f;
    float m_cameraPitch = 0.0f;
    float m_cameraSpeed = 256.0f;
    float m_cameraSensitivity = 0.0032f;
    bool m_looking = false;
    POINT m_lastMouse = {};
    bool m_keys[256] = {};
    bool m_showPanel = true;

    // Where the panel's right edge landed this frame, so the overlay can put its hint block beside
    // the panel instead of under it. Cleared when the panel is closed, which is when the block
    // takes the window corner the game draws it in.
    bool m_panelVisible = false;
    float m_panelRight = 0.0f;

    // The sun of the level: rt_physical_sun, rt_sky_sun, rt_sky_sun_size and the two angles. Off
    // uploads no directional light, which is what the game does while rt_physical_sun is 0.
    EditorSunState m_sun;

    // The two scopes of the sky block. `m_skyGlobals` is the text of the file-wide sky a level
    // without a block inherits, kept only as the baseline the level scope compares against, and
    // `m_skyLevel` carries the keys this level overrides. `m_skyGlobal` picks which one the
    // panel edits: the level scope writes the block, the global one removes it.
    EditorLevelSky m_skyGlobals;
    EditorLevelSky m_skyLevel;
    bool m_skyGlobal = true;

    // The text the panel's own values render to, taken when the session last matched the file. A
    // key whose text still matches it was not edited and writes back the file's own spelling.
    EditorLevelSky m_skyRendered;

    // The Global tab of the game editor. The frame params derived from it are
    // rebuilt every frame and only live for the duration of the qrDrawFrame call.
    EditorRenderSettings m_settings;
    EditorFrameParams m_frameParams;

    // One material per texture name; edited from the Materials tab and written into every
    // surface upload that uses it, the way the game's rt_material_t is keyed.
    EditorMaterialTable m_materials;
    int32_t m_selectedMaterial = -1;
    // The texture browser: the request the Browse button leaves for the window, which opens on
    // the next frame. The material is named rather than indexed because the list stays clickable
    // while the browser is open, so the target is the one the button was pressed on.
    bool m_textureBrowserRequested = false;
    bool m_textureBrowserFocus = false;
    std::string m_textureBrowserMaterial;
    int32_t m_textureBrowserSelection = -1;
    char m_textureBrowserSearch[64] = {};
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
    // The regenerate question: 1 shows it, 0 means none. `m_regenModels` is the list it asks
    // about -- the game reads rt_level_light_models, and this editor has no cvars, so the dialog
    // carries the same default list and lets the author state the models it must mark.
    int32_t m_regenPrompt = 0;
    char m_regenModels[256] = "flame.mdl flame2.mdl lavaball.mdl proj_balllava.mdl";
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
    // Bumped every time a new world cluster table reaches the renderer, so it can tell a new
    // map apart from a re-upload of the same one.
    uint32_t m_clusterGeneration = 0;

    // Selection state, mirroring the game editor's surface and light picks. m_selectedTriangle
    // is an index into the selected surface's own index list, not into the scene's.
    int32_t m_selectedSurface = -1;
    int32_t m_selectedTriangle = -1;
    int32_t m_selectedLight = -1;
    // Lights the level does not have, uploaded after the map's own and indexed from their own
    // list: RT_CustomLights keeps them in a file of their own, so a map change cannot lose them
    // and a map light cannot be removed from the scene by an editor action.
    std::vector<EditorLight> m_customLights;
    int32_t m_selectedCustomLight = -1;
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
    bool m_gizmoDragging = false;
    int32_t m_gizmoAxis = 0;
    float m_gizmoOrigin[3] = {};
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
