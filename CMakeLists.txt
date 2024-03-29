cmake_minimum_required(VERSION 3.15)

# Get the version
execute_process(COMMAND git describe --tags OUTPUT_VARIABLE GIT_VERSION ERROR_QUIET)
string(STRIP "${GIT_VERSION}" GIT_VERSION)

# Set the project name
project(k8psh)

# Specify the compile options / requirements
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED True)
add_definitions(-D_CRT_SECURE_NO_WARNINGS)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DDEBUG")

if (NOT ("${GIT_VERSION}" STREQUAL ""))
  add_definitions(-DGIT_VERSION=${GIT_VERSION})
endif ()

if (MSVC)
  string(REGEX REPLACE "/W[1-3]" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  add_compile_options(/W4 /WX)
else ()
  add_compile_options(-Wall -Wextra -pedantic -Werror)
endif ()

include_directories(src)

if (WIN32)
  add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX -DWINVER=0x601 -D_WIN32_WINNT=0x601)
  link_libraries(ws2_32)
else ()
  find_package (Threads)
  link_libraries(Threads::Threads)
endif ()

# Specify the executable to build
add_executable(k8psh src/k8psh.cxx)
install(TARGETS k8psh)

if (MSVC)
  set_property(TARGET k8psh PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded$<$<CONFIG:Debug>:Debug>)
  target_compile_options(k8psh PRIVATE -MT$<$<CONFIG:Debug>:d>)
elseif (NOT APPLE)
  target_link_libraries(k8psh PRIVATE -static)
endif ()

# Create the server executable
macro (add_link target link)
  if (CMAKE_HOST_WIN32)
    add_custom_command(TARGET ${target} POST_BUILD COMMAND cmd /c del ${link}${CMAKE_EXECUTABLE_SUFFIX} & mklink ${link}${CMAKE_EXECUTABLE_SUFFIX} ${target}${CMAKE_EXECUTABLE_SUFFIX} || mklink /h ${link}${CMAKE_EXECUTABLE_SUFFIX} ${target}${CMAKE_EXECUTABLE_SUFFIX} BYPRODUCTS ${link}${CMAKE_EXECUTABLE_SUFFIX})
    add_custom_target(${link} ALL DEPENDS ${target})
  else ()
    add_custom_command(TARGET ${target} POST_BUILD COMMAND ln -fs ${target}${CMAKE_EXECUTABLE_SUFFIX} ${link}${CMAKE_EXECUTABLE_SUFFIX} BYPRODUCTS ${link}${CMAKE_EXECUTABLE_SUFFIX})
  endif ()
endmacro ()

macro (install_link target link)
  if (CMAKE_HOST_WIN32)
    install(CODE "execute_process(COMMAND cmd /c del ${link}${CMAKE_EXECUTABLE_SUFFIX} & mklink ${link}${CMAKE_EXECUTABLE_SUFFIX} ${target}${CMAKE_EXECUTABLE_SUFFIX} || mklink /h ${link}${CMAKE_EXECUTABLE_SUFFIX} ${target}${CMAKE_EXECUTABLE_SUFFIX} WORKING_DIRECTORY \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin)")
  else ()
    install(CODE "execute_process(COMMAND ln -fs ${target}${CMAKE_EXECUTABLE_SUFFIX} ${link}${CMAKE_EXECUTABLE_SUFFIX} WORKING_DIRECTORY \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin)")
  endif ()
  install(CODE "message(\"-- Created link: ${link} -> ${target}\")")
endmacro ()

add_link(k8psh k8pshd)
install_link(k8psh k8pshd)

# Add tests
enable_testing()

set(TESTS
  ConfigurationTest
  k8pshTest
  SocketTest
  UtilitiesTest
)

foreach (TEST ${TESTS})
  add_executable(${TEST} test/${TEST}.cxx)
  add_test(${TEST} ${TEST})
endforeach ()

add_custom_target(check COMMAND ${CMAKE_CTEST_COMMAND} --verbose)
add_dependencies(check ${TESTS})
