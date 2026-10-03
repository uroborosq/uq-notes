cmake_minimum_required(VERSION 3.21)
# C - только для кода протокола wp_presentation, который генерирует
# wayland-scanner (src/CMakeLists.txt).
project(uq-notes VERSION 0.8.0 LANGUAGES C CXX)

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
# Задержка до показа кадра (wp_presentation, ink-beauty-pass D1). Работает
# только при UQ_FRAME_LOG на Wayland; OFF собирает заглушку - так проверяется
# сборка без wayland-client.
option(UQ_WP_PRESENTATION "Build the wp_presentation latency probe if wayland-client is available" ON)
if(UQ_WP_PRESENTATION)
    # Здесь, а не в src/: импортированные цели GuiPrivate (XKB::XKB) видны
    # только в каталоге find_package и ниже, а тесты линкуют uqnotes_core.
    find_package(Qt6 QUIET COMPONENTS GuiPrivate)
endif()
# Офлайн-стенд сравнения конвейеров чернил (tools/stroke-lab): метрики, по
# которым принимаются решения ink-beauty-pass. В пакет не входит, поэтому по
# умолчанию выключен; можно собрать и отдельно: cmake -S tools/stroke-lab.
option(UQ_BUILD_STROKE_LAB "Build the offline stroke lab" OFF)

include(GNUInstallDirs)

if(UQ_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(third_party)
add_subdirectory(src)

if(UQ_BUILD_STROKE_LAB)
    add_subdirectory(tools/stroke-lab)
endif()

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
