#include "playlistmodel.h"
#include <QFileInfo>
#include <QUrl>
#include <random>
#include <algorithm>

PlaylistModel::PlaylistModel(QObject *parent)
	: QAbstractListModel(parent)
{
}

int PlaylistModel::rowCount(const QModelIndex &parent) const
{
	if (parent.isValid()) return 0;
	return items_.size();
}

QVariant PlaylistModel::data(const QModelIndex &index, int role) const
{
	if (!index.isValid() || index.row() < 0 || index.row() >= items_.size())
		return QVariant();
	const PlaylistItem &it = items_.at(index.row());
	if (role == Qt::DisplayRole)
	{
		QString mark = (index.row() == current_) ? QString::fromUtf8("▶ ") : QString("  ");
		QString tag = it.online ? QString::fromUtf8("[在线] ") : QString();
		return mark + tag + it.title;
	}
	if (role == Qt::ToolTipRole)
		return it.url;
	if (role == Qt::UserRole)
		return it.url;
	return QVariant();
}

void PlaylistModel::clear()
{
	beginResetModel();
	items_.clear();
	shuffleOrder_.clear();
	current_ = -1;
	endResetModel();
}

void PlaylistModel::setItems(const QVector<PlaylistItem> &items)
{
	beginResetModel();
	items_ = items;
	current_ = items_.isEmpty() ? -1 : 0;
	rebuildShuffleOrder();
	endResetModel();
}

void PlaylistModel::appendItem(const PlaylistItem &item)
{
	beginInsertRows(QModelIndex(), items_.size(), items_.size());
	items_.push_back(item);
	endInsertRows();
	if (current_ < 0) current_ = 0;
	rebuildShuffleOrder();
}

void PlaylistModel::removeAt(int row)
{
	if (row < 0 || row >= items_.size()) return;
	beginRemoveRows(QModelIndex(), row, row);
	items_.removeAt(row);
	endRemoveRows();
	if (items_.isEmpty()) current_ = -1;
	else if (current_ >= items_.size()) current_ = items_.size() - 1;
	else if (current_ > row) --current_;
	rebuildShuffleOrder();
	if (!items_.isEmpty())
		emit dataChanged(index(0), index(items_.size() - 1));
}

PlaylistItem PlaylistModel::itemAt(int row) const
{
	if (row < 0 || row >= items_.size()) return {};
	return items_.at(row);
}

void PlaylistModel::setCurrentIndex(int row)
{
	if (row < -1 || row >= items_.size()) return;
	int old = current_;
	current_ = row;
	if (old >= 0 && old < items_.size())
		emit dataChanged(index(old), index(old));
	if (current_ >= 0)
		emit dataChanged(index(current_), index(current_));
}

void PlaylistModel::setLoopMode(PlaylistLoopMode mode)
{
	loopMode_ = mode;
}

void PlaylistModel::setShuffle(bool on)
{
	shuffle_ = on;
	rebuildShuffleOrder();
}

void PlaylistModel::rebuildShuffleOrder()
{
	shuffleOrder_.resize(items_.size());
	for (int i = 0; i < items_.size(); ++i)
		shuffleOrder_[i] = i;
	if (shuffleOrder_.size() > 1)
	{
		std::mt19937 rng{std::random_device{}()};
		std::shuffle(shuffleOrder_.begin(), shuffleOrder_.end(), rng);
	}
}

int PlaylistModel::nextIndex() const
{
	if (items_.isEmpty()) return -1;
	if (loopMode_ == PlaylistLoopMode::One && current_ >= 0)
		return current_;

	if (shuffle_ && !shuffleOrder_.isEmpty())
	{
		int pos = shuffleOrder_.indexOf(current_);
		if (pos < 0) return shuffleOrder_.first();
		if (pos + 1 < shuffleOrder_.size())
			return shuffleOrder_.at(pos + 1);
		if (loopMode_ == PlaylistLoopMode::List)
			return shuffleOrder_.first();
		return -1;
	}

	if (current_ < 0) return 0;
	if (current_ + 1 < items_.size()) return current_ + 1;
	if (loopMode_ == PlaylistLoopMode::List) return 0;
	return -1;
}

int PlaylistModel::prevIndex() const
{
	if (items_.isEmpty()) return -1;
	if (loopMode_ == PlaylistLoopMode::One && current_ >= 0)
		return current_;

	if (shuffle_ && !shuffleOrder_.isEmpty())
	{
		int pos = shuffleOrder_.indexOf(current_);
		if (pos < 0) return shuffleOrder_.first();
		if (pos > 0) return shuffleOrder_.at(pos - 1);
		if (loopMode_ == PlaylistLoopMode::List)
			return shuffleOrder_.last();
		return -1;
	}

	if (current_ <= 0)
	{
		if (loopMode_ == PlaylistLoopMode::List && !items_.isEmpty())
			return items_.size() - 1;
		return -1;
	}
	return current_ - 1;
}

void PlaylistModel::saveToSettings(QSettings &settings) const
{
	settings.beginWriteArray("playlist");
	for (int i = 0; i < items_.size(); ++i)
	{
		settings.setArrayIndex(i);
		settings.setValue("title", items_[i].title);
		settings.setValue("url", items_[i].url);
		settings.setValue("online", items_[i].online);
		settings.setValue("durationMs", items_[i].durationMs);
	}
	settings.endArray();
	settings.setValue("playlist/current", current_);
	settings.setValue("playlist/loop", (int)loopMode_);
	settings.setValue("playlist/shuffle", shuffle_);
}

void PlaylistModel::loadFromSettings(QSettings &settings)
{
	int n = settings.beginReadArray("playlist");
	QVector<PlaylistItem> loaded;
	for (int i = 0; i < n; ++i)
	{
		settings.setArrayIndex(i);
		PlaylistItem it;
		it.title = settings.value("title").toString();
		it.url = settings.value("url").toString();
		it.online = settings.value("online").toBool();
		it.durationMs = settings.value("durationMs").toLongLong();
		if (!it.url.isEmpty())
			loaded.push_back(it);
	}
	settings.endArray();
	setItems(loaded);
	int cur = settings.value("playlist/current", -1).toInt();
	if (cur >= 0 && cur < count())
		setCurrentIndex(cur);
	setLoopMode((PlaylistLoopMode)settings.value("playlist/loop", 0).toInt());
	setShuffle(settings.value("playlist/shuffle", false).toBool());
}
