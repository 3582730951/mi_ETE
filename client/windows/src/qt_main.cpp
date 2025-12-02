#include <QApplication>

#include "qt_window.hpp"

extern "C" __declspec(dllexport) int LaunchMiClientQt(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QtClientWindow window;
    window.show();
    return app.exec();
}
