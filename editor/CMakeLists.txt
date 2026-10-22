add_executable(quakerayeditor WIN32 Main.cpp EditorApp.cpp Scene.cpp)
target_compile_features(quakerayeditor PRIVATE cxx_std_20)
target_compile_definitions(quakerayeditor PRIVATE QR_USE_SURFACE_WIN32 UNICODE _UNICODE)
target_link_libraries(quakerayeditor PRIVATE qray::renderer user32 gdi32 shell32)
add_custom_command(TARGET quakerayeditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/editor_scene.obj"
        "$<TARGET_FILE_DIR:quakerayeditor>/editor_scene.obj"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${QR_FFX_SDK_ROOT}/PrebuiltSignedDLL/amd_fidelityfx_vk.dll"
        "$<TARGET_FILE_DIR:quakerayeditor>/amd_fidelityfx_vk.dll"
    VERBATIM)
install(TARGETS quakerayeditor RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
