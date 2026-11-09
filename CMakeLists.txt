cmake_minimum_required(VERSION 3.20)

project(Chess3D VERSION 1.0.0 DESCRIPTION "3D Chess" LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Debug, Release, RelWithDebInfo or MinSizeRel" FORCE)
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2>=2.0.18)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng)
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)

# The shaders and the icon are built into the executable.
file(GLOB SHADERS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/shaders/*.glsl ${CMAKE_SOURCE_DIR}/shaders/*.vert
     ${CMAKE_SOURCE_DIR}/shaders/*.frag ${CMAKE_SOURCE_DIR}/shaders/*.comp)
include(cmake/Embed.cmake)
embed_files(${CMAKE_BINARY_DIR}/Embedded.cpp ${SHADERS} ${CMAKE_SOURCE_DIR}/assets/chess3d.png)

add_executable(chess3d
    src/App.cpp
    src/BoardGeometry.cpp
    src/BoardView.cpp
    src/Camera.cpp
    src/Chess.cpp
    src/Desktop.cpp
    src/Engine.cpp
    src/Gl.cpp
    src/Main.cpp
    src/PieceModels.cpp
    src/Renderer.cpp
    src/Resources.cpp
    src/Settings.cpp
    src/Sound.cpp
    src/Strings.cpp
    src/Text.cpp
    src/Themes.cpp
    src/UiPainter.cpp
    ${CMAKE_BINARY_DIR}/Embedded.cpp)

target_include_directories(chess3d PRIVATE src)
target_compile_definitions(chess3d PRIVATE CHESS3D_VERSION="${PROJECT_VERSION}")
target_compile_features(chess3d PRIVATE cxx_std_23)
set_target_properties(chess3d PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(chess3d PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
target_link_libraries(chess3d PRIVATE
    PkgConfig::SDL2 PkgConfig::FREETYPE PkgConfig::FONTCONFIG PkgConfig::PNG PkgConfig::X11 Threads::Threads)

include(GNUInstallDirs)
install(TARGETS chess3d RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES assets/chess3d.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES assets/chess3d.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps)
