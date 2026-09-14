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
        ink/inkdocument.cpp
        ink/inkdocument.h
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
    QML_FILES
        qml/Main.qml
)

# Генератор регистрации QML-типов ищет заголовки по короткому имени, поэтому
# каждая папка модуля попадает в путь включения.
set(UQNOTES_MODULE_DIRS canvas export ink pdf perf storage)
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

qt_add_executable(uq-notes main.cpp)
target_link_libraries(uq-notes PRIVATE uqnotes_core uqnotes_coreplugin)

target_compile_definitions(uq-notes PRIVATE UQ_NOTES_VERSION="${PROJECT_VERSION}")
if(UQ_DIAGNOSTICS)
    target_compile_definitions(uq-notes PRIVATE UQ_DIAGNOSTICS)
endif()

install(TARGETS uq-notes RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
