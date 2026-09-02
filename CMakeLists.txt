cmake_minimum_required(VERSION 3.16)
project(nwsm VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "This project supports Linux only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(nwsm main.cpp)
target_compile_options(nwsm PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(nwsm PRIVATE pthread)

if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    target_compile_options(nwsm PRIVATE -march=x86-64-v3 -mtune=generic)
endif()

install(TARGETS nwsm RUNTIME DESTINATION bin)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    "if(NOT EXISTS \"${CMAKE_BINARY_DIR}/install_manifest.txt\")\n"
    "    message(FATAL_ERROR \"Cannot find install manifest\")\n"
    "endif()\n"
    "file(READ \"${CMAKE_BINARY_DIR}/install_manifest.txt\" installed_files)\n"
    "string(REPLACE \"\\n\" \";\" installed_files \"\${installed_files}\")\n"
    "foreach(file \${installed_files})\n"
    "    if(EXISTS \"\${file}\" OR IS_SYMLINK \"\${file}\")\n"
    "        file(REMOVE \"\${file}\")\n"
    "    endif()\n"
    "endforeach()\n")
add_custom_target(uninstall
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake")

set(CPACK_PACKAGE_NAME "nwsm")
set(CPACK_DEBIAN_PACKAGE_NAME "nwsm")
set(CPACK_PACKAGE_VENDOR "Nitrux Latinoamericana S.C.")
set(CPACK_PACKAGE_CONTACT "hello@nxos.org")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Nitrux Workspace Session Manager")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "uri_herrera@nxos.org")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "dbus, openrc")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
include(CPack)
