cmake_minimum_required(VERSION 3.19)
project(CloudMeeting LANGUAGES CXX)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets)
find_package(Qt6 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

set(FFMPEG_DIR ${CMAKE_SOURCE_DIR}/third_party/ffmpeg)

set(CMAKE_AUTOUIC_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/ui)

qt_add_executable(CloudMeeting
        src/logqueue.cpp
        src/main.cpp
        src/mainwindow.cpp
        src/mytextedit.cpp
        src/netheader.cpp
        src/screen.cpp
        src/DeviceEnumerator.cpp
        src/VideoCapture.cpp
        src/VideoWidget.cpp
        src/ffmpegDecoder.cpp
#        ${HEADERS}
        include/mainwindow.h
        include/DeviceEnumerator.h
        include/log_global.h
        include/logqueue.h
        include/mytextedit.h
        include/netheader.h
        include/screen.h
        include/ThreadSafeQueue.h
        include/VideoCapture.h
        include/VideoWidget.h
        include/AVSmartPtrs.h
        include/ffmpegDecoder.h
#        ${UIS}
        ui/mainwindow.ui


)

target_include_directories(CloudMeeting PRIVATE SYSTEM
        ${FFMPEG_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_directories(CloudMeeting PRIVATE ${FFMPEG_DIR}/lib)

target_link_libraries(CloudMeeting PRIVATE
        Qt6::Widgets

        # CMake 会在链接目录中自动寻找 avcodec.lib/avcodec.a 等
        avcodec
        avformat
        avutil
        avdevice
        swscale
        swresample
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
