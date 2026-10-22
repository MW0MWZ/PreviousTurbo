# The unit tests: the emulator's code (all but main()) with the tests' own main().
add_executable(pt_tests
	test_main.cpp
	misc_tests.cpp
	config_tests.cpp
	${CMAKE_SOURCE_DIR}/src/main.c
	$<TARGET_OBJECTS:PreviousCore>)
target_include_directories(pt_tests PRIVATE
	${CMAKE_SOURCE_DIR}/src/includes ${CMAKE_SOURCE_DIR}/src/debug ${CMAKE_SOURCE_DIR}/src/dsp
	${CMAKE_SOURCE_DIR}/src/softfloat ${CMAKE_SOURCE_DIR}/src/dimension ${CMAKE_SOURCE_DIR}/src/slirp
	${CMAKE_SOURCE_DIR}/src/gui ${CMAKE_SOURCE_DIR}/src/cpu ${CMAKE_BINARY_DIR} ${SDL3_INCLUDE_DIRS})
target_compile_features(pt_tests PRIVATE cxx_std_17)
# The program's main() is renamed: the tests have their own.
set_source_files_properties(${CMAKE_SOURCE_DIR}/src/main.c PROPERTIES
	COMPILE_DEFINITIONS "main=Previous_Main"
	COMPILE_OPTIONS "-Wno-missing-prototypes")
if(WIN32)
	target_compile_definitions(pt_tests PRIVATE SDL_MAIN_HANDLED)
endif()
target_link_libraries(pt_tests PRIVATE PreviousLibs)
add_test(NAME pt_tests COMMAND pt_tests)
