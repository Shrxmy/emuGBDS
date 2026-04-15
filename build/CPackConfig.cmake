# This file will be configured to contain variables for CPack. These variables
# should be set in the CMake list file of the project before CPack module is
# included. The list of available CPACK_xxx variables and their associated
# documentation may be obtained using
#  cpack --help-variable-list
#
# Some variables are common to all generators (e.g. CPACK_PACKAGE_NAME)
# and some are specific to a generator
# (e.g. CPACK_NSIS_EXTRA_INSTALL_COMMANDS). The generator specific variables
# usually begin with CPACK_<GENNAME>_xxxx.


set(CPACK_BINARY_7Z "OFF")
set(CPACK_BINARY_IFW "OFF")
set(CPACK_BINARY_INNOSETUP "OFF")
set(CPACK_BINARY_NSIS "ON")
set(CPACK_BINARY_NUGET "OFF")
set(CPACK_BINARY_WIX "OFF")
set(CPACK_BINARY_ZIP "OFF")
set(CPACK_BUILD_SOURCE_DIRS "D:/Programming/Personal/emuGBDS;D:/Programming/Personal/emuGBDS/build")
set(CPACK_CMAKE_GENERATOR "Visual Studio 17 2022")
set(CPACK_COMPONENTS_ALL "Unspecified;mgba;mgba-dev")
set(CPACK_COMPONENT_UNSPECIFIED_HIDDEN "TRUE")
set(CPACK_COMPONENT_UNSPECIFIED_REQUIRED "TRUE")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6")
set(CPACK_DEBIAN_PACKAGE_SECTION "games")
set(CPACK_DEB_COMPONENT_INSTALL "ON")
set(CPACK_DEFAULT_PACKAGE_DESCRIPTION_FILE "C:/Program Files/CMake/share/cmake-4.2/Templates/CPack.GenericDescription.txt")
set(CPACK_DEFAULT_PACKAGE_DESCRIPTION_SUMMARY "emuGBDS built using CMake")
set(CPACK_DMG_SLA_USE_RESOURCE_FILE_LICENSE "ON")
set(CPACK_GENERATOR "NSIS")
set(CPACK_INNOSETUP_ARCHITECTURE "x64")
set(CPACK_INSTALL_CMAKE_PROJECTS "D:/Programming/Personal/emuGBDS/build;emuGBDS;ALL;/")
set(CPACK_INSTALL_PREFIX "C:/Program Files (x86)/emuGBDS")
set(CPACK_MODULE_PATH "D:/Programming/Personal/emuGBDS/mgba-master/src/platform/cmake/")
set(CPACK_NSIS_DISPLAY_NAME "emuGBDS 0.11.0")
set(CPACK_NSIS_INSTALLER_ICON_CODE "")
set(CPACK_NSIS_INSTALLER_MUI_ICON_CODE "")
set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
set(CPACK_NSIS_PACKAGE_NAME "emuGBDS 0.11.0")
set(CPACK_NSIS_UNINSTALL_NAME "Uninstall")
set(CPACK_OUTPUT_CONFIG_FILE "D:/Programming/Personal/emuGBDS/build/CPackConfig.cmake")
set(CPACK_PACKAGE_CONTACT "Vicki Pfau <vi@endrift.com>")
set(CPACK_PACKAGE_DEFAULT_LOCATION "/")
set(CPACK_PACKAGE_DESCRIPTION_FILE "D:/Programming/Personal/emuGBDS/mgba-master/README.md")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "mGBA Game Boy Advance Emulator")
set(CPACK_PACKAGE_FILE_NAME "emuGBDS-0.11.0-win64")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "emuGBDS 0.11.0")
set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "emuGBDS 0.11.0")
set(CPACK_PACKAGE_NAME "emuGBDS")
set(CPACK_PACKAGE_RELOCATABLE "true")
set(CPACK_PACKAGE_VENDOR "Vicki Pfau")
set(CPACK_PACKAGE_VERSION "0.11.0")
set(CPACK_PACKAGE_VERSION_MAJOR "0")
set(CPACK_PACKAGE_VERSION_MINOR "11")
set(CPACK_PACKAGE_VERSION_PATCH "0")
set(CPACK_RESOURCE_FILE_LICENSE "D:/Programming/Personal/emuGBDS/mgba-master/LICENSE")
set(CPACK_RESOURCE_FILE_README "D:/Programming/Personal/emuGBDS/mgba-master/README.md")
set(CPACK_RESOURCE_FILE_WELCOME "C:/Program Files/CMake/share/cmake-4.2/Templates/CPack.GenericWelcome.txt")
set(CPACK_SET_DESTDIR "OFF")
set(CPACK_SOURCE_7Z "ON")
set(CPACK_SOURCE_GENERATOR "7Z;ZIP")
set(CPACK_SOURCE_OUTPUT_CONFIG_FILE "D:/Programming/Personal/emuGBDS/build/CPackSourceConfig.cmake")
set(CPACK_SOURCE_ZIP "ON")
set(CPACK_STRIP_FILES "ON")
set(CPACK_SYSTEM_NAME "win64")
set(CPACK_THREADS "1")
set(CPACK_TOPLEVEL_TAG "win64")
set(CPACK_WIX_SIZEOF_VOID_P "8")

if(NOT CPACK_PROPERTIES_FILE)
  set(CPACK_PROPERTIES_FILE "D:/Programming/Personal/emuGBDS/build/CPackProperties.cmake")
endif()

if(EXISTS ${CPACK_PROPERTIES_FILE})
  include(${CPACK_PROPERTIES_FILE})
endif()

# Configuration for component group "base"

# Configuration for component "mgba"

SET(CPACK_COMPONENTS_ALL Unspecified mgba mgba-dev)
set(CPACK_COMPONENT_MGBA_GROUP base)

# Configuration for component group "dev"
set(CPACK_COMPONENT_GROUP_DEV_PARENT_GROUP "base")

# Configuration for component "libmgba"

SET(CPACK_COMPONENTS_ALL Unspecified mgba mgba-dev)
set(CPACK_COMPONENT_LIBMGBA_GROUP dev)

# Configuration for component "mgba-dev"

SET(CPACK_COMPONENTS_ALL Unspecified mgba mgba-dev)
set(CPACK_COMPONENT_MGBA-DEV_GROUP dev)

# Configuration for component group "test"
set(CPACK_COMPONENT_GROUP_TEST_PARENT_GROUP "dev")

# Configuration for component "mgba-perf"

SET(CPACK_COMPONENTS_ALL Unspecified mgba mgba-dev)
set(CPACK_COMPONENT_MGBA-PERF_GROUP test)

# Configuration for component "mgba-test"

SET(CPACK_COMPONENTS_ALL Unspecified mgba mgba-dev)
set(CPACK_COMPONENT_MGBA-TEST_GROUP test)
