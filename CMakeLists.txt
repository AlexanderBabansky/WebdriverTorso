cmake_minimum_required(VERSION 3.20)

set(webdrivertorso_VERSION 1.0)
project(webdrivertorso VERSION ${webdrivertorso_VERSION})

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set (LIBAV_EXT_PATH "" CACHE PATH "Libav filepath")
set (IMPLIB_LOCATION ${LIBAV_EXT_PATH}/bin)

find_package(EasyRTMP REQUIRED)

add_executable(${PROJECT_NAME})

target_sources(${PROJECT_NAME} PRIVATE
    main.cpp
    )

target_link_libraries(${PROJECT_NAME} PRIVATE
    ${IMPLIB_LOCATION}/avcodec.lib
    ${IMPLIB_LOCATION}/avformat.lib
    ${IMPLIB_LOCATION}/avutil.lib
    ${IMPLIB_LOCATION}/swresample.lib
    ${IMPLIB_LOCATION}/swscale.lib
    easyrtmp::easyrtmp
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${LIBAV_EXT_PATH}/include
)

if (NOT WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        pthread
        )
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE
        Ws2_32.lib
        )
endif()


list(APPEND DLLS "avcodec-60.dll")
list(APPEND DLLS "avformat-60.dll")
list(APPEND DLLS "avutil-58.dll")
list(APPEND DLLS "swresample-4.dll")
list(APPEND DLLS "swscale-7.dll")

foreach(DLL ${DLLS})
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different 
        ${IMPLIB_LOCATION}/${DLL}
        $<TARGET_FILE_DIR:${PROJECT_NAME}>)
endforeach()

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different 
        ${easyrtmp_PACKAGE_FOLDER_DEBUG}/bin/easyrtmp.dll
        $<TARGET_FILE_DIR:${PROJECT_NAME}>)

