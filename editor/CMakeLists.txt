set(QR_EDITOR_SHADER_DIR "" CACHE PATH
    "Directory with compiled QuakeRay shaders copied next to the editor")
set(QR_EDITOR_TEXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../gfx/textures" CACHE PATH
    "Directory with editor runtime textures")
set(QR_EDITOR_ADAPTER_SOURCES "" CACHE STRING
    "Optional external format adapter source files compiled into quakerayeditor")
set(QR_EDITOR_ADAPTER_INCLUDE_DIRS "" CACHE STRING
    "Optional include directories for external format adapter sources")

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
if(WIN32)
    target_compile_definitions(quakerayeditor_imgui PRIVATE UNICODE _UNICODE)
endif()

set(QR_EDITOR_COMMON_SOURCES
    Main.cpp
    EditorApp.cpp
    EditorDtalPipeline.cpp
    EditorGui.cpp
    EditorOverlayPipeline.cpp
    EditorPanels.cpp
    EditorMaterial.cpp
    EditorSceneRuntime.cpp
    EditorSettings.cpp
    EditorSession.cpp
    EditorLightSelection.cpp
    EditorLightSelection.h
    EditorUi.cpp
    EditorViewport.cpp
    EditorSpatial.cpp
    Scene.cpp
    Assets.cpp)

if(WIN32)
    add_executable(quakerayeditor WIN32 ${QR_EDITOR_COMMON_SOURCES} EditorPlatformWin32.cpp)
    target_compile_definitions(quakerayeditor PRIVATE QR_USE_SURFACE_WIN32 UNICODE _UNICODE)
    target_link_libraries(quakerayeditor PRIVATE qray::renderer quakerayeditor_imgui user32 gdi32 shell32)
else()
    find_package(X11 REQUIRED)
    find_package(Threads REQUIRED)
    add_executable(quakerayeditor ${QR_EDITOR_COMMON_SOURCES} EditorPlatformX11.cpp)
    target_compile_definitions(quakerayeditor PRIVATE QR_USE_SURFACE_XLIB)
    target_link_libraries(quakerayeditor PRIVATE qray::renderer quakerayeditor_imgui X11::X11 Threads::Threads ${CMAKE_DL_LIBS})
    set_target_properties(quakerayeditor PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN")
endif()

if(QR_EDITOR_ADAPTER_SOURCES)
    target_sources(quakerayeditor PRIVATE ${QR_EDITOR_ADAPTER_SOURCES})
endif()

target_compile_features(quakerayeditor PRIVATE cxx_std_20)
target_include_directories(quakerayeditor PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Source/Stb")
if(QR_EDITOR_ADAPTER_INCLUDE_DIRS)
    target_include_directories(quakerayeditor PRIVATE ${QR_EDITOR_ADAPTER_INCLUDE_DIRS})
endif()

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
        "${QR_EDITOR_IMGUI_DIR}/fonts/fa-solid-900.ttf"
        "$<TARGET_FILE_DIR:quakerayeditor>/fonts/fa-solid-900.ttf")

if(WIN32)
    list(APPEND QR_EDITOR_DEPLOY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QR_FFX_VK_RUNTIME_DLL}"
            "$<TARGET_FILE_DIR:quakerayeditor>/amd_fidelityfx_vk.dll")
else()
    list(APPEND QR_EDITOR_DEPLOY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QR_FFX_VK_SO}"
            "$<TARGET_FILE_DIR:quakerayeditor>/libamd_fidelityfx_vk.so")
endif()

if(QR_EDITOR_SHADER_DIR AND EXISTS "${QR_EDITOR_SHADER_DIR}")
    list(APPEND QR_EDITOR_DEPLOY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${QR_EDITOR_SHADER_DIR}"
            "$<TARGET_FILE_DIR:quakerayeditor>/shaders")
endif()

if(QR_BUILD_TESTS)
    add_executable(qray_editor_multiselect_tests
        EditorMultiSelectTests.cpp
        EditorLightSelection.cpp
        EditorGui.cpp
        EditorSession.cpp
        EditorMaterial.cpp
        EditorUi.cpp
        EditorViewport.cpp
        Scene.cpp
        Assets.cpp
        "${CMAKE_CURRENT_SOURCE_DIR}/../Source/Stb/stb_image.cpp")
    target_compile_features(qray_editor_multiselect_tests PRIVATE cxx_std_20)
    target_compile_definitions(qray_editor_multiselect_tests PRIVATE QR_STATIC)
    target_include_directories(qray_editor_multiselect_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Source/Stb")
    target_link_libraries(qray_editor_multiselect_tests PRIVATE quakerayeditor_imgui)
    add_test(NAME qray_editor_multiselect_tests COMMAND qray_editor_multiselect_tests)
endif()

set(QR_EDITOR_BLUE_NOISE_FILE "")
if(QR_EDITOR_TEXTURE_DIR AND EXISTS "${QR_EDITOR_TEXTURE_DIR}/BlueNoise_LDR_RGBA_128.png")
    set(QR_EDITOR_BLUE_NOISE_FILE "${QR_EDITOR_TEXTURE_DIR}/BlueNoise_LDR_RGBA_128.png")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../Assets/textures/BlueNoise_LDR_RGBA_128.png")
    set(QR_EDITOR_BLUE_NOISE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../Assets/textures/BlueNoise_LDR_RGBA_128.png")
endif()

set(QR_EDITOR_WATER_NORMAL_FILE "")
if(QR_EDITOR_TEXTURE_DIR AND EXISTS "${QR_EDITOR_TEXTURE_DIR}/WaterNormal_n.png")
    set(QR_EDITOR_WATER_NORMAL_FILE "${QR_EDITOR_TEXTURE_DIR}/WaterNormal_n.png")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../Assets/textures/WaterNormal_n.png")
    set(QR_EDITOR_WATER_NORMAL_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../Assets/textures/WaterNormal_n.png")
endif()

if(QR_EDITOR_BLUE_NOISE_FILE OR QR_EDITOR_WATER_NORMAL_FILE)
    list(APPEND QR_EDITOR_DEPLOY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:quakerayeditor>/textures")
endif()
if(QR_EDITOR_BLUE_NOISE_FILE)
    list(APPEND QR_EDITOR_DEPLOY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QR_EDITOR_BLUE_NOISE_FILE}"
            "$<TARGET_FILE_DIR:quakerayeditor>/textures/BlueNoise_LDR_RGBA_128.png")
endif()
if(QR_EDITOR_WATER_NORMAL_FILE)
    list(APPEND QR_EDITOR_DEPLOY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QR_EDITOR_WATER_NORMAL_FILE}"
            "$<TARGET_FILE_DIR:quakerayeditor>/textures/WaterNormal_n.png")
endif()

add_custom_command(TARGET quakerayeditor POST_BUILD
    ${QR_EDITOR_DEPLOY_COMMANDS}
    VERBATIM)
install(TARGETS quakerayeditor RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
