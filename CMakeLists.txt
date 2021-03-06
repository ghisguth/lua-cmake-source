cmake_minimum_required(VERSION 2.6)
project(luacmake)

# module path
SET(luacmake_CMAKE_DIR "${luacmake_SOURCE_DIR}/etc/cmake")
SET(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${luacmake_CMAKE_DIR}")

if(${WITH_LUAJIT} EQUAL "1")
	set(WITH_LUACPP 0)
endif(${WITH_LUAJIT} EQUAL "1")

if(NOT WIN32)
	IF (CMAKE_SIZEOF_VOID_P MATCHES 4)
		set(ENFORCE_32_BIT 0)
		set(ENFORCE_64_BIT 0)
	ENDIF(CMAKE_SIZEOF_VOID_P MATCHES 4)
	if(${ENFORCE_64_BIT} EQUAL "1")
		set(ENFORCE_32_BIT 0)
	endif(${ENFORCE_64_BIT} EQUAL "1")
else(NOT WIN32)
	set(ENFORCE_32_BIT 0)
	set(ENFORCE_64_BIT 0)
endif(NOT WIN32)

# luajit check
if(NOT WIN32)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wunused")
	IF (CMAKE_SIZEOF_VOID_P MATCHES 4)
		set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=i686")
	ENDIF(CMAKE_SIZEOF_VOID_P MATCHES 4)
	if(${WITH_LUAJIT} EQUAL "1")
		IF (CMAKE_SIZEOF_VOID_P MATCHES 8 AND NOT ${ENFORCE_32_BIT} EQUAL "1")
			message( FATAL_ERROR " luajit is not supported on x86_64 platform. use 32 chroot in order to build project" )
		ENDIF (CMAKE_SIZEOF_VOID_P MATCHES 8 AND NOT ${ENFORCE_32_BIT} EQUAL "1")
	endif(${WITH_LUAJIT} EQUAL "1")
	if(${ENFORCE_32_BIT} EQUAL "1")
		set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -m32")
		set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -m32")
		set(CMAKE_LINK_FLAGS "${CMAKE_LINK_FLAGS} -m32")
	endif(${ENFORCE_32_BIT} EQUAL "1")
	if(${ENFORCE_64_BIT} EQUAL "1")
		set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -m64")
		set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -m64")
		set(CMAKE_LINK_FLAGS "${CMAKE_LINK_FLAGS} -m64")
	endif(${ENFORCE_64_BIT} EQUAL "1")
else(NOT WIN32)
	if(${WITH_LUAJIT} EQUAL "1")
			message( FATAL_ERROR " luajit is not support win32 platform. comment this message and fix cmake scripts" )
	endif(${WITH_LUAJIT} EQUAL "1")
endif(NOT WIN32)

# default build type
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif(NOT CMAKE_BUILD_TYPE)

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/../bin)
set(LIBRARY_OUTPUT_PATH ${PROJECT_BINARY_DIR}/../lib)

if(${WITH_LUAJIT} EQUAL "1")
	message(STATUS "pcre enabled only on native platform")
else(${WITH_LUAJIT} EQUAL "1")
	find_package(PCRE REQUIRED)
	include_directories(${PCRE_INCLUDE_DIR})
endif(${WITH_LUAJIT} EQUAL "1")


if(${WITH_LUAJIT} EQUAL "1")
	include_directories(${CMAKE_CURRENT_SOURCE_DIR}/libs/luajit/src/)
else(${WITH_LUAJIT} EQUAL "1")
	include_directories(${CMAKE_CURRENT_SOURCE_DIR}/libs/lua/src/)
endif(${WITH_LUAJIT} EQUAL "1")

subdirs(libs)
subdirs(src)
