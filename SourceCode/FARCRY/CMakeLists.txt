project(FarCry)

set(SRC_FILES
    "Main.cpp"
)

if (WIN32)
    list(APPEND SRC_FILES "FarCry.rc")
endif()

if (ANDROID)
    add_library(${PROJECT_NAME} SHARED ${SRC_FILES})
else()
    add_executable(${PROJECT_NAME} WIN32 ${SRC_FILES})
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/SourceCode/CryCommon
    ${CMAKE_SOURCE_DIR}/Externals/SDL/include
)

if (ANDROID)
    target_link_libraries(${PROJECT_NAME} SDL3::SDL3 dl log)
else()
    target_link_libraries(${PROJECT_NAME} SDL3::SDL3 dl)
endif()
