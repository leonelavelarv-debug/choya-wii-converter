# Prueba larga sin Dawn (tests/test_sin_dawn.py con CHOYA_DEPS): compila aurora con el WebGPU nulo de Choya y el
# programa prueba_aurora, sin descargar nada. CHOYA_DEPS es una carpeta con los clones de las dependencias de aurora
# (abseil-cpp, xxhash, fmt, sdl, imgui, zstd, freetype, tracy); CHOYA_AURORA, la carpeta con aurora-main ya ajustada
# por Choya (retarget.patch_aurora).
cmake_minimum_required(VERSION 3.25)
project(choya_sin_dawn LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(FETCHCONTENT_FULLY_DISCONNECTED ON)
foreach(dep ABSEIL-CPP:abseil-cpp XXHASH:xxhash FMT:fmt SDL:sdl IMGUI:imgui ZSTD:zstd FREETYPE:freetype TRACY:tracy)
  string(REPLACE ":" ";" par "${dep}")
  list(GET par 0 nombre)
  list(GET par 1 carpeta)
  set(FETCHCONTENT_SOURCE_DIR_${nombre} "${CHOYA_DEPS}/${carpeta}")
endforeach()
find_package(SQLite3 REQUIRED)
add_library(sqlite3 INTERFACE)
target_link_libraries(sqlite3 INTERFACE SQLite::SQLite3)
add_subdirectory("${CHOYA_AURORA}/aurora-main" aurora-build EXCLUDE_FROM_ALL)
add_executable(prueba_aurora "${CMAKE_CURRENT_LIST_DIR}/prueba_aurora.cpp")
target_link_libraries(prueba_aurora PRIVATE aurora::core aurora::gx aurora::vi aurora::mtx ${CMAKE_DL_LIBS})
add_executable(banco_gx "${CMAKE_CURRENT_LIST_DIR}/banco_gx.cpp")
target_link_libraries(banco_gx PRIVATE aurora::core aurora::gx aurora::vi aurora::mtx)
