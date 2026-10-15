# =============================================================================
#  arwReader — lector de Sony RAW (.arw/.sr2/.srf) para The Foundry Nuke
#
#  Uso típico (Windows, Nuke 14.0):
#    cmake -S . -B build-14.0 -G "Visual Studio 16 2019" -A x64 ^
#          -DNUKE_ROOT="C:/Program Files/Nuke14.0v5"
#    cmake --build build-14.0 --config Release
#    cmake --install build-14.0 --config Release --prefix "%USERPROFILE%/.nuke/arwReader"
#
#  Un binario por versión mayor.menor de Nuke: repite con otro NUKE_ROOT y otra
#  carpeta de build. El init.py elige automáticamente la subcarpeta correcta.
# =============================================================================
cmake_minimum_required(VERSION 3.16)
project(arwReader VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(ARW_BUILD_PLUGIN   "Compilar el plugin de Nuke (requiere NUKE_ROOT)" ON)
option(ARW_BUILD_PROBE    "Compilar arw_probe (prueba del núcleo sin Nuke)" OFF)
option(ARW_LIBRAW_OPENMP  "LibRaw con OpenMP (más rápido, posible conflicto con el runtime de Nuke)" OFF)
set(ARW_LIBRAW_TAG  "0.22.2" CACHE STRING "Versión de LibRaw a descargar")
set(ARW_LIBRAW_ROOT ""       CACHE PATH   "LibRaw ya instalado (vacío = descargar y compilar estático)")
set(NUKE_ROOT       ""       CACHE PATH   "Carpeta de instalación de Nuke (la que contiene include/DDImage)")

# -----------------------------------------------------------------------------
#  Linux: ABI de libstdc++. Nuke <= 14 usa la ABI antigua (0), Nuke >= 15 la
#  nueva (1). TODO el código (incluida LibRaw) debe compilarse con la misma,
#  por eso se fija aquí ANTES de añadir LibRaw.
# -----------------------------------------------------------------------------
set(NUKE_VERSION_MAJOR 0)
set(NUKE_VERSION_MINOR 0)
if(ARW_BUILD_PLUGIN)
  if(NOT EXISTS "${NUKE_ROOT}/include/DDImage/Reader.h")
    message(FATAL_ERROR "NUKE_ROOT no es válido: no existe ${NUKE_ROOT}/include/DDImage/Reader.h\n"
                        "Ejemplo: -DNUKE_ROOT=\"C:/Program Files/Nuke14.0v5\"")
  endif()
  file(STRINGS "${NUKE_ROOT}/include/DDImage/ddImageVersionNumbers.h" _vmaj REGEX "define kDDImageVersionMajorNum")
  file(STRINGS "${NUKE_ROOT}/include/DDImage/ddImageVersionNumbers.h" _vmin REGEX "define kDDImageVersionMinorNum")
  string(REGEX MATCH "[0-9]+" NUKE_VERSION_MAJOR "${_vmaj}")
  string(REGEX MATCH "[0-9]+" NUKE_VERSION_MINOR "${_vmin}")
  message(STATUS "arwReader: compilando para Nuke ${NUKE_VERSION_MAJOR}.${NUKE_VERSION_MINOR}")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND ARW_BUILD_PLUGIN)
  if(NUKE_VERSION_MAJOR LESS 15)
    set(_abi_default 0)
  else()
    set(_abi_default 1)
  endif()
  set(ARW_GLIBCXX_ABI "${_abi_default}" CACHE STRING "_GLIBCXX_USE_CXX11_ABI (0 = Nuke <= 14, 1 = Nuke >= 15)")
  add_compile_definitions(_GLIBCXX_USE_CXX11_ABI=${ARW_GLIBCXX_ABI})
endif()

if(MSVC)
  # Nuke usa el runtime dinámico (/MD); LibRaw estático debe usar el mismo.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
  add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

