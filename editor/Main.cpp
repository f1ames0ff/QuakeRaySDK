#include "EditorApp.h"

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand)
{
    EditorApp app(instance);
    return app.Run(showCommand);
}
