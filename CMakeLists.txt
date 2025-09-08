cmake_minimum_required(VERSION 3.19)
project(CloudMeeting LANGUAGES CXX)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets)
find_package(Qt6 REQUIRED COMPONENTS Widgets)

set(FFMPEG_DIR ${CMAKE_SOURCE_DIR}/third_party/ffmpeg)

qt_standard_project_setup()

set(CMAKE_AUTOUIC_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/ui)
#file(GLOB_RECURSE SOURCES "/src/*.cpp")
file(GLOB_RECURSE HEADERS include/*.h)
file(GLOB_RECURSE UIS ui/*.ui)

#message(STATUS "Found source files: ${HEADERS}")

qt_add_executable(CloudMeeting
        src/logqueue.cpp
        src/main.cpp
        src/mainwindow.cpp
        src/mytextedit.cpp
        src/netheader.cpp
        src/screen.cpp
        ${HEADERS}
        ${UIS}
)

target_include_directories(CloudMeeting PRIVATE SYSTEM
        ${FFMPEG_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_directories(CloudMeeting PRIVATE ${FFMPEG_DIR}/lib)


INCLUDE_DIRECTORIES(
    ${PROJECT_SOURCE_DIR}
)


target_link_libraries(CloudMeeting PRIVATE
    Qt6::Widgets
    # Qt::Gui
    Qt::Core
    Qt::Widgets

    # 再链接FFmpeg库
    # 注意：请根据你lib文件夹下的实际文件名修改
    # 如果是 .a 文件，就写 avcodec.a
    # 如果是 .lib 文件，就写 avcodec.lib 或者直接写 avcodec
    avcodec
    avformat
    avutil
    avdevice
    swscale
)

include(GNUInstallDirs)

install(TARGETS CloudMeeting
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET CloudMeeting
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
