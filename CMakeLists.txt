# ============================================================================
# qray renderer, vendored into QuakeRay. Built as a STATIC library and
# linked into QuakeRay. The public QR_* API stays an internal interface
# (plain function calls).
# ============================================================================
cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(qray_renderer VERSION 0.1.0 LANGUAGES C CXX)
    set(QR_RENDERER_STANDALONE ON)
else()
    set(QR_RENDERER_STANDALONE OFF)
endif()

include(GNUInstallDirs)

# Resolved this early on purpose: the GPU test targets below copy the FidelityFX
# runtime DLL next to their executables, and a variable that is still unset there
# expands to an empty prefix silently - the copy is then baked as
# "/PrebuiltSignedDLL/amd_fidelityfx_vk.dll" and only fails at link time, on the
# first configure of a fresh build tree. The value is validated further down,
# next to the rest of the FidelityFX wiring.
if(WIN32)
    if(NOT DEFINED QR_FFX_SDK_ROOT OR QR_FFX_SDK_ROOT STREQUAL "")
        set(QR_FFX_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/Source/FidelityFX_SDK_1.1.4/src"
            CACHE PATH "FidelityFX SDK root (contains ffx-api/include and PrebuiltSignedDLL)")
    endif()
    file(TO_CMAKE_PATH "${QR_FFX_SDK_ROOT}" QR_FFX_SDK_ROOT)
endif()

set(renderer_CXX_STANDARD 20)

# The renderer source + public headers + shaders live in this folder.
set(renderer_PUBLIC_HEADERS
    Include/qray/qray.h
    Include/qray/host_contract.h)

set(renderer_SOURCES
    Source/qray.cpp
    Source/HostContracts.cpp
    Source/VulkanDevice.cpp
    Source/VulkanDevice_Init.cpp
    Source/Buffer.cpp
    Source/Scene.cpp
    Source/PhysicalDevice.cpp
    Source/Queues.cpp
    Source/Swapchain.cpp
    Source/GlobalUniform.cpp
    Source/CommandBufferManager.cpp
    Source/ShaderManager.cpp
    Source/VertexCollector.cpp
    Source/ASManager.cpp
    Source/VertexCollectorFilter.cpp
    Source/ASBuilder.cpp
    Source/ScratchBuffer.cpp
    Source/Utils.cpp
    Source/Common.cpp
    Source/Matrix.cpp
    Source/SpatialFrame.cpp
    Source/RasterizedDataCollector.cpp
    Source/Vma/vk_mem_alloc_imp.cpp
    Source/ImageLoader.cpp
    Source/TextureManager.cpp
    Source/MemoryAllocator.cpp
    Source/SamplerManager.cpp
    Source/TextureOverrides.cpp
    Source/TextureDescriptors.cpp
    Source/TextureUploader.cpp
    Source/VertexCollectorFilterType.cpp
    Source/Generated/ShaderCommonCFramebuf.cpp
    Source/Framebuffers.cpp
    Source/BlueNoise.cpp
    Source/Tonemapping.cpp
    Source/ClusterLightLists.cpp
    Source/LightManager.cpp
    Source/AutoBuffer.cpp
    Source/ASComponent.cpp
    Source/CubemapManager.cpp
    Source/CubemapUploader.cpp
    Source/GeomInfoManager.cpp
    Source/VertexPreprocessing.cpp
    Source/UserFunction.cpp
    Source/QrException.cpp
    Source/DLSS.cpp
    Source/HaltonSequence.cpp
    Source/DecalManager.cpp
    Source/Stb/stb_image.cpp
    Source/ImageLoaderDev.cpp
    Source/PortalList.cpp
    Source/TextureObserver.cpp
    Source/RayStats.cpp
    Source/RHI/NvrhiContext.cpp
    Source/RHI/NvrhiFrameSkeleton.cpp
    Source/RHI/NvrhiRequirements.cpp
    Source/RHI/RhiAccelStructs.cpp
    Source/RHI/RhiBloomPass.cpp
    Source/RHI/RhiCloudsPass.cpp
    Source/RHI/RhiCloudShadowBinding.cpp
    Source/RHI/RhiDecalPass.cpp
    Source/RHI/RhiDescriptors.cpp
    Source/RHI/RhiFrameContext.cpp
    Source/RHI/RhiFsrPass.cpp
    Source/RHI/RhiPipeline.cpp
    Source/RHI/RhiPostEffectPass.cpp
    Source/RHI/RhiProceduralSkyPass.cpp
    Source/RHI/RhiRasterSkyPass.cpp
    Source/RHI/RhiRasterOverlayPass.cpp
    Source/RHI/RhiRtComposePass.cpp
    Source/RHI/RhiRtDirectPass.cpp
    Source/RHI/RhiRtGodRaysPass.cpp
    Source/RHI/RhiRtIndirectPass.cpp
    Source/RHI/RhiRtPrimaryPass.cpp
    Source/RHI/RhiRtReflRefrPass.cpp
    Source/RHI/RhiShadowMapPass.cpp
    Source/RHI/RhiSkyPass.cpp
    Source/RHI/RhiResources.cpp
    Source/RHI/RhiTextureSource.cpp
    Source/RHI/RhiTextureTable.cpp
    Source/RHI/RhiUiPass.cpp
    Source/WorldLights.cpp
)

