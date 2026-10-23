set(QR_EDITOR_SHADER_DIR "" CACHE PATH
    "Directory with compiled QuakeRay shaders copied next to the editor")
set(QR_EDITOR_TEXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../gfx/textures" CACHE PATH
    "Directory with editor runtime textures")

set(QR_EDITOR_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/imgui")

if(NOT EXISTS "${QR_EDITOR_IMGUI_DIR}/imgui.cpp")
    message(FATAL_ERROR "Dear ImGui sources are missing under ${QR_EDITOR_IMGUI_DIR}")
endif()

add_library(quakerayeditor_imgui STATIC
    "${QR_EDITOR_IMGUI_DIR}/imgui.cpp"
    "${QR_EDITOR_IMGUI_DIR}/imgui_draw.cpp"
    "${QR_EDITOR_IMGUI_DIR}/imgui_tables.cpp"
    "${QR_EDITOR_IMGUI_DIR}/imgui_widgets.cpp")
target_include_directories(quakerayeditor_imgui PUBLIC "${QR_EDITOR_IMGUI_DIR}")
target_compile_features(quakerayeditor_imgui PRIVATE cxx_std_20)
target_compile_definitions(quakerayeditor_imgui PRIVATE UNICODE _UNICODE)

add_executable(quakerayeditor WIN32 Main.cpp EditorApp.cpp EditorMaterial.cpp EditorSettings.cpp
    EditorUi.cpp Scene.cpp Assets.cpp)
target_compile_features(quakerayeditor PRIVATE cxx_std_20)
target_compile_definitions(quakerayeditor PRIVATE QR_USE_SURFACE_WIN32 UNICODE _UNICODE)
target_link_libraries(quakerayeditor PRIVATE qray::renderer quakerayeditor_imgui user32 gdi32 shell32)

set(QR_EDITOR_DEPLOY_COMMANDS
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/editor_scene.obj"
        "$<TARGET_FILE_DIR:quakerayeditor>/editor_scene.obj"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:quakerayeditor>/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${QR_EDITOR_IMGUI_DIR}/fonts/Roboto-Regular.ttf"
        "$<TARGET_FILE_DIR:quakerayeditor>/fonts/Roboto-Regular.ttf"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${QR_FFX_SDK_ROOT}/PrebuiltSignedDLL/amd_fidelityfx_vk.dll"
        "$<TARGET_FILE_DIR:quakerayeditor>/amd_fidelityfx_vk.dll")

if(QR_EDITOR_SHADER_DIR AND EXISTS "${QR_EDITOR_SHADER_DIR}")
    list(APPEND QR_EDITOR_DEPLOY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${QR_EDITOR_SHADER_DIR}"
            "$<TARGET_FILE_DIR:quakerayeditor>/shaders")
endif()

if(QR_EDITOR_TEXTURE_DIR AND EXISTS "${QR_EDITOR_TEXTURE_DIR}")
    list(APPEND QR_EDITOR_DEPLOY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:quakerayeditor>/textures"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QR_EDITOR_TEXTURE_DIR}/BlueNoise_LDR_RGBA_128.png"
            "$<TARGET_FILE_DIR:quakerayeditor>/textures/BlueNoise_LDR_RGBA_128.png")
endif()

add_custom_command(TARGET quakerayeditor POST_BUILD
    ${QR_EDITOR_DEPLOY_COMMANDS}
    VERBATIM)
install(TARGETS quakerayeditor RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
