# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

# Procedural eyes UI component (used when CONFIG_EYES_UI_ENABLED=y)
set(EYES_UI_COMPONENT_DIR "${CMAKE_CURRENT_LIST_DIR}/../eyes-ui-demo/components/eyes"
    CACHE PATH "Path to the eyes ESP-IDF component")
if(EXISTS "${EYES_UI_COMPONENT_DIR}/CMakeLists.txt")
    list(APPEND EXTRA_COMPONENT_DIRS "${EYES_UI_COMPONENT_DIR}")
else()
    message(WARNING "eyes component not found at ${EYES_UI_COMPONENT_DIR}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "2.5.3")
project(xiaozhi)
