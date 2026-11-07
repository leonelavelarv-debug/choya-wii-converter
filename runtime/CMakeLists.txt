cmake_minimum_required(VERSION 3.16)
project(wl_runtime CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(WL_WITH_GL "Compilar la subida a GLES y el visor wl_view (requiere SDL2 y GLES)" ON)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  # Cortex-A53 de la Allwinner H700
  add_compile_options(-mcpu=cortex-a53)
endif()
add_compile_options(-Wall -Wextra -O2)

find_package(ZLIB REQUIRED)

add_library(wl_core STATIC src/format.cpp src/pack.cpp src/texcache.cpp src/audio.cpp src/model.cpp)
target_include_directories(wl_core PUBLIC include)
target_link_libraries(wl_core PUBLIC ZLIB::ZLIB)

add_executable(wl_info tools/wl_info.cpp)
target_link_libraries(wl_info PRIVATE wl_core)

add_executable(wl_selftest tools/wl_selftest.cpp)
target_link_libraries(wl_selftest PRIVATE wl_core)

if(WL_WITH_GL)
  find_package(SDL2 REQUIRED)
  find_library(GLES_LIB NAMES GLESv2 REQUIRED)
  add_library(wl_gl STATIC src/gl_upload.cpp)
  target_compile_definitions(wl_gl PUBLIC WL_WITH_GL)
  target_link_libraries(wl_gl PUBLIC wl_core ${GLES_LIB})
  add_executable(wl_view tools/wl_view.cpp)
  add_executable(choya_model tools/choya_model.cpp)
  foreach(tool wl_view choya_model)
    if(TARGET SDL2::SDL2)
      target_link_libraries(${tool} PRIVATE wl_gl SDL2::SDL2)
    else()  # sdl2-config.cmake antiguo
      target_include_directories(${tool} PRIVATE ${SDL2_INCLUDE_DIRS})
      target_link_libraries(${tool} PRIVATE wl_gl ${SDL2_LIBRARIES})
    endif()
  endforeach()
endif()

install(TARGETS wl_info DESTINATION bin)
if(WL_WITH_GL)
  install(TARGETS wl_view choya_model DESTINATION bin)
endif()
