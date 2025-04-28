cmake_minimum_required ( VERSION 3.0.2 )

project ( imgui )


add_library ( ${PROJECT_NAME}
	${CMAKE_CURRENT_SOURCE_DIR}/imconfig.h
	${CMAKE_CURRENT_SOURCE_DIR}/imgui.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/imgui.h
	${CMAKE_CURRENT_SOURCE_DIR}/imgui_demo.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/imgui_draw.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/imgui_internal.h
	${CMAKE_CURRENT_SOURCE_DIR}/imgui_tables.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/imgui_widgets.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/imstb_rectpack.h
	${CMAKE_CURRENT_SOURCE_DIR}/imstb_textedit.h
	${CMAKE_CURRENT_SOURCE_DIR}/imstb_truetype.h
	${CMAKE_CURRENT_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/misc/cpp/imgui_stdlib.h
#	${CMAKE_CURRENT_SOURCE_DIR}/misc/freetype/imgui_freetype.cpp
#	${CMAKE_CURRENT_SOURCE_DIR}/misc/freetype/imgui_freetype.h
	${CMAKE_CURRENT_SOURCE_DIR}/misc/natvis/imgui.natvis
    )

target_compile_definitions(${PROJECT_NAME} PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
	
target_include_directories(${PROJECT_NAME} PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${CMAKE_CURRENT_SOURCE_DIR}/.."
)
