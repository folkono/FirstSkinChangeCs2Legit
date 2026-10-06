cmake_minimum_required(VERSION 3.20)
project(vmoverlay CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(IMGUI third_party/imgui)
add_executable(vmoverlay WIN32
  src/app.cpp src/render.cpp src/capture.cpp src/mask_ai.cpp
  ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
  ${IMGUI}/backends/imgui_impl_win32.cpp ${IMGUI}/backends/imgui_impl_dx11.cpp)
target_include_directories(vmoverlay PRIVATE third_party ${IMGUI})
target_compile_definitions(vmoverlay PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(vmoverlay PRIVATE /utf-8 /W3 /MP)
target_link_libraries(vmoverlay PRIVATE d3d11 dxgi dcomp d3dcompiler dwmapi windowscodecs ole32 user32 cabinet)
target_link_libraries(vmoverlay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/onnxruntime.lib)
foreach(runtime onnxruntime.dll onnxruntime_providers_shared.dll DirectML.dll)
  add_custom_command(TARGET vmoverlay POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/${runtime} $<TARGET_FILE_DIR:vmoverlay>)
endforeach()

