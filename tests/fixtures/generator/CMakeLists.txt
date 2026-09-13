qt_add_executable(uq_make_fixtures main.cpp)
target_link_libraries(uq_make_fixtures PRIVATE Qt6::Gui PkgConfig::QPDF)
