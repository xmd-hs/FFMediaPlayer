#include <QApplication>
#include "player_window.h"

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    PlayerWindow window;
    window.show();
    return application.exec();
}
