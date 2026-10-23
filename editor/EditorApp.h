#pragma once

#include <windows.h>

#include <chrono>
#include <string>

#include <qray/qray.h>

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
    bool CreateUi();
    bool LoadScene();
    bool UploadScene();
    void ResetCameraToScene();
    void UpdateCamera(float deltaSeconds);
    void UploadSun();
    void DrawPanel(float deltaSeconds);
    void Frame();
    void Shutdown();

    LRESULT HandleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam);

    HWND m_window = nullptr;
    HINSTANCE m_instanceHandle = nullptr;
    QrInstance m_instance = nullptr;
    QrMaterial m_material = QR_NO_MATERIAL;
    EditorUi m_ui;
    EditorScene m_scene;
    std::string m_scenePath;
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

    float m_frameTimeMs = 0.0f;
    uint32_t m_clientWidth = 0;
    uint32_t m_clientHeight = 0;
};
