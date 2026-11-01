cmake_minimum_required(VERSION 3.16)
project(UniVerify CXX)

# Engine-free build of the Unified Universe core and its verification harness (D-015, D-016).
# Any C++17 compiler. -ffp-contract=off keeps the shared shader cores from fusing a*b+c into
# an FMA that the GPU path would not take (see UniHlslShim.h).

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(UNI_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

file(GLOB UNI_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
add_executable(univerify ${UNI_SRC})
target_include_directories(univerify PRIVATE
  ${UNI_ROOT}/Source/UniCore
  ${UNI_ROOT}/Shaders/Private/Core
  ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(univerify PRIVATE -O2 -Wall -Wextra -Werror -Wno-unused-function -ffp-contract=off -fno-fast-math)
set_target_properties(univerify PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/build)
