# stroke-lab - офлайн-стенд сравнения конвейеров рукописных чернил.
#
# Собирается отдельно от приложения (быстро, без QML/PDF):
#   cmake -S tools/stroke-lab -B build-lab -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-lab
# или вместе с приложением: -DUQ_BUILD_STROKE_LAB=ON в корневом CMake. В пакет
# не входит: install() здесь нет.
cmake_minimum_required(VERSION 3.21)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(stroke-lab LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui)

set(UQ_SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
if(NOT TARGET ink_stroke_modeler)
    add_subdirectory(${UQ_SOURCE_ROOT}/third_party ${CMAKE_CURRENT_BINARY_DIR}/third_party)
endif()

# Исходники чернил приложения - те же файлы, без копий.
add_library(stroke_lab_ink STATIC
    ${UQ_SOURCE_ROOT}/src/ink/oneeurofilter.cpp
    ${UQ_SOURCE_ROOT}/src/ink/strokesampler.cpp
    ${UQ_SOURCE_ROOT}/src/ink/stroketessellator.cpp
    ${UQ_SOURCE_ROOT}/src/ink/wetstroke.cpp
)
target_include_directories(stroke_lab_ink PUBLIC ${UQ_SOURCE_ROOT}/src ${UQ_SOURCE_ROOT}/src/ink)
target_link_libraries(stroke_lab_ink PUBLIC Qt6::Core Qt6::Gui ink_stroke_modeler)
set_target_properties(stroke_lab_ink PROPERTIES AUTOMOC OFF)

# Вариант - один .cpp в variants/: подхватывается сам.
file(GLOB STROKE_LAB_VARIANTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/variants/*.cpp)

add_executable(stroke-lab
    main.cpp
    corpus.cpp
    endcheck.cpp
    metrics.cpp
    raster.cpp
    aaraster.cpp
    rastermetrics.cpp
    render.cpp
    ${STROKE_LAB_VARIANTS}
)
target_include_directories(stroke-lab PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${UQ_SOURCE_ROOT}/tests        # support/referencestrokes.h
)
target_link_libraries(stroke-lab PRIVATE stroke_lab_ink Qt6::Core Qt6::Gui)
set_target_properties(stroke-lab PROPERTIES AUTOMOC OFF)

# Качество кромки тонкой линии: MSAA 4x/8x/16x против аналитического покрытия
# (exp/dry-refine). От вариантов не зависит.
add_executable(stroke-lab-aabench aabench.cpp raster.cpp)
target_include_directories(stroke-lab-aabench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(stroke-lab-aabench PRIVATE Qt6::Core Qt6::Gui)
set_target_properties(stroke-lab-aabench PROPERTIES AUTOMOC OFF)

# aa-gpu-bench: та же геометрия на настоящем GPU (QRhi/Vulkan) - проверка
# эмуляции сглаживания и цена кадра MSAA 4x против бахромы (gpu/bench.cpp).
# Собирается, только если есть Qt ShaderTools и приватные заголовки QtGui (rhi).
find_package(Qt6 QUIET COMPONENTS ShaderTools GuiPrivate)
if(TARGET Qt6::ShaderTools AND TARGET Qt6::GuiPrivate)
    add_executable(aa-gpu-bench
        gpu/bench.cpp
        corpus.cpp
        metrics.cpp
        raster.cpp
        aaraster.cpp
        rastermetrics.cpp
        render.cpp
        ${STROKE_LAB_VARIANTS}
    )
    target_include_directories(aa-gpu-bench PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${UQ_SOURCE_ROOT}/tests
    )
    target_link_libraries(aa-gpu-bench PRIVATE stroke_lab_ink Qt6::Core Qt6::Gui Qt6::GuiPrivate)
    set_target_properties(aa-gpu-bench PROPERTIES AUTOMOC OFF)
    qt_add_shaders(aa-gpu-bench "aa_gpu_shaders"
        PREFIX "/shaders"
        BASE gpu
        FILES gpu/ink.vert gpu/ink.frag gpu/flat.vert gpu/flat.frag
    )
endif()
