#pragma once

#include "playlistmodel.h"
#include <QObject>
#include <QNetworkAccessManager>
#include <QUrl>

class OnlineCatalog : public QObject
{
	Q_OBJECT
public:
	explicit OnlineCatalog(QObject *parent = nullptr);

	void fetch(const QUrl &url);
	static bool parseJson(const QByteArray &data, QVector<PlaylistItem> *out, QString *error);
	static bool loadFromFile(const QString &path, QVector<PlaylistItem> *out, QString *error);

signals:
	void loaded(const QVector<PlaylistItem> &items);
	void failed(const QString &reason);

private slots:
	void onFinished();

private:
	QNetworkAccessManager nam_;
};
