find_package(PkgConfig REQUIRED)
pkg_check_modules(QPDF REQUIRED IMPORTED_TARGET libqpdf)

qt_add_executable(uq_spike_export_overlay main.cpp)
target_link_libraries(uq_spike_export_overlay PRIVATE Qt6::Gui Qt6::Pdf PkgConfig::QPDF)
