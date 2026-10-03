find_package(PkgConfig REQUIRED)
pkg_check_modules(QPDF REQUIRED IMPORTED_TARGET libqpdf)
find_package(Qt6 REQUIRED COMPONENTS Test)

add_subdirectory(fixtures/generator)
add_subdirectory(support/crashwriter)

# Фикстуры генерируются на этапе сборки: держать PDF в репозитории значит
# позволить им разойтись с генератором.
set(UQ_FIXTURES_DIR ${CMAKE_CURRENT_BINARY_DIR}/fixtures)
set(UQ_FIXTURE_FILES
    ${UQ_FIXTURES_DIR}/textbook.pdf
    ${UQ_FIXTURES_DIR}/mixed-pages.pdf
    ${UQ_FIXTURES_DIR}/no-outline.pdf
    ${UQ_FIXTURES_DIR}/large.pdf
    ${UQ_FIXTURES_DIR}/corrupt.pdf
    ${UQ_FIXTURES_DIR}/encrypted.pdf
)
add_custom_command(
    OUTPUT ${UQ_FIXTURE_FILES}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${UQ_FIXTURES_DIR}
    COMMAND ${CMAKE_COMMAND} -E env QT_QPA_PLATFORM=offscreen
            $<TARGET_FILE:uq_make_fixtures> ${UQ_FIXTURES_DIR}
    DEPENDS uq_make_fixtures
    COMMENT "Generating test fixture PDFs"
    VERBATIM
)
add_custom_target(uq_fixtures DEPENDS ${UQ_FIXTURE_FILES})

configure_file(fixtures/fixtures.h.in ${CMAKE_CURRENT_BINARY_DIR}/fixtures.h @ONLY)

# Тесты не открывают окон и не зависят от наличия дисплея.
function(uq_add_test name)
    qt_add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE uqnotes_core uqnotes_coreplugin Qt6::Test)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
    add_dependencies(${name} uq_fixtures)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

uq_add_test(tst_buildenv tst_buildenv.cpp)
uq_add_test(tst_pdfdocument tst_pdfdocument.cpp)
uq_add_test(tst_pagerender tst_pagerender.cpp)
uq_add_test(tst_documentlayout tst_documentlayout.cpp)
uq_add_test(tst_documentcanvas tst_documentcanvas.cpp)
uq_add_test(tst_strokesampler tst_strokesampler.cpp)
uq_add_test(tst_stroketessellator tst_stroketessellator.cpp)
uq_add_test(tst_inkdocument tst_inkdocument.cpp)
uq_add_test(tst_wetstroke tst_wetstroke.cpp)
uq_add_test(tst_canvasink tst_canvasink.cpp)
uq_add_test(tst_pagescenegraph tst_pagescenegraph.cpp)
uq_add_test(tst_insertoverlay tst_insertoverlay.cpp)
uq_add_test(tst_outlinepanel tst_outlinepanel.cpp)
uq_add_test(tst_penonlymode tst_penonlymode.cpp)
uq_add_test(tst_outlinenavigator tst_outlinenavigator.cpp)
uq_add_test(tst_crashrecovery tst_crashrecovery.cpp)
add_dependencies(tst_crashrecovery uq_crash_writer)
target_compile_definitions(tst_crashrecovery PRIVATE
    UQ_CRASH_WRITER="$<TARGET_FILE:uq_crash_writer>")

uq_add_test(tst_notestore tst_notestore.cpp)
uq_add_test(tst_export tst_export.cpp)
uq_add_test(tst_idlepower tst_idlepower.cpp)
uq_add_test(tst_framestats tst_framestats.cpp)

# Единственное исключение из правила «тесты не открывают окон»: покой в простое
# измеряется по кадрам, а под offscreen цикл рендера крутится вхолостую всегда
# и покой там неотличим от петли. Тест сам пропускается, если кадры не
# наблюдаемы, поэтому без сессии он не краснеет, а сообщает SKIP.
uq_add_test(tst_idleframes tst_idleframes.cpp)
set_tests_properties(tst_idleframes PROPERTIES ENVIRONMENT "")
