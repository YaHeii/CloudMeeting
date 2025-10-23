add_rules("mode.debug", "mode.release")
set_project("CloudMeeting")
add_requires("msvc")
set_languages("c++17")

-- 全局声明 vcpkg 的包
add_requires("vcpkg::ffmpeg", {
    version = "7.1.2",
    configs = {features = {"avcodec", "avformat", "swresample", "swscale", "avfilter", "avdevice"}} -- 确保 avdevice 也被包含
})
add_requires("vcpkg::libdatachannel", {
    version = "0.23.2",
    configs = {features = {"ws", "srtp"}}
})

-- 定义你的目标
target("CloudMeeting")
    -- 使用更明确的 qt.application 规则，它能更好地处理 MOC, UIC, RCC
    add_rules("qt.application")

    -- 将所有源文件和需要 MOC 处理的头文件都加入编译
    add_files("src/*.cpp", "include/*.h")
    add_files("ui/mainwindow.ui")

    add_includedirs("include")

    -- 应用 Qt 规则和 vcpkg 包
    add_packages("qt6", {modules = {"core", "gui", "network", "multimedia", "widgets"}})
    add_packages("vcpkg::ffmpeg", "vcpkg::libdatachannel")

    add_defines("AV_DLL")

    if is_plat("windows") then
        -- 包含所有需要的系统库
        add_syslinks(
            -- General
            "winmm", "crypt32", "iphlpapi", "ws2_32", "secur32", "bcrypt", "mswsock",
            -- For FFmpeg DirectShow & VFW
            "strmiids", "uuid", "ole32", "gdi32", "oleaut32", "vfw32", "user32"
        )
    end

    after_install(function (target)
        print("Installing %s ...", target:name())
        os.cp(target:targetfile(), target:installdir("bin"))
        target:add("qt", {deploy = true})
        print("%s installed!", target:name())
    end)