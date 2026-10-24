
include_directories(. ../.. ../debug ../includes ../gui ${SDL3_INCLUDE_DIRS})

if(PCAP_FOUND)
    include_directories(${PCAP_INCLUDE_DIR})
endif(PCAP_FOUND)

if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_C_COMPILER_ID MATCHES "Clang")
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-write-strings")
endif()

# The menus and dialogs are in ../gui
add_library(GuiSdl
    sdlaudio.c sdlevent.c sdlhost.c sdlkeymap.c sdlpointer.c sdlscreen.c
)

target_link_libraries(GuiSdl PRIVATE ${SDL3_LIBRARIES})
