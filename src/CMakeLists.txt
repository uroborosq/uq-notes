find_package(PkgConfig REQUIRED)
pkg_check_modules(QPDF REQUIRED IMPORTED_TARGET libqpdf)

# Вся логика живёт в библиотеке, чтобы тесты могли линковать её напрямую;
# исполняемый файл - только точка входа.
qt_add_library(uqnotes_core STATIC)

qt_add_qml_module(uqnotes_core
    URI UqNotes
    VERSION 1.0
    SOURCES
        canvas/documentcanvas.cpp
        canvas/documentcanvas.h
        canvas/documentlayout.cpp
        canvas/documentlayout.h
        canvas/insertlistmodel.cpp
        canvas/insertlistmodel.h
        ink/inkdocument.cpp
        ink/inkdocument.h
        ink/inkcoverage.h
        ink/inktypes.h
        ink/oneeurofilter.cpp
        ink/oneeurofilter.h
        ink/strokesampler.cpp
        ink/strokesampler.h
        ink/stroketessellator.cpp
        ink/stroketessellator.h
        ink/wetstroke.cpp
        ink/wetstroke.h
        export/noteexporter.cpp
        export/noteexporter.h
        input/touchguard.cpp
        input/touchguard.h
        storage/notestore.cpp
        storage/notestore.h
        pdf/outlinenavigator.cpp
        pdf/outlinenavigator.h
        pdf/pagerenderservice.cpp
        pdf/pagerenderservice.h
        pdf/pdfdocumentmodel.cpp
        pdf/pdfdocumentmodel.h
        perf/framestats.cpp
        perf/framestats.h
        perf/presentationstats.cpp
        perf/presentationstats.h
        perf/presentationfeedback.cpp
        perf/presentationfeedback.h
    QML_FILES
        qml/Main.qml
)

# Генератор регистрации QML-типов ищет заголовки по короткому имени, поэтому
# каждая папка модуля попадает в путь включения.
set(UQNOTES_MODULE_DIRS canvas export ink input pdf perf storage)
target_include_directories(uqnotes_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
foreach(dir IN LISTS UQNOTES_MODULE_DIRS)
    target_include_directories(uqnotes_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/${dir})
endforeach()
target_link_libraries(uqnotes_core PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Pdf
    Qt6::Sql
    Qt6::Concurrent
    PkgConfig::QPDF
    ink_stroke_modeler
)

# Задержка «событие -> показ» по wp_presentation (ink-beauty-pass, D1). Только
# для замера и только на Wayland: без wayland-client, протокола или
# Qt6::GuiPrivate приложение собирается как раньше, а PresentationFeedback
# тихо возвращает nullptr.
pkg_check_modules(WAYLAND_CLIENT QUIET IMPORTED_TARGET wayland-client)
pkg_get_variable(UQ_WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
pkg_get_variable(UQ_WAYLAND_SCANNER wayland-scanner wayland_scanner)
set(UQ_PRESENTATION_XML
    "${UQ_WAYLAND_PROTOCOLS_DIR}/stable/presentation-time/presentation-time.xml")
if(UQ_WP_PRESENTATION AND WAYLAND_CLIENT_FOUND AND UQ_WAYLAND_SCANNER
   AND EXISTS "${UQ_PRESENTATION_XML}" AND TARGET Qt6::GuiPrivate)
    set(UQ_WAYLAND_GEN ${CMAKE_CURRENT_BINARY_DIR}/wayland)
    file(MAKE_DIRECTORY ${UQ_WAYLAND_GEN})
    add_custom_command(
        OUTPUT ${UQ_WAYLAND_GEN}/presentation-time-client-protocol.h
               ${UQ_WAYLAND_GEN}/presentation-time-protocol.c
        COMMAND ${UQ_WAYLAND_SCANNER} client-header ${UQ_PRESENTATION_XML}
                ${UQ_WAYLAND_GEN}/presentation-time-client-protocol.h
        COMMAND ${UQ_WAYLAND_SCANNER} private-code ${UQ_PRESENTATION_XML}
                ${UQ_WAYLAND_GEN}/presentation-time-protocol.c
        DEPENDS ${UQ_PRESENTATION_XML}
        COMMENT "Generating wp_presentation client code"
        VERBATIM)
    # Код протокола - отдельной объектной библиотекой: libqpdf.pc кладёт в
    # флаги -std=c++20, и через uqnotes_core он попал бы в компиляцию C.
    add_library(uq_wp_presentation_protocol OBJECT
        ${UQ_WAYLAND_GEN}/presentation-time-protocol.c)
    target_link_libraries(uq_wp_presentation_protocol PRIVATE PkgConfig::WAYLAND_CLIENT)
    target_sources(uqnotes_core PRIVATE
        ${UQ_WAYLAND_GEN}/presentation-time-client-protocol.h
        $<TARGET_OBJECTS:uq_wp_presentation_protocol>)
    target_include_directories(uqnotes_core PRIVATE ${UQ_WAYLAND_GEN})
    target_link_libraries(uqnotes_core PRIVATE PkgConfig::WAYLAND_CLIENT Qt6::GuiPrivate)
    target_compile_definitions(uqnotes_core PRIVATE UQ_HAVE_WP_PRESENTATION)
    message(STATUS "uq-notes: замер задержки до показа (wp_presentation) включён")
endif()

qt_add_executable(uq-notes main.cpp)
target_link_libraries(uq-notes PRIVATE uqnotes_core uqnotes_coreplugin)

target_compile_definitions(uq-notes PRIVATE UQ_NOTES_VERSION="${PROJECT_VERSION}")
if(UQ_DIAGNOSTICS)
    target_compile_definitions(uq-notes PRIVATE UQ_DIAGNOSTICS)
endif()

install(TARGETS uq-notes RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
