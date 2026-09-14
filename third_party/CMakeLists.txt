# ink-stroke-modeler - модель траектории штриха, та же, что у rnote
# (openspec/changes/rnote-grade-stroke-fidelity/design.md, D3).
#
# Собственный CMakeLists библиотеки не используется: при выключенных тестах он
# всё равно тянет googletest через FetchContent на этапе конфигурации, а пакет
# собирается без сети. Поэтому здесь собираются только продуктовые исходники,
# против системного Abseil.
set(UQ_INK_STROKE_MODELER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ink-stroke-modeler)
if(NOT EXISTS ${UQ_INK_STROKE_MODELER_DIR}/ink_stroke_modeler/stroke_modeler.h)
    message(FATAL_ERROR
        "Нет исходников ink-stroke-modeler в third_party/ink-stroke-modeler. "
        "Получить их: git submodule update --init")
endif()

find_package(absl REQUIRED)

set(src ${UQ_INK_STROKE_MODELER_DIR}/ink_stroke_modeler)
add_library(ink_stroke_modeler STATIC
    ${src}/params.cc
    ${src}/stroke_modeler.cc
    ${src}/types.cc
    ${src}/internal/internal_types.cc
    ${src}/internal/loop_contraction_mitigation_modeler.cc
    ${src}/internal/position_modeler.cc
    ${src}/internal/stylus_state_modeler.cc
    ${src}/internal/utils.cc
    ${src}/internal/wobble_smoother.cc
    ${src}/internal/prediction/kalman_filter/axis_predictor.cc
    ${src}/internal/prediction/kalman_filter/kalman_filter.cc
    ${src}/internal/prediction/kalman_predictor.cc
    ${src}/internal/prediction/stroke_end_predictor.cc
)
# SYSTEM: предупреждения чужого кода не смешиваются с нашими.
target_include_directories(ink_stroke_modeler SYSTEM PUBLIC ${UQ_INK_STROKE_MODELER_DIR})
target_link_libraries(ink_stroke_modeler PUBLIC
    absl::status
    absl::statusor
    absl::strings
    absl::str_format
)

# Чужой код без Qt: разбирать его moc незачем.
set_target_properties(ink_stroke_modeler PROPERTIES AUTOMOC OFF)
