cmake_minimum_required(VERSION 3.21)
project(CG LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DIRECTXTEX_ROOT "${CMAKE_SOURCE_DIR}/packages/directxtex_desktop_win10.2025.10.28.1")
string(REPLACE "\\" "/" CONTENT_DIR_NORM "${CMAKE_SOURCE_DIR}/content")

add_executable(CG WIN32
    src/app/main.cpp
    src/app/Application.cpp
    src/platform/Window.cpp
    src/camera/Camera.cpp
    src/render/Renderer.cpp
    src/render/RenderingSystem.cpp
    src/render/GBuffer.cpp
    src/render/mesh/ObjLoader.cpp
    src/render/mesh/Model.cpp
    src/render/mesh/TextureLoader.cpp
    src/render/cull/Frustum.cpp
    src/render/cull/Octree.cpp
    src/render/shadow/CascadedShadowMaps.cpp
    src/render/particles/ParticleSystem.cpp
    src/render/post/PostProcess.cpp
)

target_include_directories(CG PRIVATE
    src/app
    src/platform
    src/camera
    src/common
    src/render
    src/render/mesh
    src/render/cull
    src/render/shadow
    src/render/particles
    src/render/post
    "${DIRECTXTEX_ROOT}/include"
)

target_link_libraries(CG PRIVATE
    d3d12
    dxgi
    dxguid
    d3dcompiler
    "$<IF:$<CONFIG:Debug>,${DIRECTXTEX_ROOT}/native/lib/x64/Debug/DirectXTex.lib,${DIRECTXTEX_ROOT}/native/lib/x64/Release/DirectXTex.lib>"
)

target_compile_definitions(CG PRIVATE
    "CONTENT_DIR=L\"${CONTENT_DIR_NORM}\""
)

if(MSVC)
    target_compile_options(CG PRIVATE /W4 /permissive-)
    target_compile_definitions(CG PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
