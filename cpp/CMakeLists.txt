cmake_minimum_required(VERSION 3.16)
project(coomerfans_downloader CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.5 GIT_SHALLOW TRUE)
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(imgui json)

find_package(OpenGL REQUIRED)
find_package(X11 REQUIRED)
find_package(Threads REQUIRED)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC ${CMAKE_DL_LIBS})

add_executable(coomerfans-gui
  src/main.cpp src/platform_x11.cpp src/engine.cpp src/http.cpp
  src/scraper.cpp src/html.cpp src/text.cpp)
target_compile_options(coomerfans-gui PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_include_directories(coomerfans-gui PRIVATE src ${X11_INCLUDE_DIR})
target_link_libraries(coomerfans-gui PRIVATE imgui nlohmann_json::nlohmann_json
  OpenGL::GL ${X11_LIBRARIES} Threads::Threads)
