cmake_minimum_required(VERSION 3.0)

# Set the project name
project(k8psh)
include_directories(src)

# Specify the compile options / requirements
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)
add_definitions(-D_CRT_SECURE_NO_WARNINGS)

if (MSVC)
  string(REGEX REPLACE "/W[1-3]" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  add_compile_options(/W4 /WX)
else ()
  add_compile_options(-Wall -Wextra -pedantic -Werror)
endif ()

if (WIN32)
  link_libraries(ws2_32)
endif ()

# Specify the executable to build
add_executable(k8psh src/k8psh.cxx)
install(TARGETS k8psh)

if (MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
elseif (NOT APPLE)
  target_link_libraries(k8psh PRIVATE -static)
endif ()

# Create the server executable
macro (install_link target link)
  if (WIN32)
    install(CODE "execute_process(COMMAND cmd /c del ${link}.exe & mklink /h ${link}.exe ${target}.exe WORKING_DIRECTORY \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin)")
  else ()
    install(CODE "execute_process(COMMAND ln -fs ${target} ${link} WORKING_DIRECTORY \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin)")
  endif ()
  install(CODE "message(\"-- Created link: ${link} -> ${target}\")")
endmacro ()

install_link(k8psh k8pshd)

# Add tests
enable_testing()

set(TESTS
  ConfigurationTest
  SocketTest
  UtilitiesTest
)

foreach (TEST ${TESTS})
  add_executable(${TEST} test/${TEST}.cxx)
  add_test(${TEST} ${TEST})
endforeach ()

add_custom_target(check COMMAND ${CMAKE_CTEST_COMMAND} --verbose)
add_dependencies(check ${TESTS})
