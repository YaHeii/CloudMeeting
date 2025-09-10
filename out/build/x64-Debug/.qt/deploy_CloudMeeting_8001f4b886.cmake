include("D:/code/C++/QT/CloudMeeting/out/build/x64-Debug/.qt/QtDeploySupport.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/CloudMeeting-plugins.cmake" OPTIONAL)
set(__QT_DEPLOY_I18N_CATALOGS "qtbase")

qt6_deploy_runtime_dependencies(
    EXECUTABLE D:/code/C++/QT/CloudMeeting/out/build/x64-Debug/CloudMeeting.exe
    GENERATE_QT_CONF
)