set(renderer_KTX_FOLDER Source/KTX/lib)
set(renderer_KTX_SOURCES
    "${renderer_KTX_FOLDER}/texture.c"
    "${renderer_KTX_FOLDER}/texture1.c"
    "${renderer_KTX_FOLDER}/texture2.c"
    "${renderer_KTX_FOLDER}/hashlist.c"
    "${renderer_KTX_FOLDER}/checkheader.c"
    "${renderer_KTX_FOLDER}/swap.c"
    "${renderer_KTX_FOLDER}/memstream.c"
    "${renderer_KTX_FOLDER}/filestream.c"
    "${renderer_KTX_FOLDER}/vkloader.c"
    "${renderer_KTX_FOLDER}/vk_funcs.c"
    "${renderer_KTX_FOLDER}/basisu/zstd/zstd.c"
    "${renderer_KTX_FOLDER}/dfdutils/createdfd.c"
    "${renderer_KTX_FOLDER}/dfdutils/interpretdfd.c"
    "${renderer_KTX_FOLDER}/dfdutils/queries.c"
    "${renderer_KTX_FOLDER}/dfdutils/vk2dfd.c"
)

# Win32 Vulkan surface is required by the host (SDL window).
add_compile_definitions(
    QR_USE_SURFACE_WIN32
    VK_USE_PLATFORM_WIN32_KHR
    NOMINMAX
    # Static build: the QR_* functions are plain internal calls, not exports.
    # qray.h turns QRAPI into an empty macro when QR_STATIC is defined.
    QR_STATIC
    # KTX-Software
    KHRONOS_STATIC
    LIBKTX
    QR_USE_FSR3
)

if(WIN32)
    # DLSS stays optional (off by default, matching the current qray build)
    option(QR_WITH_NVIDIA_DLSS "Build qray with Nvidia DLSS" OFF)
    if(QR_WITH_NVIDIA_DLSS)
        add_compile_definitions(QR_USE_NVIDIA_DLSS)
        if(DEFINED ENV{DLSS_SDK_PATH})
            target_include_directories(renderer PRIVATE "$ENV{DLSS_SDK_PATH}/include")
            add_compile_definitions(NV_WINDOWS)
            if(CMAKE_BUILD_TYPE STREQUAL "Debug")
                target_link_libraries(renderer PRIVATE "$ENV{DLSS_SDK_PATH}/lib/Windows_x86_64/x86_64/nvsdk_ngx_d_dbg.lib")
            else()
                target_link_libraries(renderer PRIVATE "$ENV{DLSS_SDK_PATH}/lib/Windows_x86_64/x86_64/nvsdk_ngx_d.lib")
            endif()
        else()
            message(FATAL_ERROR "QR_WITH_NVIDIA_DLSS is ON but DLSS_SDK_PATH is not set")
        endif()
    endif()
endif()

