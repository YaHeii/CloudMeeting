cmake_minimum_required(VERSION 3.16)
project(CloudMeeting LANGUAGES CXX)
if (MSVC)
    # /FS 标志解决 PDB 文件并行写入冲突
    add_compile_options(/FS)
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(Qt6_DIR "D:/QT/6.9.3/msvc2022_64/lib/cmake/Qt6")
find_package(Qt6 6.9.3 REQUIRED COMPONENTS Core Widgets Multimedia)
find_package(ffmpeg REQUIRED)
find_package(LibDataChannel CONFIG REQUIRED)

qt_standard_project_setup()
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

add_executable(CloudMeeting
        # --- UI 文件 ---
        ui/mainwindow.ui

        # --- 源文件 (.cpp) ---
        src/Capture.cpp
        src/DeviceEnumerator.cpp
        src/ffmpegAudioDecoder.cpp
        src/ffmpegEncoder.cpp
        src/ffmpegVideoDecoder.cpp
        src/logqueue.cpp
        src/main.cpp
        src/mainwindow.cpp
        src/mytextedit.cpp
        src/netheader.cpp
        src/RtmpPublisher.cpp
        src/screen.cpp
        src/VideoWidget.cpp
        src/WebRTCPublisher.cpp
        src/RtmpAudioPlayer.cpp
        src/RtmpPuller.cpp
        src/RTPDepacketizer.cpp
        src/rtp_jitter.cpp
        src/WebRTCPuller.cpp

        include/AudioResampleConfig.h
        include/Capture.h
        include/DeviceEnumerator.h
        include/ffmpegAudioDecoder.h
        include/ffmpegEncoder.h
        include/ffmpegVideoDecoder.h
        include/logqueue.h
        include/mainwindow.h
        include/mytextedit.h
        include/WebRTCPublisher.h 
        include/AVSmartPtrs.h
        include/log_global.h 
        include/RtmpPublisher.h 
        include/screen.h 
        include/ThreadSafeQueue.h 
        include/VideoWidget.h
        include/netheader.h
        include/RtmpAudioPlayer.h
        include/RtmpPuller.h
        include/WebRTCPuller.h
        include/RTPDepacketizer.h
        include/rtp_jitter.h
 "include/WebRTCPuller.h" "src/WebRTCPuller.cpp")

set_target_properties(CloudMeeting PROPERTIES
    AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/ui"
)
# --- 添加头文件搜索路径 ---
target_include_directories(CloudMeeting PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${FFMPEG_INCLUDE_DIRS}
)
target_link_directories(CloudMeeting PRIVATE
        ${FFMPEG_LIBRARY_DIRS}
)
# --- 添加编译宏定义 ---
target_compile_definitions(CloudMeeting PRIVATE
        AV_DLL
)

if(MSVC)
    # 强制使用 UTF-8 编码处理源文件，解决 C4819 警告
    target_compile_options(CloudMeeting PRIVATE /utf-8)
    # 设置 Windows 子系统为窗口程序，而不是控制台
    set_target_properties(CloudMeeting PROPERTIES WIN32_EXECUTABLE ON)
endif()


# --- 链接所有需要的库 ---
target_link_libraries(CloudMeeting PRIVATE
        # --- Qt6 模块 ---
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Network
        Qt6::Multimedia

        # --- Vcpkg 库 ---
        ${FFMPEG_LIBRARIES}
        avdevice.lib
        swresample.lib
        swscale.lib

        LibDataChannel::LibDataChannel

        # --- Windows 系统库 ---
        winmm
        crypt32
        iphlpapi
        ws2_32
        secur32
        bcrypt
        mswsock
        strmiids
        uuid
        ole32
        gdi32
        oleaut32
        vfw32
        user32
)
