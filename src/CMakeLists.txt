
set(SOURCES
	adb.c bmap.c cfgopts.c configuration.c change.c cycInt.c dialog.c dma.c 
	esp.c enet_slirp.c enet_pcap.c ethernet.c file.c floppy.c grab.c ioMem.c 
	ioMemTabNEXT.c ioMemTabTurbo.c kms.c m68000.c mo.c nbic.c ncc.c 
	paths.c printer.c queue.c ramdac.c reset.c rom.c rs.c rtcnvram.c scandir.c 
	scc.c scsi.c shortcut.c snd.c str.c sysReg.c tablet.c timing.c tmc.c video.c 
	NextBus.cpp)

# The NeXT ROMs, found beside the program (in the bundle's Resources on macOS)
set(ROMS ND_step1_v43.BIN Rev_0.8_v31.BIN Rev_1.0_v41.BIN Rev_2.5_v66.BIN Rev_3.3_v74.BIN)

if(APPLE)
	set(SOURCES ${SOURCES} gui-osx/paths.m gui-osx/shortcut.m)
	set_source_files_properties(gui-osx/paths.m gui-osx/shortcut.m PROPERTIES LANGUAGE C)
endif()

# On Windows the program is a "windows GUI" program, not a console one
if(WIN32)
	set(SOURCES ${SOURCES} gui-win/opencon.c)
	set(GUIWIN_RES gui-win/previous-winicon.rc)
	add_definitions(-DSDL_MAIN_HANDLED)
endif(WIN32)

include_directories(includes debug dsp softfloat dimension slirp gui
		    ${CMAKE_BINARY_DIR} ${SDL3_INCLUDE_DIRS} cpu)

if(PNG_FOUND)
	include_directories(${PNG_INCLUDE_DIR})
endif(PNG_FOUND)

if(PCAP_FOUND)
	include_directories(${PCAP_INCLUDE_DIR})
endif(PCAP_FOUND)

link_directories(${CMAKE_CURRENT_BINARY_DIR}/debug
		 ${CMAKE_CURRENT_BINARY_DIR}/softfloat
		 ${CMAKE_CURRENT_BINARY_DIR}/gui-sdl
		 ${CMAKE_CURRENT_BINARY_DIR}/cpu
		 ${CMAKE_CURRENT_BINARY_DIR}/dsp
		 ${CMAKE_CURRENT_BINARY_DIR}/dimension
		 ${CMAKE_CURRENT_BINARY_DIR}/slirp)

add_subdirectory(debug)
add_subdirectory(softfloat)
add_subdirectory(gui-sdl)
add_subdirectory(gui)
add_subdirectory(cpu)
add_subdirectory(dsp)
add_subdirectory(dimension)
add_subdirectory(slirp)
add_subdirectory(ditool)

# Everything but main(), for the program and the unit tests
add_library(PreviousCore OBJECT ${SOURCES})

# The libraries the program and the tests are linked with
add_library(PreviousLibs INTERFACE)
target_link_libraries(PreviousLibs INTERFACE Debug GuiSdl Gui UaeCpu DSP Dimension Slirp SoftFloat ${SDL3_LIBRARIES})
if(Math_FOUND AND NOT APPLE)
	target_link_libraries(PreviousLibs INTERFACE ${MATH_LIBRARY})
endif()
if(Readline_FOUND)
	target_link_libraries(PreviousLibs INTERFACE ${READLINE_LIBRARY})
endif(Readline_FOUND)
if(PNG_FOUND)
	target_link_libraries(PreviousLibs INTERFACE ${PNG_LIBRARY})
endif(PNG_FOUND)
if(PCAP_FOUND)
	target_link_libraries(PreviousLibs INTERFACE ${PCAP_LIBRARY})
endif(PCAP_FOUND)
if(APPLE)
	target_link_libraries(PreviousLibs INTERFACE "-framework Cocoa")
endif()
if(WIN32)
	# Needed for socket() on Windows, and for the Documents folder
	target_link_libraries(PreviousLibs INTERFACE ws2_32 Iphlpapi shell32)
