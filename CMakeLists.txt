cmake_minimum_required(VERSION 3.16)

# Re-run CMake automatically when VERSION changes, even with an existing build directory.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/VERSION")

# Single source of truth for the firmware version.
# Change only VERSION (for example v2.0.0 -> v2.0.1).
file(READ "${CMAKE_SOURCE_DIR}/VERSION" AIRPLAY_PROJECT_VERSION)
string(STRIP "${AIRPLAY_PROJECT_VERSION}" AIRPLAY_PROJECT_VERSION)

if(NOT AIRPLAY_PROJECT_VERSION MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+([.-][0-9A-Za-z]+)*$")
    message(FATAL_ERROR
        "Invalid VERSION '${AIRPLAY_PROJECT_VERSION}'. Expected e.g. v2.0.0 or v2.0.1-beta1")
endif()

# ESP-IDF stores PROJECT_VER in esp_app_desc_t::version.
set(PROJECT_VER "${AIRPLAY_PROJECT_VERSION}")
set(EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The project name is also the generated .bin basename, so the version is
# automatically reflected in build/airplay-esp32_vX.Y.Z.bin.
project("airplay-esp32_${AIRPLAY_PROJECT_VERSION}")

spiffs_create_partition_image(storage "${CMAKE_SOURCE_DIR}/data" FLASH_IN_PROJECT)
