cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Versione dell'immagine (esp_app_desc_t.version) = FIRMWARE_VERSION di
# main/version.h, non il git describe: ota_update.c la legge dalla partizione
# annullata dal bootloader per non reinstallarla, online_update.c rifiuta un
# file non piu' nuovo (revisione del 23/10/2026). CMAKE_CONFIGURE_DEPENDS: a
# ogni cambio di version.h CMake si riconfigura da solo, altrimenti l'immagine
# porterebbe ancora la versione vecchia e l'aggiornamento online la rifiuterebbe.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/main/version.h")
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/main/version.h" _fw_ver_line REGEX "^#define FIRMWARE_VERSION ")
string(REGEX REPLACE "^#define FIRMWARE_VERSION \"([^\"]+)\".*" "\\1" PROJECT_VER "${_fw_ver_line}")
if(NOT PROJECT_VER MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "FIRMWARE_VERSION non letta da main/version.h (trovato: '${PROJECT_VER}')")
endif()
project(baseesp32-ethelite)
