cmake_minimum_required(VERSION 3.16)
project(mixer-console VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick)

qt_add_executable(mixer-console main.cpp mixerclient.cpp calibration.cpp)

qt_add_qml_module(mixer-console
    URI MixerConsole
    VERSION 1.0
    QML_FILES
        main.qml
        qml/Strip.qml
        qml/Fader.qml
        qml/Knob.qml
        qml/MeterBar.qml
        qml/VUNeedle.qml
        qml/SpectrumView.qml
        qml/PageEffects.qml
        qml/PageMastering.qml
        qml/PagePads.qml
        qml/PageRouting.qml
        qml/PageSystem.qml
        qml/CalibrationOverlay.qml
        qml/StripFxDrawer.qml
        qml/IntroOverlay.qml
        qml/stripfx.js
)

target_link_libraries(mixer-console PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick)

install(TARGETS mixer-console RUNTIME DESTINATION bin)