endif(WIN32)
if(HAIKU)
	# Needed for socket() on Haiku
	target_link_libraries(PreviousLibs INTERFACE network)
endif(HAIKU)

if(ENABLE_OSX_BUNDLE)
	# A macOS application: build/PreviousTurbo.app, with the system menu bar,
	# its icon and the ROMs inside. build/previousturbo links to the program
	# inside, for the terminal. `cmake --install build --prefix DIR` makes a
	# self-contained copy in DIR, with the libraries it uses (SDL3, libpng,
	# ...) inside it.
	set(ICNS gui-osx/Previous.icns)
	configure_file(${CMAKE_SOURCE_DIR}/cmake/Info.plist.in ${CMAKE_BINARY_DIR}/Info.plist @ONLY)
	add_executable(${APP_NAME} MACOSX_BUNDLE main.c $<TARGET_OBJECTS:PreviousCore> ${ICNS} ${ROMS})
	set_source_files_properties(${ICNS} ${ROMS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
	set_target_properties(${APP_NAME} PROPERTIES
			      OUTPUT_NAME "PreviousTurbo"
			      MACOSX_BUNDLE_INFO_PLIST ${CMAKE_BINARY_DIR}/Info.plist)
	add_custom_command(TARGET ${APP_NAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E rm -f ${CMAKE_BINARY_DIR}/previousturbo
		COMMAND ${CMAKE_COMMAND} -E create_symlink "PreviousTurbo.app/Contents/MacOS/PreviousTurbo"
			${CMAKE_BINARY_DIR}/previousturbo
		VERBATIM)

elseif(WIN32)
	# Set a default rc compiler if it was not defined yet
	if(NOT CMAKE_RC_COMPILER)
		set(CMAKE_RC_COMPILER windres)
	endif(NOT CMAKE_RC_COMPILER)
	ENABLE_LANGUAGE(RC)
	set(CMAKE_RC_COMPILE_OBJECT "<CMAKE_RC_COMPILER> -Ocoff -o <OBJECT> <SOURCE>")
	set_source_files_properties(${GUIWIN_RES} PROPERTIES LANGUAGE RC)
	add_executable(${APP_NAME} WIN32 ${GUIWIN_RES} main.c $<TARGET_OBJECTS:PreviousCore>)

else()
	add_executable(${APP_NAME} main.c $<TARGET_OBJECTS:PreviousCore>)
endif(ENABLE_OSX_BUNDLE)

target_link_libraries(${APP_NAME} PreviousLibs)

if(NOT ENABLE_OSX_BUNDLE)
	# The ROMs beside the program in the build folder, so it runs from there
	foreach(rom ${ROMS})
		add_custom_command(TARGET ${APP_NAME} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				${CMAKE_CURRENT_SOURCE_DIR}/${rom} ${CMAKE_BINARY_DIR}/${rom}
			VERBATIM)
	endforeach()
endif()

if(ENABLE_OSX_BUNDLE)
	install(TARGETS ${APP_NAME} BUNDLE DESTINATION .)
	# The Info.plist is installed explicitly as well, and the libraries are
	# gathered from the program's own path, so neither depends on the other.
	install(FILES ${CMAKE_BINARY_DIR}/Info.plist DESTINATION "PreviousTurbo.app/Contents")
	install(CODE [[
		include(BundleUtilities)
		set(app "${CMAKE_INSTALL_PREFIX}/PreviousTurbo.app")
		fixup_bundle("${app}/Contents/MacOS/PreviousTurbo" "" "")
		# Signed for this machine (ad hoc): copying the libraries in changed them.
		execute_process(COMMAND codesign --force --deep --sign - "${app}" COMMAND_ERROR_IS_FATAL ANY)
		execute_process(COMMAND codesign --verify --deep --strict "${app}" COMMAND_ERROR_IS_FATAL ANY)
	]])
else()
	install(TARGETS ${APP_NAME} RUNTIME DESTINATION ${BINDIR})
	install(FILES Previous-icon.bmp DESTINATION ${DATADIR})
	install(FILES ${ROMS} DESTINATION ${DATADIR})
endif(ENABLE_OSX_BUNDLE)
