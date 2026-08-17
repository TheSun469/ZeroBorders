# 平台无关源文件
set(ZB_SOURCES
    main.cpp
    App.h
    App.cpp
    core/Log.h
    core/Log.cpp
    core/Types.h
    core/Hash.h
    core/Hash.cpp
    core/Event.h
    core/Protocol.h
    core/Protocol.cpp
    network/TcpTransport.h
    network/TcpTransport.cpp
    network/UdpDiscovery.h
    network/UdpDiscovery.cpp
    network/NetworkServer.h
    network/NetworkServer.cpp
    network/NetworkClient.h
    network/NetworkClient.cpp
    input/IInputCapturer.h
    input/IInputInjector.h
    core/ScreenLayout.h
    router/ScreenRouter.h
    router/ScreenRouter.cpp
    router/InputEventSender.h
    clipboard/ClipboardManager.h
    clipboard/ClipboardManager.cpp
    transfer/FileTransferManager.h
    transfer/FileTransferManager.cpp
    config/AppConfig.h
    config/ConfigManager.h
    config/ConfigManager.cpp
    gui/MainWindow.h
    gui/MainWindow.cpp
    gui/DeviceLayoutWidget.h
    gui/DeviceLayoutWidget.cpp
    gui/PathComboBox.h
    gui/PathComboBox.cpp
    gui/RemotePathComboBox.h
    gui/RemotePathComboBox.cpp
    gui/FileBrowserWidget.h
    gui/FileBrowserWidget.cpp
    gui/RemoteFileModel.h
    gui/RemoteFileModel.cpp
    gui/RemoteDirTreeModel.h
    gui/RemoteDirTreeModel.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)

# 平台相关源文件
if(WIN32)
    list(APPEND ZB_SOURCES
        input/WinInputCapturer.h
        input/WinInputCapturer.cpp
        input/WinInputInjector.h
        input/WinInputInjector.cpp
        ${CMAKE_SOURCE_DIR}/resources/app.rc
    )
else()
    list(APPEND ZB_SOURCES
        input/LinuxInputCapturer.h
        input/LinuxInputCapturer.cpp
        input/LinuxInputInjector.h
        input/LinuxInputInjector.cpp
    )
endif()

add_executable(ZeroBorders WIN32 ${ZB_SOURCES})

target_include_directories(ZeroBorders PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ZeroBorders PRIVATE
    nlohmann_json::nlohmann_json
    Qt6::Widgets
    Qt6::Network
)

if(WIN32)
    target_link_libraries(ZeroBorders PRIVATE
        ws2_32
        bcrypt
        iphlpapi
        user32
        gdi32
        dbghelp
        wtsapi32
    )
else()
    target_link_libraries(ZeroBorders PRIVATE
        ${X11_LIBRARIES}
        ${XTST_LIBRARIES}
        ${OPENSSL_LIBRARIES}
    )
    target_include_directories(ZeroBorders PRIVATE
        ${X11_INCLUDE_DIRS}
        ${XTST_INCLUDE_DIRS}
        ${OPENSSL_INCLUDE_DIRS}
    )
endif()

set_target_properties(ZeroBorders PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    AUTOUIC ON
)

if(WIN32)
    set_target_properties(ZeroBorders PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    # Copy Qt runtime DLLs next to the executable so it runs from the build tree.
    add_custom_command(TARGET ZeroBorders POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ZeroBorders>"
        COMMAND "${QT6_INSTALL_PREFIX}/bin/windeployqt"
                --no-translations --no-system-d3d-compiler --no-opengl-sw
                --no-quick-import --no-svg
                "$<TARGET_FILE:ZeroBorders>"
        VERBATIM
    )
endif()
