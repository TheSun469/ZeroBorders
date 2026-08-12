set(ZB_SOURCES
    main.cpp
    App.h
    App.cpp
    core/Log.h
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
    input/WinInputCapturer.h
    input/WinInputCapturer.cpp
    input/WinInputInjector.h
    input/WinInputInjector.cpp
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
    ${CMAKE_SOURCE_DIR}/resources/app.rc
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)

add_executable(ZeroBorders WIN32 ${ZB_SOURCES})

target_include_directories(ZeroBorders PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ZeroBorders PRIVATE
    nlohmann_json::nlohmann_json
    Qt6::Widgets
    Qt6::Network
    ws2_32
    bcrypt
    iphlpapi
    user32
    gdi32
)

set_target_properties(ZeroBorders PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    AUTOMOC ON
    AUTORCC ON
    AUTOUIC ON
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
