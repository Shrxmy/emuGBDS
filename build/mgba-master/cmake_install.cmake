# Install script for directory: D:/Programming/Personal/emuGBDS/mgba-master

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "C:/Program Files (x86)/emuGBDS")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/." TYPE STATIC_LIBRARY FILES "D:/Programming/Personal/emuGBDS/build/mgba-master/Debug/mgba.lib")
  elseif(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/." TYPE STATIC_LIBRARY FILES "D:/Programming/Personal/emuGBDS/build/mgba-master/Release/mgba.lib")
  elseif(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/." TYPE STATIC_LIBRARY FILES "D:/Programming/Personal/emuGBDS/build/mgba-master/MinSizeRel/mgba.lib")
  elseif(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/." TYPE STATIC_LIBRARY FILES "D:/Programming/Personal/emuGBDS/build/mgba-master/RelWithDebInfo/mgba.lib")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "mgba-dev" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include" TYPE DIRECTORY FILES "D:/Programming/Personal/emuGBDS/mgba-master/include/mgba" FILES_MATCHING REGEX "/[^/]*\\.h$")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "mgba-dev" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include" TYPE DIRECTORY FILES "D:/Programming/Personal/emuGBDS/mgba-master/include/mgba-util" FILES_MATCHING REGEX "/[^/]*\\.h$")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "mgba-dev" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/mgba" TYPE FILE FILES "D:/Programming/Personal/emuGBDS/build/mgba-master/include/mgba/flags.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "mgba" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/./licenses" TYPE FILE FILES "D:/Programming/Personal/emuGBDS/mgba-master/res/licenses/inih.txt")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "mgba" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/." TYPE FILE FILES
    "D:/Programming/Personal/emuGBDS/mgba-master/README.md"
    "D:/Programming/Personal/emuGBDS/mgba-master/README_DE.md"
    "D:/Programming/Personal/emuGBDS/mgba-master/README_ES.md"
    "D:/Programming/Personal/emuGBDS/mgba-master/README_JP.md"
    "D:/Programming/Personal/emuGBDS/mgba-master/README_ZH_CN.md"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "mgba" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/." TYPE FILE FILES
    "D:/Programming/Personal/emuGBDS/mgba-master/CHANGES"
    "D:/Programming/Personal/emuGBDS/mgba-master/LICENSE"
    )
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for each subdirectory.
  include("D:/Programming/Personal/emuGBDS/build/mgba-master/src/debugger/cmake_install.cmake")
  include("D:/Programming/Personal/emuGBDS/build/mgba-master/src/feature/cmake_install.cmake")
  include("D:/Programming/Personal/emuGBDS/build/mgba-master/src/arm/cmake_install.cmake")
  include("D:/Programming/Personal/emuGBDS/build/mgba-master/src/core/cmake_install.cmake")
  include("D:/Programming/Personal/emuGBDS/build/mgba-master/src/gb/cmake_install.cmake")
  include("D:/Programming/Personal/emuGBDS/build/mgba-master/src/gba/cmake_install.cmake")
  include("D:/Programming/Personal/emuGBDS/build/mgba-master/src/sm83/cmake_install.cmake")
  include("D:/Programming/Personal/emuGBDS/build/mgba-master/src/util/cmake_install.cmake")
  include("D:/Programming/Personal/emuGBDS/build/mgba-master/test/cmake_install.cmake")

endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "D:/Programming/Personal/emuGBDS/build/mgba-master/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
