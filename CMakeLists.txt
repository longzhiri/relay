cmake_minimum_required (VERSION 3.8)

project(relay)

include_directories(3rd/include include)

link_directories(3rd/bin)

set(RELAY_LIB_SRC src/common.h src/common.c src/relay_server.c src/relay_client.c)
	
add_library(relay STATIC ${RELAY_LIB_SRC})

add_executable (relay_test src/relay_test.c)

if(WIN32)
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

target_link_libraries(relay_test libczmq libzmq-mt-s-4_3_3 relay ${RELAY_PLATFORM_LINK_LIBRARIES})
