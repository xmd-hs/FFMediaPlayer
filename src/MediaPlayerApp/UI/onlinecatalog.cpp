#include "onlinecatalog.h"
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFile>
#include <QFileInfo>
#include <QUrl>

OnlineCatalog::OnlineCatalog(QObject *parent)
	: QObject(parent)
{
}

void OnlineCatalog::fetch(const QUrl &url)
{
	if (!url.isValid())
	{
		emit failed(QString::fromUtf8("无效的在线列表地址"));
		return;
	}
	QNetworkRequest req(url);
	req.setHeader(QNetworkRequest::UserAgentHeader, "FFMediaPlayer/1.0");
	QNetworkReply *reply = nam_.get(req);
	connect(reply, &QNetworkReply::finished, this, &OnlineCatalog::onFinished);
}

void OnlineCatalog::onFinished()
{
	auto *reply = qobject_cast<QNetworkReply*>(sender());
	if (!reply) return;
	reply->deleteLater();

	if (reply->error() != QNetworkReply::NoError)
	{
		emit failed(reply->errorString());
		return;
	}

	QVector<PlaylistItem> items;
	QString err;
	if (!parseJson(reply->readAll(), &items, &err))
	{
		emit failed(err);
		return;
	}
	emit loaded(items);
}

bool OnlineCatalog::parseJson(const QByteArray &data, QVector<PlaylistItem> *out, QString *error)
{
	if (!out) return false;
	out->clear();

	QJsonParseError pe{};
	QJsonDocument doc = QJsonDocument::fromJson(data, &pe);
	if (pe.error != QJsonParseError::NoError || !doc.isArray())
	{
		if (error) *error = QString::fromUtf8("JSON 格式错误：需要数组 [{title,url},...]");
		return false;
	}

	const QJsonArray arr = doc.array();
	for (const QJsonValue &v : arr)
	{
		if (!v.isObject()) continue;
		QJsonObject o = v.toObject();
		PlaylistItem it;
		it.url = o.value("url").toString().trimmed();
		it.title = o.value("title").toString().trimmed();
		it.durationMs = (qint64)o.value("durationMs").toDouble(0);
		it.online = true;
		if (it.url.isEmpty()) continue;
		if (it.title.isEmpty())
		{
			QUrl u(it.url);
			it.title = u.fileName();
			if (it.title.isEmpty()) it.title = it.url;
		}
		out->push_back(it);
	}

	if (out->isEmpty())
	{
		if (error) *error = QString::fromUtf8("在线列表为空或缺少 url 字段");
		return false;
	}
	return true;
}

bool OnlineCatalog::loadFromFile(const QString &path, QVector<PlaylistItem> *out, QString *error)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
	{
		if (error) *error = QString::fromUtf8("无法打开文件: ") + path;
		return false;
	}
	return parseJson(f.readAll(), out, error);
}
