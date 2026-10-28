cmake_minimum_required(VERSION 3.18)
project(BedrockClient C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)
FetchContent_Declare(and64 GIT_REPOSITORY https://github.com/Rprop/And64InlineHook.git GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(and64)
set_source_files_properties(${and64_SOURCE_DIR}/And64InlineHook.cpp PROPERTIES COMPILE_FLAGS "-w")

set(IMGUI ${CMAKE_SOURCE_DIR}/third_party/imgui)
add_library(BedrockClient SHARED main.cpp ${and64_SOURCE_DIR}/And64InlineHook.cpp
  ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp
  ${IMGUI}/imgui_widgets.cpp ${IMGUI}/backends/imgui_impl_opengl3.cpp)
target_include_directories(BedrockClient PRIVATE ${IMGUI} ${IMGUI}/backends ${and64_SOURCE_DIR})
target_compile_definitions(BedrockClient PRIVATE IMGUI_IMPL_OPENGL_ES3)
target_link_libraries(BedrockClient PRIVATE android log EGL GLESv3 dl)
