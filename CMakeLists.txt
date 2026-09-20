cmake_minimum_required(VERSION 3.21)
project(uq-notes VERSION 0.7.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick QuickControls2 Pdf Sql Concurrent)

qt_standard_project_setup(REQUIRES 6.5)

# Тесты собираются по умолчанию, но пакету они не нужны: там нет ни Qt6::Test,
# ни генерации тестовых PDF.
option(UQ_BUILD_TESTS "Build the test suite" ON)
# Сценарный ввод и снимки экрана: нужны для замеров и проверки сценариев без
# человека у экрана. В пакет не входят.
option(UQ_DIAGNOSTICS "Build scripted-input and screenshot hooks" ON)

include(GNUInstallDirs)

if(UQ_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(third_party)
add_subdirectory(src)

if(UQ_BUILD_TESTS)
    add_subdirectory(spikes)
    add_subdirectory(tests)
endif()

# Установка в систему: исполняемый файл, ярлык, иконка и тип файла заметки.
install(FILES packaging/uq-notes.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/uq-notes.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES packaging/uq-notes.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages)

# Растровые размеры для окружений, которые не берут SVG из hicolor.
find_program(UQ_RSVG_CONVERT rsvg-convert)
if(UQ_RSVG_CONVERT)
    set(UQ_ICON_PNGS "")
    foreach(size IN ITEMS 32 48 64 128 256)
        set(png ${CMAKE_CURRENT_BINARY_DIR}/icons/${size}/uq-notes.png)
        add_custom_command(
            OUTPUT ${png}
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    ${CMAKE_CURRENT_BINARY_DIR}/icons/${size}
            COMMAND ${UQ_RSVG_CONVERT} -w ${size} -h ${size}
                    ${CMAKE_CURRENT_SOURCE_DIR}/packaging/uq-notes.svg -o ${png}
            DEPENDS packaging/uq-notes.svg
            COMMENT "Rendering ${size}x${size} icon"
            VERBATIM)
        list(APPEND UQ_ICON_PNGS ${png})
        install(FILES ${png}
                DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps)
    endforeach()
    add_custom_target(uq_icons ALL DEPENDS ${UQ_ICON_PNGS})
endif()
