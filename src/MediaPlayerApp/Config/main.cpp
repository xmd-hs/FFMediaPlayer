#include "mediaplayer.h"
#include <QtWidgets/QApplication>
#include <QCoreApplication>

int main(int argc, char *argv[])
{
	QApplication a(argc, argv);
	QCoreApplication::addLibraryPath(QCoreApplication::applicationDirPath());
	MediaPlayer w;
	w.show();
	return a.exec();
}
