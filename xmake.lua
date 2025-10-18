add_rules("mode.debug", "mode.release")
set_project("CloudMeeting")
add_requires("msvc")
set_languages("c++17")

add_requires("vcpkg::ffmpeg", {
    version = "7.1.2",
    configs = {features = {"avcodec", "avformat", "swresample", "swscale", "avfilter"}}
})
add_requires("vcpkg::libdatachannel", {
    version = "0.23.2",
    configs = {features = {"ws", "srtp"}}
})

target("CloudMeeting")
    add_rules("qt.application")
    add_files("src/*.cpp")
    add_files("ui/mainwindow.ui")

    add_includedirs("include")
    add_packages("qt")
    add_packages("vcpkg::ffmpeg", "vcpkg::libdatachannel")
    add_defines("AV_DLL")

    if is_plat("windows") then
        add_syslinks("winmm", "crypt32", "iphlpapi", "ws2_32", "secur32", "bcrypt")
    end


    after_install(function (target)
        print("Installing %s ...", target:name())

        os.cp(target:targetfile(), target:installdir("bin"))
        target:add("qt", {deploy = true})
        print("%s installed!", target:name())
    end)