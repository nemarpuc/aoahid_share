# The settings window. Dear ImGui and GLFW are fetched at pinned, hashed
# releases and linked statically, so the program needs nothing beyond the
# system's OpenGL driver.
#
# To build offline, point FETCHCONTENT_SOURCE_DIR_IMGUI / _GLFW at local
# copies of the same releases.
include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW) # extracted files get the extraction time
endif()

FetchContent_Declare(imgui
  URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9.tar.gz
  URL_HASH SHA256=af97ed649182c39314320514a672b82008ab462b9293fe23d37b30bfa5d05519
)
FetchContent_Declare(glfw
  URL https://github.com/glfw/glfw/releases/download/3.5.1/glfw-3.5.1.zip
  URL_HASH SHA256=ea79bc5feffc254c87291980c2d0bce9acebb68c4983b79f961dcd2cb8a611a0
)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF)
# Dear ImGui has no CMake project, so this only downloads its sources.
FetchContent_MakeAvailable(glfw imgui)

find_package(OpenGL REQUIRED)

# Built apart from the rest so the project's warning flags do not apply to it.
add_library(aoahid_share_imgui STATIC
  "${imgui_SOURCE_DIR}/imgui.cpp"
  "${imgui_SOURCE_DIR}/imgui_draw.cpp"
  "${imgui_SOURCE_DIR}/imgui_tables.cpp"
  "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
  "${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp"
)
target_include_directories(aoahid_share_imgui PUBLIC "${imgui_SOURCE_DIR}"
                                                     "${imgui_SOURCE_DIR}/backends")
target_link_libraries(aoahid_share_imgui PUBLIC glfw OpenGL::GL ${CMAKE_DL_LIBS})

# The UI font (Roboto Medium, Apache-2.0, shipped with Dear ImGui) is
# compiled in, so the window looks the same wherever it runs.
set(font_source "${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf")
set(font_cpp "${CMAKE_CURRENT_BINARY_DIR}/embedded_font.cpp")
add_custom_command(
  OUTPUT "${font_cpp}"
  COMMAND "${CMAKE_COMMAND}" "-DINPUT=${font_source}" "-DOUTPUT=${font_cpp}"
          -DSYMBOL=aoahid_player_font -P "${PROJECT_SOURCE_DIR}/cmake/EmbedBinary.cmake"
  DEPENDS "${font_source}" "${PROJECT_SOURCE_DIR}/cmake/EmbedBinary.cmake"
  COMMENT "Embedding Roboto-Medium.ttf"
  VERBATIM)

# The look: black on white, and the font compiled in.
add_library(aoahid_share_look STATIC look.cpp "${font_cpp}")
target_link_libraries(aoahid_share_look PUBLIC aoahid_share_imgui)
target_compile_features(aoahid_share_look PUBLIC cxx_std_20)
aoahid_share_warnings(aoahid_share_look)

add_executable(aoahid_share_gui WIN32
  main.cpp
  app.cpp
  actions.cpp
  field.cpp
  layout_view.cpp
  page.cpp
  placement.cpp
  status_view.cpp)
# aoahid_share_output is linked for its process launcher only.
target_link_libraries(aoahid_share_gui PRIVATE aoahid_share_control aoahid_share_output
                                               aoahid_share_look)
target_compile_definitions(aoahid_share_gui PRIVATE
  AOAHID_SHARE_VERSION="${PROJECT_VERSION}")
aoahid_share_warnings(aoahid_share_gui)
if(MSVC)
  target_link_options(aoahid_share_gui PRIVATE /ENTRY:mainCRTStartup)
endif()

install(TARGETS aoahid_share_gui RUNTIME DESTINATION bin)
install(FILES "${PROJECT_SOURCE_DIR}/third_party/licenses/Roboto-LICENSE.txt"
        DESTINATION share/licenses/aoahid_share)