# -----------------------------------------------------------------------------
#  LibRaw
#  Por defecto se descarga y se compila ESTÁTICA dentro del plugin: así no hay
#  que distribuir DLLs aparte ni pelearse con versiones ya cargadas por Nuke.
# -----------------------------------------------------------------------------
if(ARW_LIBRAW_ROOT)
  find_path(LIBRAW_INCLUDE_DIR libraw/libraw.h PATHS "${ARW_LIBRAW_ROOT}/include" NO_DEFAULT_PATH)
  find_library(LIBRAW_LIBRARY NAMES raw_r libraw_r raw libraw PATHS "${ARW_LIBRAW_ROOT}/lib" NO_DEFAULT_PATH)
  if(NOT LIBRAW_INCLUDE_DIR OR NOT LIBRAW_LIBRARY)
    message(FATAL_ERROR "No se encontró LibRaw en ARW_LIBRAW_ROOT=${ARW_LIBRAW_ROOT}")
  endif()
  add_library(arw_libraw INTERFACE)
  target_include_directories(arw_libraw INTERFACE "${LIBRAW_INCLUDE_DIR}")
  target_link_libraries(arw_libraw INTERFACE "${LIBRAW_LIBRARY}")
else()
  include(FetchContent)
  FetchContent_Declare(libraw_src
    GIT_REPOSITORY https://github.com/LibRaw/LibRaw.git
    GIT_TAG        ${ARW_LIBRAW_TAG}
    GIT_SHALLOW    TRUE)
  FetchContent_Declare(libraw_cmake
    GIT_REPOSITORY https://github.com/LibRaw/LibRaw-cmake.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE)
  # LibRaw no tiene CMakeLists propio: MakeAvailable solo lo descarga.
  FetchContent_MakeAvailable(libraw_src)
  set(LIBRAW_PATH        "${libraw_src_SOURCE_DIR}" CACHE PATH "" FORCE)
  set(BUILD_SHARED_LIBS  OFF CACHE BOOL "" FORCE)
  set(ENABLE_OPENMP      ${ARW_LIBRAW_OPENMP} CACHE BOOL "" FORCE)
  set(ENABLE_LCMS        OFF CACHE BOOL "" FORCE)
  set(ENABLE_JASPER      OFF CACHE BOOL "" FORCE)
  set(ENABLE_EXAMPLES    OFF CACHE BOOL "" FORCE)
  set(LIBRAW_INSTALL     OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(libraw_cmake)
  add_library(arw_libraw INTERFACE)
  target_link_libraries(arw_libraw INTERFACE libraw::libraw_r)
endif()

# -----------------------------------------------------------------------------
#  Núcleo (independiente de Nuke)
# -----------------------------------------------------------------------------
add_library(arw_core STATIC src/RawDecoder.cpp src/RawDecoder.h)
target_include_directories(arw_core PUBLIC src)
target_link_libraries(arw_core PUBLIC arw_libraw)

if(ARW_BUILD_PROBE)
  add_executable(arw_probe tools/arw_probe.cpp)
  target_link_libraries(arw_probe PRIVATE arw_core)
endif()

# -----------------------------------------------------------------------------
#  Plugin de Nuke
# -----------------------------------------------------------------------------
if(ARW_BUILD_PLUGIN)
  if(APPLE)
    find_library(DDIMAGE_LIBRARY DDImage PATHS "${NUKE_ROOT}" "${NUKE_ROOT}/Contents/MacOS" NO_DEFAULT_PATH)
  else()
    find_library(DDIMAGE_LIBRARY DDImage PATHS "${NUKE_ROOT}" NO_DEFAULT_PATH)
  endif()
  if(NOT DDIMAGE_LIBRARY)
    message(FATAL_ERROR "No se encontró la librería DDImage en ${NUKE_ROOT}")
  endif()

  add_library(arwReader MODULE src/arwReader.cpp)
  target_include_directories(arwReader PRIVATE "${NUKE_ROOT}/include")
  target_link_libraries(arwReader PRIVATE arw_core "${DDIMAGE_LIBRARY}")
  # Nuke busca exactamente "arwReader.dll|.so|.dylib", sin prefijo "lib".
  set_target_properties(arwReader PROPERTIES PREFIX "")
  if(APPLE)
    set_target_properties(arwReader PROPERTIES SUFFIX ".dylib")
  endif()
  if(UNIX AND NOT APPLE)
    # No exportar símbolos de LibRaw: evita choques con otras copias en Nuke.
    target_link_options(arwReader PRIVATE "-Wl,--exclude-libs,ALL")
  endif()

  set(_dest "${NUKE_VERSION_MAJOR}.${NUKE_VERSION_MINOR}")
  install(TARGETS arwReader LIBRARY DESTINATION "${_dest}" RUNTIME DESTINATION "${_dest}")
  install(FILES nuke/sr2Reader.tcl nuke/srfReader.tcl DESTINATION "${_dest}")
  install(FILES nuke/init.py nuke/arw_colorspace.py DESTINATION ".")
endif()
