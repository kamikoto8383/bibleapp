cmake_minimum_required(VERSION 3.16)
project(BibleApp VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets PrintSupport Sql)

set(SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/TitleBar.cpp
    src/Module.cpp
    src/ModuleManager.cpp
    src/Converter.cpp
    src/BibleView.cpp
    src/ModuleManagerDialog.cpp
)

set(HEADERS
    src/MainWindow.h
    src/TitleBar.h
    src/Module.h
    src/ModuleManager.h
    src/Converter.h
    src/BibleView.h
    src/ModuleManagerDialog.h
)

qt6_add_resources(BIBLEAPP_RES resources/icons.qrc)
add_executable(bibleapp ${SOURCES} ${HEADERS} ${BIBLEAPP_RES})

target_link_libraries(bibleapp
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::PrintSupport
    Qt6::Sql
)

install(TARGETS bibleapp DESTINATION bin)
