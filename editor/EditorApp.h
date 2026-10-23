#pragma once

#include <windows.h>

#include <chrono>
#include <string>
#include <vector>

#include <qray/qray.h>

#include "Assets.h"
#include "EditorMaterial.h"
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

    float m_sunIntensity = 8.0f;
    float m_sunAzimuth = 32.0f;
    float m_sunElevation = 45.0f;
    float m_sunAngularDiameter = 0.5f;

    // The Global tab of the game editor. The frame params derived from it are
    // rebuilt every frame and only live for the duration of the qrDrawFrame call.
    EditorRenderSettings m_settings;
    EditorFrameParams m_frameParams;

    // One material per texture name; edited from the Materials tab and written into every
    // surface upload that uses it, the way the game's rt_material_t is keyed.
    EditorMaterialTable m_materials;
    int32_t m_selectedMaterial = -1;
    // Set by an edit, cleared once the scene has been re-uploaded with the new values. The
    // rebuild waits for the pointer to leave the control so a drag does not rebuild per frame.
    bool m_materialsChanged = false;

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
    uint64_t m_loggedTabLayout = 0;
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
