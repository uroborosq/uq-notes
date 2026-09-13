qt_add_executable(uq_spike_input_probe main.cpp probeitem.cpp probeitem.h)

qt_add_qml_module(uq_spike_input_probe
    URI UqInputProbe
    VERSION 1.0
    QML_FILES Main.qml
)

target_link_libraries(uq_spike_input_probe PRIVATE Qt6::Gui Qt6::Quick Qt6::QuickControls2)
