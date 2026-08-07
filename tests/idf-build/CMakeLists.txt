cmake_minimum_required(VERSION 3.16)

get_filename_component(DRAGON_CORE_ROOT "../.." ABSOLUTE
                       BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(EXTRA_COMPONENT_DIRS "${DRAGON_CORE_ROOT}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dragon_core_compile_test)
