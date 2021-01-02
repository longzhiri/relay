cmake_minimum_required (VERSION 3.6)

set(CMAKE_VERBOSE_MAKEFILE ON)

project(relay)

include_directories(3rd/include include)

link_directories(3rd/bin)

set(RELAY_LIB_SRC src/common.h src/common.c src/relay_server.c src/relay_client.c)
	
add_library(relay STATIC ${RELAY_LIB_SRC})

add_executable (relay_test src/relay_test.c)

if(WIN32)
    set(ZMQ_LIB libzmq-mt-s-4_3_3)
	set(RELAY_PLATFORM_LINK_LIBRARIES WS2_32 iphlpapi Rpcrt4)
	
	set(CompilerFlags
        CMAKE_CXX_FLAGS
        CMAKE_CXX_FLAGS_DEBUG
        CMAKE_CXX_FLAGS_RELEASE
        CMAKE_C_FLAGS
        CMAKE_C_FLAGS_DEBUG
        CMAKE_C_FLAGS_RELEASE
        )
		
	foreach(CompilerFlag ${CompilerFlags})
		string(REPLACE "/MD" "/MT" ${CompilerFlag} "${${CompilerFlag}}")
	endforeach()
endif()

if (UNIX AND NOT APPLE)
    set(ZMQ_LIB zmq)
    set(RELAY_PLATFORM_LINK_LIBRARIES stdc++ pthread)
endif()

target_link_libraries(relay_test czmq ${ZMQ_LIB} relay ${RELAY_PLATFORM_LINK_LIBRARIES})
