#pragma once

#include <windows.h>

#include <chrono>
#include <string>
#include <vector>

#include <qray/qray.h>

#include "Assets.h"
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
    bool UploadWorldClusters();
    QrMaterial SurfaceMaterial(const EditorSurface &surface) const;
    static QrGeometryPassThroughType PassThroughTypeForSurface(EditorSurfaceKind kind);
    void ResetCameraToScene();
    void UpdateCamera(float deltaSeconds);
    void UploadSun();
    void SunDirection(float direction[3]) const;
    void UploadLights();
    void UploadClusterLightSources();
    void DrawPanel(float deltaSeconds);
    void DrawGlobalSettings();

    // Casts a ray through the cursor the way QRE_DoPick resolves a click against the world:
    // the nearest surface along the ray wins, and a light in front of that surface wins over it.
    void Pick();
    void ViewBasis(float side[3], float up[3], float forward[3]) const;
    int32_t PickSurface(const float origin[3], const float direction[3], int32_t &outTriangle,
                        float &outDistance) const;
    int32_t PickLight(const float origin[3], const float direction[3], float &outDistance) const;
    void DrawSelectionOverlay();
    void ClearSelection();

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
    float m_pickPoint[3] = {};
    bool m_hasPickPoint = false;
    uint32_t m_pickCluster = 0;
};
