#pragma once

#include <windows.h>

#include <chrono>
#include <string>

#include <qray/qray.h>

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
    bool LoadScene();
    bool UploadScene();
    void Frame();
    void Shutdown();

    HWND m_window = nullptr;
    HINSTANCE m_instanceHandle = nullptr;
    QrInstance m_instance = nullptr;
    QrMaterial m_material = QR_NO_MATERIAL;
    EditorScene m_scene;
    std::string m_scenePath;
    std::string m_shaderFolder;
    std::string m_blueNoisePath;
    std::chrono::steady_clock::time_point m_startTime;
    bool m_running = true;
};
