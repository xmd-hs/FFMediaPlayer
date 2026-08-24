#pragma once

#include <QAbstractListModel>
#include <QVector>
#include <QString>
#include <QSettings>

struct PlaylistItem
{
	QString title;
	QString url;
	qint64 durationMs = 0;
	bool online = false;
};

enum class PlaylistLoopMode
{
	Off = 0,
	List = 1,
	One = 2
};

class PlaylistModel : public QAbstractListModel
{
	Q_OBJECT
public:
	explicit PlaylistModel(QObject *parent = nullptr);

	int rowCount(const QModelIndex &parent = QModelIndex()) const override;
	QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

	void clear();
	void setItems(const QVector<PlaylistItem> &items);
	void appendItem(const PlaylistItem &item);
	void removeAt(int row);
	bool isEmpty() const { return items_.isEmpty(); }
	int count() const { return items_.size(); }
	PlaylistItem itemAt(int row) const;
	int currentIndex() const { return current_; }
	void setCurrentIndex(int row);
	int nextIndex() const;
	int prevIndex() const;

	void setLoopMode(PlaylistLoopMode mode);
	PlaylistLoopMode loopMode() const { return loopMode_; }
	void setShuffle(bool on);
	bool shuffle() const { return shuffle_; }

	void saveToSettings(QSettings &settings) const;
	void loadFromSettings(QSettings &settings);

private:
	void rebuildShuffleOrder();
	QVector<PlaylistItem> items_;
	int current_ = -1;
	PlaylistLoopMode loopMode_ = PlaylistLoopMode::Off;
	bool shuffle_ = false;
	QVector<int> shuffleOrder_;
};
