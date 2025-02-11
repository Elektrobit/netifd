cmake_minimum_required(VERSION 3.5)

PROJECT(netifd C)

include(CheckCCompilerFlag)
check_c_compiler_flag(-Wimplicit-fallthrough HAS_IMPLICIT_FALLTHROUGH)

OPTION(ENABLE_PROTO_SHELL "enable shell protocol handlers" ON)

ADD_DEFINITIONS(-Wall -Werror)
IF(CMAKE_C_COMPILER_VERSION VERSION_GREATER 6)
	add_definitions(-Wextra -Werror=implicit-function-declaration)
	add_definitions(-Wformat -Werror=format-security -Werror=format-nonliteral)
ENDIF()
ADD_DEFINITIONS(-Os --std=gnu99 -Wmissing-declarations -Wno-unused-parameter -Wno-unused-but-set-parameter)

IF(HAS_IMPLICIT_FALLTHROUGH)
  ADD_DEFINITIONS(-Wimplicit-fallthrough)
ENDIF()

SET(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "")

SET(SOURCES
	main.c utils.c system.c tunnel.c handler.c
	interface.c interface-ip.c interface-event.c
	iprule.c proto.c proto-static.c
	config.c device.c bridge.c veth.c vlan.c alias.c
	macvlan.c ubus.c vlandev.c wireless.c
	extdev.c bonding.c proto-isc-dhcp.c proto-udhcp.c
	proto-zcip.c)

IF(ENABLE_PROTO_SHELL)
	LIST(APPEND SOURCES proto-shell.c)
ENDIF()

FIND_LIBRARY(uci NAMES uci)
FIND_LIBRARY(ubox NAMES ubox)
FIND_LIBRARY(ubus NAMES ubus)
FIND_LIBRARY(json NAMES json-c json)
FIND_LIBRARY(blobmsg_json NAMES blobmsg_json)

SET(LIBS ${ubox} ${ubus} ${uci} ${json} ${blobmsg_json} )

FIND_PATH(ubox_include_dir libubox/usock.h)
INCLUDE_DIRECTORIES(${ubox_include_dir})

IF (NOT DEFINED LIBNL_LIBS)
	include(FindPkgConfig)
	pkg_search_module(LIBNL libnl-3.0 libnl-3 libnl nl-3 nl)
	IF (LIBNL_FOUND)
		include_directories(${LIBNL_INCLUDE_DIRS})
		SET(LIBNL_LIBS ${LIBNL_LIBRARIES})
	ENDIF()
ENDIF()

ADD_CUSTOM_COMMAND(
	OUTPUT ethtool-modes.h
	COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/make_ethtool_modes_h.sh ${CMAKE_C_COMPILER} > ${CMAKE_CURRENT_BINARY_DIR}/ethtool-modes.h
	DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/make_ethtool_modes_h.sh
)
ADD_CUSTOM_TARGET(ethtool-modes-h DEPENDS ethtool-modes.h)

IF("${CMAKE_SYSTEM_NAME}" MATCHES "Linux" AND NOT DUMMY_MODE)
	SET(SOURCES ${SOURCES} system-linux.c)
	SET(LIBS ${LIBS} ${LIBNL_LIBS})
ELSE()
	ADD_DEFINITIONS(-DDUMMY_MODE=1)
	SET(SOURCES ${SOURCES} system-dummy.c)
ENDIF()

IF(DEBUG)
  ADD_DEFINITIONS(-DDEBUG -g3)
  IF(NO_OPTIMIZE)
    ADD_DEFINITIONS(-O0)
  ENDIF()
ENDIF()


ADD_EXECUTABLE(netifd ${SOURCES})
target_include_directories(netifd PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
TARGET_COMPILE_DEFINITIONS(netifd PRIVATE $<$<BOOL:${ENABLE_PROTO_SHELL}>:ENABLE_PROTO_SHELL>)

TARGET_LINK_LIBRARIES(netifd ${LIBS})

INSTALL(TARGETS netifd
	RUNTIME DESTINATION sbin
)

ADD_EXECUTABLE(isc-dhcp-script isc-dhcp-script.c)
TARGET_LINK_LIBRARIES(isc-dhcp-script ${ubus} ${ubox} ${LIBS})
INSTALL(TARGETS isc-dhcp-script
	RUNTIME DESTINATION libexec/netifd
)

ADD_EXECUTABLE(udhcp-script udhcp-script.c)
TARGET_LINK_LIBRARIES(udhcp-script ${ubus} ${ubox})
INSTALL(TARGETS udhcp-script
	RUNTIME DESTINATION libexec/netifd
)

ADD_EXECUTABLE(zcip-script zcip-script.c)
TARGET_LINK_LIBRARIES(zcip-script ${ubus} ${ubox})
INSTALL(TARGETS zcip-script
	RUNTIME DESTINATION libexec/netifd
)
ADD_DEPENDENCIES(netifd ethtool-modes-h)
