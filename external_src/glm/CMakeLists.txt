cmake_minimum_required ( VERSION 3.0.2 )

project ( glm )

file(GLOB_RECURSE SOURCE_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.inl"
)

if(${CMAKE_VERSION} VERSION_LESS "3.19")
	add_library ( ${PROJECT_NAME} INTERFACE)
	target_sources(${PROJECT_NAME} INTERFACE
		${SOURCE_FILES}
	)
else()
	add_library ( ${PROJECT_NAME} INTERFACE ${SOURCE_FILES} )
	set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)
endif()

target_compile_definitions(${PROJECT_NAME} INTERFACE GLM_FORCE_UNRESTRICTED_GENTYPE)
    
target_include_directories(${PROJECT_NAME} INTERFACE
	"${CMAKE_CURRENT_SOURCE_DIR}/include"
)