option(QR_BUILD_TESTS "Build headless renderer GPU regression tests" OFF)
option(QR_BUILD_EDITOR "Build the standalone QuakeRay editor" OFF)
if(QR_RENDERER_STANDALONE)
    if(NOT WIN32)
        message(FATAL_ERROR "qray_renderer currently supports Win32 only")
    endif()

    find_package(Vulkan REQUIRED)

    if(NOT TARGET nvrhi)
        if(NOT DEFINED QR_NVRHI_SOURCE_DIR)
            set(QR_NVRHI_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/nvrhi")
        endif()
        file(TO_CMAKE_PATH "${QR_NVRHI_SOURCE_DIR}" QR_NVRHI_SOURCE_DIR)

        if(NOT EXISTS "${QR_NVRHI_SOURCE_DIR}/CMakeLists.txt")
            message(FATAL_ERROR
                "NVRHI was not found. Set QR_NVRHI_SOURCE_DIR to the NVRHI source tree "
                "or provide an existing nvrhi target.")
        endif()

        set(QR_NVRHI_HEADER "${QR_NVRHI_SOURCE_DIR}/include/nvrhi/nvrhi.h")
        if(EXISTS "${QR_NVRHI_HEADER}")
            file(READ "${QR_NVRHI_HEADER}" QR_NVRHI_HEADER_TEXT)
            if(QR_NVRHI_HEADER_TEXT MATCHES
                "c_MaxBindingLayouts[ \t\r\n]*=[ \t\r\n]*([0-9]+)")
                if(CMAKE_MATCH_1 LESS 13)
                    message(FATAL_ERROR
                        "QuakeRaySDK requires NVRHI c_MaxBindingLayouts >= 13. "
                        "Apply cmake/nvrhi-max-binding-layouts.patch or use a compatible NVRHI fork.")
                endif()
            else()
                message(FATAL_ERROR
                    "Could not read nvrhi::c_MaxBindingLayouts from ${QR_NVRHI_HEADER}.")
            endif()
        endif()

        set(NVRHI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        set(NVRHI_INSTALL OFF CACHE BOOL "" FORCE)
        set(NVRHI_WITH_VULKAN ON CACHE BOOL "" FORCE)
        set(NVRHI_WITH_DX11 OFF CACHE BOOL "" FORCE)
        set(NVRHI_WITH_DX12 OFF CACHE BOOL "" FORCE)
        set(NVRHI_WITH_VALIDATION ON CACHE BOOL "" FORCE)
        set(NVRHI_FETCH_VULKAN_HEADERS OFF CACHE BOOL "" FORCE)
        add_subdirectory("${QR_NVRHI_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/nvrhi")
    endif()
endif()

if(QR_BUILD_TESTS)
    add_executable(qray_host_contract_tests Tests/HostContractTests.cpp Source/HostContracts.cpp)
    target_compile_features(qray_host_contract_tests PRIVATE cxx_std_20)
    target_compile_definitions(qray_host_contract_tests PRIVATE QR_STATIC QR_USE_SURFACE_WIN32 NOMINMAX)
    target_include_directories(qray_host_contract_tests PRIVATE Include)
    add_test(NAME qray_host_contract_tests COMMAND qray_host_contract_tests)
    set_tests_properties(qray_host_contract_tests PROPERTIES TIMEOUT 30)

    add_executable(qray_readback_api_tests Tests/ReadbackApiTests.cpp)
    target_compile_features(qray_readback_api_tests PRIVATE cxx_std_20)
    target_include_directories(qray_readback_api_tests PRIVATE Include)
    add_test(NAME qray_readback_api_tests COMMAND qray_readback_api_tests)
    set_tests_properties(qray_readback_api_tests PROPERTIES TIMEOUT 30)

    add_executable(qray_swapchain_policy Tests/SwapchainPolicyTests.cpp)
    target_compile_features(qray_swapchain_policy PRIVATE cxx_std_20)
    target_include_directories(qray_swapchain_policy PRIVATE Source ${Vulkan_INCLUDE_DIRS})
    add_test(NAME qray_swapchain_policy COMMAND qray_swapchain_policy)
    set_tests_properties(qray_swapchain_policy PROPERTIES TIMEOUT 30)

    add_executable(qray_spatial_contract_tests Tests/SpatialContractTests.cpp Source/SpatialFrame.cpp)
    target_compile_features(qray_spatial_contract_tests PRIVATE cxx_std_20)
    target_compile_definitions(qray_spatial_contract_tests PRIVATE QR_STATIC)
    target_include_directories(qray_spatial_contract_tests PRIVATE Include Source)
    add_test(NAME qray_spatial_contract_tests COMMAND qray_spatial_contract_tests)
    set_tests_properties(qray_spatial_contract_tests PROPERTIES TIMEOUT 30)

    add_executable(qray_worldlights_tests Tests/WorldLightsTests.cpp Source/WorldLights.cpp
        Source/QrException.cpp Source/UserFunction.cpp)
    target_compile_features(qray_worldlights_tests PRIVATE cxx_std_20)
    target_compile_definitions(qray_worldlights_tests PRIVATE QR_STATIC)
    target_include_directories(qray_worldlights_tests PRIVATE Include Source ${Vulkan_INCLUDE_DIRS})
    add_test(NAME qray_worldlights_tests COMMAND qray_worldlights_tests)
    set_tests_properties(qray_worldlights_tests PROPERTIES TIMEOUT 30)

    add_executable(qray_spatial_twin_gpu Tests/SpatialTwinGpu.cpp)
    target_compile_features(qray_spatial_twin_gpu PRIVATE cxx_std_20)
    target_compile_definitions(qray_spatial_twin_gpu PRIVATE QR_STATIC QR_USE_SURFACE_WIN32 UNICODE _UNICODE)
    target_include_directories(qray_spatial_twin_gpu PRIVATE Include Source)
    target_link_libraries(qray_spatial_twin_gpu PRIVATE renderer nvrhi nvrhi_vk user32)
    add_custom_command(TARGET qray_spatial_twin_gpu POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QR_FFX_SDK_ROOT}/PrebuiltSignedDLL/amd_fidelityfx_vk.dll"
            "$<TARGET_FILE_DIR:qray_spatial_twin_gpu>/amd_fidelityfx_vk.dll"
        VERBATIM)
    add_test(NAME qray_spatial_twin_gpu COMMAND qray_spatial_twin_gpu
        "${CMAKE_CURRENT_SOURCE_DIR}/Build" "${CMAKE_CURRENT_SOURCE_DIR}/../gfx/textures")
    set_tests_properties(qray_spatial_twin_gpu PROPERTIES TIMEOUT 180
        ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~" LABELS gpu)

    get_filename_component(QR_VULKAN_BIN "${Vulkan_GLSLC_EXECUTABLE}" DIRECTORY)
    find_program(QR_DXC NAMES dxc HINTS "${QR_VULKAN_BIN}" REQUIRED)
    set(QR_PROBE_DIR "${CMAKE_CURRENT_BINARY_DIR}/Tests")

    set(QR_VERTEX_TRANSFORM_PROBE "${QR_PROBE_DIR}/VertexTransformProbe.comp.spv")
    add_custom_command(OUTPUT "${QR_VERTEX_TRANSFORM_PROBE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${QR_PROBE_DIR}"
        COMMAND "${QR_DXC}" -spirv -fspv-target-env=vulkan1.2 -T cs_6_2 -E main
            -I "${CMAKE_CURRENT_SOURCE_DIR}/Source/Shaders"
            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/VertexTransformProbe.comp.hlsl"
            -Fo "${QR_VERTEX_TRANSFORM_PROBE}"
        DEPENDS
            Tests/VertexTransformProbe.comp.hlsl
            Source/Shaders/VertexTransform.hlsli
        VERBATIM)
    add_executable(qray_vertex_transform_gpu Tests/VertexTransformProbe.cpp
        "${QR_VERTEX_TRANSFORM_PROBE}")
    target_compile_features(qray_vertex_transform_gpu PRIVATE cxx_std_20)
    target_include_directories(qray_vertex_transform_gpu PRIVATE Source)
    target_link_libraries(qray_vertex_transform_gpu PRIVATE renderer nvrhi nvrhi_vk)
    if(WIN32)
        target_link_libraries(qray_vertex_transform_gpu PRIVATE user32)
    endif()
    add_test(NAME qray_vertex_transform_gpu COMMAND qray_vertex_transform_gpu "${QR_VERTEX_TRANSFORM_PROBE}")
    set_tests_properties(qray_vertex_transform_gpu PROPERTIES TIMEOUT 60
        ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~" LABELS gpu)

    set(QR_PROBE_DEPENDENCIES
        Tests/CloudsProbe.comp.hlsl
        Source/Shaders/SkyMotion.hlsli
        Source/Shaders/CloudLayer.hlsli
        Source/Shaders/CloudShadowWorld.hlsli
        Source/Shaders/CloudShadowMap.hlsli
        Source/Generated/ShaderCommonHLSL.hlsli)
    add_custom_command(OUTPUT "${QR_PROBE_DIR}/CloudsProbe.comp.spv" "${QR_PROBE_DIR}/CloudsPush.comp.spv"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${QR_PROBE_DIR}"
        COMMAND "${QR_DXC}" -spirv -fspv-target-env=vulkan1.3 -fvk-use-gl-layout -T cs_6_6 -E main
            -I "${CMAKE_CURRENT_SOURCE_DIR}/Source/Shaders" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/CloudsProbe.comp.hlsl"
            -Fo "${QR_PROBE_DIR}/CloudsProbe.comp.spv"
        COMMAND "${QR_DXC}" -spirv -fspv-target-env=vulkan1.3 -fvk-use-gl-layout -T cs_6_6 -E main
            -DDESC_SET_CLOUD_SHADOW=5 -DWITH_PUSH_CONSTANTS=1
            -I "${CMAKE_CURRENT_SOURCE_DIR}/Source/Shaders" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/CloudsProbe.comp.hlsl"
            -Fo "${QR_PROBE_DIR}/CloudsPush.comp.spv"
        DEPENDS ${QR_PROBE_DEPENDENCIES}
        VERBATIM)
    add_executable(qray_clouds_smoke Tests/CloudsSmoke.cpp
        "${QR_PROBE_DIR}/CloudsProbe.comp.spv" "${QR_PROBE_DIR}/CloudsPush.comp.spv")
    target_compile_features(qray_clouds_smoke PRIVATE cxx_std_20)
    target_include_directories(qray_clouds_smoke PRIVATE Source)
    target_link_libraries(qray_clouds_smoke PRIVATE renderer nvrhi nvrhi_vk)
    if(WIN32)
        target_link_libraries(qray_clouds_smoke PRIVATE user32)
    endif()
    add_test(NAME qray_clouds_gpu COMMAND qray_clouds_smoke "${CMAKE_CURRENT_SOURCE_DIR}/Build" "${QR_PROBE_DIR}")
    set_tests_properties(qray_clouds_gpu PROPERTIES TIMEOUT 300 ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~" LABELS gpu)

    add_custom_command(OUTPUT "${QR_PROBE_DIR}/LocalExposureProbe.comp.spv"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${QR_PROBE_DIR}"
        COMMAND "${QR_DXC}" -spirv -fspv-target-env=vulkan1.2 -T cs_6_2 -E main
            -I "${CMAKE_CURRENT_SOURCE_DIR}/Source/Shaders"
            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/LocalExposureProbe.comp.hlsl"
            -Fo "${QR_PROBE_DIR}/LocalExposureProbe.comp.spv"
        DEPENDS Tests/LocalExposureProbe.comp.hlsl Source/Shaders/LocalExposure.hlsli
        VERBATIM)
    add_custom_command(OUTPUT "${QR_PROBE_DIR}/ColorCompositingProbe.comp.spv"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${QR_PROBE_DIR}"
        COMMAND "${QR_DXC}" -spirv -fspv-target-env=vulkan1.2 -T cs_6_2 -E main
            -I "${CMAKE_CURRENT_SOURCE_DIR}/Source/Shaders"
            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/ColorCompositingProbe.comp.hlsl"
            -Fo "${QR_PROBE_DIR}/ColorCompositingProbe.comp.spv"
        DEPENDS Tests/ColorCompositingProbe.comp.hlsl Source/Shaders/ColorCompositing.hlsli
        VERBATIM)
    add_custom_command(OUTPUT "${QR_PROBE_DIR}/NearDofProbe.comp.spv"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${QR_PROBE_DIR}"
        COMMAND "${QR_DXC}" -spirv -fspv-target-env=vulkan1.2 -T cs_6_2 -E main
            -I "${CMAKE_CURRENT_SOURCE_DIR}/Source/Shaders"
            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/NearDofProbe.comp.hlsl"
            -Fo "${QR_PROBE_DIR}/NearDofProbe.comp.spv"
        DEPENDS Tests/NearDofProbe.comp.hlsl Source/Shaders/NearDof.hlsli
            Source/Generated/ShaderCommonHLSL.hlsli
        VERBATIM)
    add_executable(qray_posteffects_smoke Tests/PostEffectsSmoke.cpp
        "${QR_PROBE_DIR}/LocalExposureProbe.comp.spv" "${QR_PROBE_DIR}/ColorCompositingProbe.comp.spv"
        "${QR_PROBE_DIR}/NearDofProbe.comp.spv")
    target_compile_features(qray_posteffects_smoke PRIVATE cxx_std_20)
    target_include_directories(qray_posteffects_smoke PRIVATE Source)
    target_link_libraries(qray_posteffects_smoke PRIVATE renderer nvrhi nvrhi_vk)
    if(WIN32)
        target_link_libraries(qray_posteffects_smoke PRIVATE user32)
    endif()
    add_test(NAME qray_posteffects_gpu COMMAND qray_posteffects_smoke "${CMAKE_CURRENT_SOURCE_DIR}/Build" "${QR_PROBE_DIR}")
    set_tests_properties(qray_posteffects_gpu PROPERTIES TIMEOUT 120 ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~" LABELS gpu)
    set(QR_DTAL_PROBE "${QR_PROBE_DIR}/DtalPerf.comp.spv")
    add_custom_command(OUTPUT "${QR_DTAL_PROBE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${QR_PROBE_DIR}"
        COMMAND "${QR_DXC}" -spirv -fspv-target-env=vulkan1.3 -fvk-use-gl-layout -T cs_6_6 -E main
            -I "${CMAKE_CURRENT_SOURCE_DIR}/Source/Shaders" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/DtalPerf.comp.hlsl"
            -Fo "${QR_DTAL_PROBE}"
        DEPENDS
            Tests/DtalPerf.comp.hlsl
            Source/Shaders/Light.hlsli
            Source/Shaders/Random.hlsli
            Source/Shaders/ShaderCommonHLSLFunc.hlsli
            Source/Shaders/ShaderCommonHLSL.hlsli
            Source/Shaders/Structs.hlsli
            Source/Generated/ShaderCommonHLSL.hlsli
        VERBATIM)
    add_executable(qray_dtal_perf Tests/DtalPerf.cpp "${QR_DTAL_PROBE}")
    target_compile_features(qray_dtal_perf PRIVATE cxx_std_20)
    target_include_directories(qray_dtal_perf PRIVATE Source)
    target_link_libraries(qray_dtal_perf PRIVATE renderer nvrhi nvrhi_vk)
    if(WIN32)
        target_link_libraries(qray_dtal_perf PRIVATE user32)
    endif()
    add_test(NAME qray_dtal_perf COMMAND qray_dtal_perf "${QR_DTAL_PROBE}" "${QR_PROBE_DIR}/DtalPerf.baseline.txt")
    set_tests_properties(qray_dtal_perf PROPERTIES TIMEOUT 300 ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~" LABELS gpu)

endif()

add_library(qray_alias OBJECT Source/rt_alias.c)
target_compile_features(qray_alias PRIVATE c_std_11)

add_library(renderer STATIC
    ${renderer_SOURCES}
    ${renderer_KTX_SOURCES}
    ${renderer_PUBLIC_HEADERS}
    $<TARGET_OBJECTS:qray_alias>
)

add_library(qray::renderer ALIAS renderer)

target_compile_features(renderer PRIVATE cxx_std_20)

# Keep the vendored renderer warnings under control (the host build uses /WX).
if(MSVC)
    target_compile_options(renderer PRIVATE /W3 /EHsc /WX- /MP)
    target_compile_options(renderer PRIVATE /wd4244 /wd4267 /wd4018 /wd4305 /wd4996 /wd4100 /wd4505 /wd5287 /wd4005)
else()
    target_compile_options(renderer PRIVATE -w)
endif()

# KTX includes
target_include_directories(renderer PRIVATE
    "Source/KTX/include"
    "Source/KTX/other_include"
    "Source/KTX/lib/basisu/zstd"
)

# Public include dir (qray/qray.h) — needed by the host and by qray sources
target_include_directories(renderer PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# qray.h selects the surface declarations and the static-library calling
# convention from these definitions. They must travel with the exported target;
# directory-scope definitions do not reach a host in another CMake directory.
target_compile_definitions(renderer
    INTERFACE
        QR_STATIC
        QR_USE_SURFACE_WIN32
        $<$<COMPILE_LANGUAGE:CXX>:NOMINMAX>
)

# Vulkan (found in the parent scope or by the standalone package setup)
target_link_libraries(renderer PUBLIC Vulkan::Vulkan)

# RHI layer: NVRHI built by the root CMakeLists (target nvrhi_vk is the static
# Vulkan backend, nvrhi holds the shared interface and the validation layer).
target_link_libraries(renderer PRIVATE
    $<BUILD_INTERFACE:nvrhi_vk>
    $<BUILD_INTERFACE:nvrhi>
)

if(WIN32)
    set(QR_FFX_INCLUDE_DIR "${QR_FFX_SDK_ROOT}/ffx-api/include")
    set(QR_FFX_VK_LIBRARY "${QR_FFX_SDK_ROOT}/PrebuiltSignedDLL/amd_fidelityfx_vk.lib")

    if(NOT EXISTS "${QR_FFX_INCLUDE_DIR}/ffx_api/ffx_api.h" OR
       NOT EXISTS "${QR_FFX_VK_LIBRARY}")
        message(FATAL_ERROR
            "FidelityFX SDK was not found at QR_FFX_SDK_ROOT=${QR_FFX_SDK_ROOT}. "
            "Install the AMD FidelityFX SDK and set QR_FFX_SDK_ROOT, or provide "
            "the ffx-api headers and amd_fidelityfx_vk.lib.")
    endif()

    add_library(qray_ffx INTERFACE)
    target_include_directories(qray_ffx INTERFACE
        $<BUILD_INTERFACE:${QR_FFX_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/ffx>
    )
    target_link_libraries(qray_ffx INTERFACE
        $<BUILD_INTERFACE:${QR_FFX_VK_LIBRARY}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_LIBDIR}/amd_fidelityfx_vk.lib>
    )

    target_sources(renderer PRIVATE "Source/FSR.cpp")
    target_link_libraries(renderer PRIVATE $<BUILD_INTERFACE:qray_ffx>)
endif()

include(CMakePackageConfigHelpers)

set(QRAY_RENDERER_CONFIG_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/qray_renderer")
set(QRAY_RENDERER_VERSION "0.1.0")

if(WIN32)
    install(TARGETS renderer
        EXPORT qray_rendererTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(FILES Include/qray/qray.h Include/qray/host_contract.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/qray)
    install(DIRECTORY "${QR_FFX_SDK_ROOT}/ffx-api/include/ffx_api"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ffx)
    install(FILES "${QR_FFX_SDK_ROOT}/PrebuiltSignedDLL/amd_fidelityfx_vk.lib"
        DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES "${QR_FFX_SDK_ROOT}/PrebuiltSignedDLL/amd_fidelityfx_vk.dll"
        DESTINATION ${CMAKE_INSTALL_BINDIR})
else()
    install(TARGETS renderer
        EXPORT qray_rendererTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(FILES Include/qray/qray.h Include/qray/host_contract.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/qray)
endif()

install(FILES
    docs/api.md
    docs/host-driver-contract.md
    docs/host-contract.md
    docs/sky-and-atmosphere.md
    docs/sun-and-god-rays.md
    docs/lights-and-sampling.md
    docs/gi-and-denoiser.md
    docs/host-feature-matrix.md
    docs/spatial-contract.md
    docs/embedding.md
    cmake/nvrhi-max-binding-layouts.patch
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
)

set(QRAY_FFX_INCLUDE_INSTALL_DIR "${CMAKE_INSTALL_INCLUDEDIR}/ffx")
set(QRAY_FFX_LIBRARY_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}")

install(EXPORT qray_rendererTargets
    FILE qray_rendererTargets.cmake
    NAMESPACE qray::
    DESTINATION ${QRAY_RENDERER_CONFIG_INSTALL_DIR}
)

configure_package_config_file(
    cmake/qray_rendererConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/qray_rendererConfig.cmake"
    INSTALL_DESTINATION ${QRAY_RENDERER_CONFIG_INSTALL_DIR}
    PATH_VARS QRAY_FFX_INCLUDE_INSTALL_DIR QRAY_FFX_LIBRARY_INSTALL_DIR
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/qray_rendererConfigVersion.cmake"
    VERSION ${QRAY_RENDERER_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/qray_rendererConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/qray_rendererConfigVersion.cmake"
    DESTINATION ${QRAY_RENDERER_CONFIG_INSTALL_DIR}
)

if(QR_BUILD_EDITOR)
    add_subdirectory(editor)
endif()
