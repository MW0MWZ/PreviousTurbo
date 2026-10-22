# The window's menus, dialogs and status bar, drawn with Dear ImGui
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)

add_library(Gui STATIC
    gui.cpp dialogs.cpp files.cpp settings.cpp statusbar.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)
if(APPLE)
	# The system menu bar
	enable_language(OBJCXX)
	target_sources(Gui PRIVATE menu_mac.mm)
	set_source_files_properties(menu_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()

# Newer Clang warns about Dear ImGui passing an unset character with a
# length of 0, which is never read
check_cxx_compiler_flag(-Wuninitialized-const-pointer HAVE_WUNINITIALIZED_CONST_POINTER)
if(HAVE_WUNINITIALIZED_CONST_POINTER)
	set_source_files_properties(${IMGUI_DIR}/imgui_widgets.cpp PROPERTIES
				    COMPILE_OPTIONS "-Wno-uninitialized-const-pointer")
endif()

set_target_properties(Gui PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_compile_definitions(Gui PUBLIC IMGUI_DISABLE_DEMO_WINDOWS IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_include_directories(Gui PUBLIC . ${IMGUI_DIR}
                           PRIVATE ../includes ../debug ../dimension ../gui-sdl ${CMAKE_BINARY_DIR})
if(PCAP_FOUND)
	target_include_directories(Gui PRIVATE ${PCAP_INCLUDE_DIR})
endif()
target_link_libraries(Gui PUBLIC ${SDL3_LIBRARIES})
