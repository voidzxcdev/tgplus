/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tgplus/tgplus_archive.h"

#include "base/timer.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_location.h"
#include "core/version.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "settings.h"
#include "storage/serialize_document.h"
#include "storage/storage_account.h"
#include "tgplus/tgplus_settings.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <set>

namespace Tgplus {
namespace {

constexpr auto kFormatVersion = 1;
constexpr auto kMaxDeletedPerPeer = size_t(5000);
constexpr auto kMaxEditedPerPeer = size_t(5000);
constexpr auto kMaxEditsPerMessage = size_t(30);
constexpr auto kMaxDismissedPerPeer = size_t(2000);
constexpr auto kFlushDelay = crl::time(1500);
constexpr auto kMaxSavedMediaSize = int64(64 * 1024 * 1024);
constexpr auto kMediaPhoto = quint8('p');
constexpr auto kMediaDocument = quint8('d');

struct PeerArchive {
	std::map<MsgId, DeletedRecord> deleted;
	std::map<MsgId, std::vector<EditRecord>> edits;
	std::set<MsgId> dismissed;

	[[nodiscard]] bool empty() const {
		return deleted.empty() && edits.empty() && dismissed.empty();
	}
};

[[nodiscard]] QString StoragePrefix() {
	return u"tgplus/p/"_q;
}

[[nodiscard]] QString StorageKey(PeerId peerId) {
	return StoragePrefix() + QString::number(peerId.value);
}

[[nodiscard]] QJsonArray SerializeEntities(const EntitiesInText &entities) {
	auto result = QJsonArray();
	for (const auto &entity : entities) {
		auto item = QJsonArray();
		item.append(int(entity.type()));
		item.append(entity.offset());
		item.append(entity.length());
		item.append(entity.data());
		result.append(item);
	}
	return result;
}

[[nodiscard]] EntitiesInText ParseEntities(const QJsonArray &list) {
	auto result = EntitiesInText();
	for (const auto &value : list) {
		const auto item = value.toArray();
		if (item.size() < 3) {
			continue;
		}
		const auto type = item.at(0).toInt();
		if (type <= 0 || type > int(EntityType::FormattedDate)) {
			continue;
		}
		result.push_back(EntityInText(
			static_cast<EntityType>(type),
			item.at(1).toInt(),
			item.at(2).toInt(),
			(item.size() > 3) ? item.at(3).toString() : QString()));
	}
	return result;
}

void WriteText(QJsonObject &object, const TextWithEntities &text) {
	object.insert(u"t"_q, text.text);
	if (!text.entities.isEmpty()) {
		object.insert(u"en"_q, SerializeEntities(text.entities));
	}
}

[[nodiscard]] TextWithEntities ReadText(const QJsonObject &object) {
	return {
		object.value(u"t"_q).toString(),
		ParseEntities(object.value(u"en"_q).toArray()),
	};
}

[[nodiscard]] QJsonObject SerializeRecord(const DeletedRecord &record) {
	auto result = QJsonObject();
	result.insert(u"id"_q, double(record.id.bare));
	result.insert(u"date"_q, double(record.date));
	result.insert(u"from"_q, QString::number(record.from.value));
	result.insert(u"fn"_q, record.fromName);
	if (!record.postAuthor.isEmpty()) {
		result.insert(u"pa"_q, record.postAuthor);
	}
	if (record.replyTo) {
		result.insert(u"rt"_q, double(record.replyTo.bare));
	}
	if (record.topicRootId) {
		result.insert(u"tr"_q, double(record.topicRootId.bare));
	}
	WriteText(result, record.text);
	if (!record.mediaLabel.isEmpty()) {
		result.insert(u"ml"_q, record.mediaLabel);
	}
	if (!record.media.isEmpty()) {
		result.insert(u"m"_q, QString::fromLatin1(record.media.toBase64()));
	}
	if (!record.savedFile.isEmpty()) {
		result.insert(u"sf"_q, record.savedFile);
	}
	result.insert(u"at"_q, double(record.deletedAt));
	result.insert(u"out"_q, record.out);
	result.insert(u"post"_q, record.post);
	result.insert(u"srv"_q, record.fromServer);
	return result;
}

[[nodiscard]] DeletedRecord ParseRecord(const QJsonObject &object) {
	auto result = DeletedRecord();
	result.id = MsgId(int64(object.value(u"id"_q).toDouble()));
	result.date = TimeId(object.value(u"date"_q).toDouble());
	result.from = PeerId(PeerIdHelper(
		object.value(u"from"_q).toString().toULongLong()));
	result.fromName = object.value(u"fn"_q).toString();
	result.postAuthor = object.value(u"pa"_q).toString();
	result.replyTo = MsgId(int64(object.value(u"rt"_q).toDouble()));
	result.topicRootId = MsgId(int64(object.value(u"tr"_q).toDouble()));
	result.text = ReadText(object);
	result.mediaLabel = object.value(u"ml"_q).toString();
	result.media = QByteArray::fromBase64(
		object.value(u"m"_q).toString().toLatin1());
	result.savedFile = object.value(u"sf"_q).toString();
	result.deletedAt = TimeId(object.value(u"at"_q).toDouble());
	result.out = object.value(u"out"_q).toBool();
	result.post = object.value(u"post"_q).toBool();
	result.fromServer = object.value(u"srv"_q).toBool();
	return result;
}

[[nodiscard]] QByteArray SerializeArchive(const PeerArchive &archive) {
	auto deleted = QJsonArray();
	for (const auto &[id, record] : archive.deleted) {
		deleted.append(SerializeRecord(record));
	}
	auto edits = QJsonArray();
	for (const auto &[id, list] : archive.edits) {
		auto versions = QJsonArray();
		for (const auto &edit : list) {
			auto version = QJsonObject();
			WriteText(version, edit.text);
			version.insert(u"at"_q, double(edit.replacedAt));
			versions.append(version);
		}
		auto entry = QJsonObject();
		entry.insert(u"id"_q, double(id.bare));
		entry.insert(u"h"_q, versions);
		edits.append(entry);
	}
	auto dismissed = QJsonArray();
	for (const auto &id : archive.dismissed) {
		dismissed.append(double(id.bare));
	}
	auto root = QJsonObject();
	root.insert(u"v"_q, kFormatVersion);
	root.insert(u"d"_q, deleted);
	root.insert(u"e"_q, edits);
	if (!dismissed.isEmpty()) {
		root.insert(u"x"_q, dismissed);
	}
	return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

[[nodiscard]] PeerArchive ParseArchive(const QByteArray &bytes) {
	auto result = PeerArchive();
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(bytes, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		LOG(("TGPlus: bad archive json."));
		return result;
	}
	const auto root = document.object();
	for (const auto &value : root.value(u"d"_q).toArray()) {
		auto record = ParseRecord(value.toObject());
		if (record.id) {
			const auto id = record.id;
			result.deleted.emplace(id, std::move(record));
		}
	}
	for (const auto &value : root.value(u"e"_q).toArray()) {
		const auto entry = value.toObject();
		const auto id = MsgId(int64(entry.value(u"id"_q).toDouble()));
		if (!id) {
			continue;
		}
		auto &list = result.edits[id];
		for (const auto &version : entry.value(u"h"_q).toArray()) {
			const auto object = version.toObject();
			list.push_back({
				.text = ReadText(object),
				.replacedAt = TimeId(object.value(u"at"_q).toDouble()),
			});
		}
	}
	for (const auto &value : root.value(u"x"_q).toArray()) {
		if (const auto id = MsgId(int64(value.toDouble()))) {
			result.dismissed.emplace(id);
		}
	}
	return result;
}

template <typename Map>
void TrimOldest(Map &map, size_t limit) {
	while (map.size() > limit) {
		map.erase(map.begin());
	}
}

class Archive final {
public:
	explicit Archive(not_null<Main::Session*> session);

	[[nodiscard]] PeerArchive &peer(PeerId peerId);
	[[nodiscard]] std::vector<PeerId> storedPeers() const;
	void changed(PeerId peerId);
	void flush();

private:
	const not_null<Main::Session*> _session;
	std::map<PeerId, PeerArchive> _peers;
	base::flat_set<PeerId> _dirty;
	base::Timer _flushTimer;

};

Archive::Archive(not_null<Main::Session*> session)
: _session(session)
, _flushTimer([=] { flush(); }) {
}

PeerArchive &Archive::peer(PeerId peerId) {
	const auto i = _peers.find(peerId);
	if (i != end(_peers)) {
		return i->second;
	}
	auto loaded = PeerArchive();
	const auto value = _session->local().readWalletEngineValue(
		StorageKey(peerId));
	if (value.state == Storage::WalletEngineValue::State::Read) {
		loaded = ParseArchive(value.bytes);
	}
	return _peers.emplace(peerId, std::move(loaded)).first->second;
}

std::vector<PeerId> Archive::storedPeers() const {
	auto result = base::flat_set<PeerId>();
	const auto prefix = StoragePrefix();
	for (const auto &key : _session->local().walletEngineStorageKeys(prefix)) {
		const auto value = key.mid(prefix.size()).toULongLong();
		if (value) {
			result.emplace(PeerId(PeerIdHelper(value)));
		}
	}
	for (const auto &[peerId, archive] : _peers) {
		if (!archive.deleted.empty() || !archive.edits.empty()) {
			result.emplace(peerId);
		}
	}
	return { begin(result), end(result) };
}

void Archive::changed(PeerId peerId) {
	_dirty.emplace(peerId);
	if (!_flushTimer.isActive()) {
		_flushTimer.callOnce(kFlushDelay);
	}
}

void Archive::flush() {
	_flushTimer.cancel();
	auto &local = _session->local();
	for (const auto peerId : base::take(_dirty)) {
		const auto i = _peers.find(peerId);
		if (i == end(_peers)) {
			continue;
		}
		auto &archive = i->second;
		TrimOldest(archive.deleted, kMaxDeletedPerPeer);
		TrimOldest(archive.edits, kMaxEditedPerPeer);
		TrimOldest(archive.dismissed, kMaxDismissedPerPeer);
		const auto key = StorageKey(peerId);
		if (archive.empty()) {
			local.removeWalletEngineValue(key);
		} else if (!local.writeWalletEngineValue(
				key,
				SerializeArchive(archive))) {
			LOG(("TGPlus: could not write archive for %1."
				).arg(peerId.value));
		}
	}
}

[[nodiscard]] std::map<not_null<Main::Session*>, std::unique_ptr<Archive>> &
Archives() {
	// WHY: never destroyed, every archive is flushed and removed together
	// with its session, so nothing is left for static destruction.
	static const auto result = new std::map<
		not_null<Main::Session*>,
		std::unique_ptr<Archive>>();
	return *result;
}

[[nodiscard]] Archive &For(not_null<Main::Session*> session) {
	auto &map = Archives();
	auto i = map.find(session);
	if (i == end(map)) {
		i = map.emplace(session, std::make_unique<Archive>(session)).first;
		session->lifetime().add([=] {
			auto &map = Archives();
			if (const auto j = map.find(session); j != end(map)) {
				j->second->flush();
				map.erase(j);
			}
		});
	}
	return *i->second;
}

[[nodiscard]] QString SafeFileName(QString name) {
	static const auto bad = QRegularExpression(u"[\\\\/:*?\"<>|]"_q);
	name.replace(bad, u"_"_q);
	return name.trimmed().isEmpty() ? u"file"_q : name.trimmed();
}

[[nodiscard]] QString TargetPath(
		not_null<HistoryItem*> item,
		const QString &name) {
	const auto folder = DeletedMediaFolder();
	if (!QDir().mkpath(folder)) {
		return QString();
	}
	return folder
		+ QString::number(item->history()->peer->id.value)
		+ '_'
		+ QString::number(item->id.bare)
		+ '_'
		+ SafeFileName(name);
}

[[nodiscard]] QString WriteBytes(const QString &path, const QByteArray &bytes) {
	if (path.isEmpty() || bytes.isEmpty()) {
		return QString();
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly)
		|| file.write(bytes) != bytes.size()) {
		return QString();
	}
	return path;
}

[[nodiscard]] QString SavePhotoCopy(
		not_null<HistoryItem*> item,
		not_null<PhotoData*> photo) {
	const auto view = photo->activeMediaView();
	if (!view) {
		return QString();
	}
	const auto path = TargetPath(item, u"photo.jpg"_q);
	if (path.isEmpty()) {
		return QString();
	}
	const auto bytes = view->imageBytes(Data::PhotoSize::Large);
	if (!bytes.isEmpty()) {
		return WriteBytes(path, bytes);
	}
	const auto image = view->image(Data::PhotoSize::Large);
	return (image && image->original().save(path, "JPG", 92))
		? path
		: QString();
}

[[nodiscard]] QString SaveDocumentCopy(
		not_null<HistoryItem*> item,
		not_null<DocumentData*> document) {
	if (document->size > kMaxSavedMediaSize) {
		return QString();
	}
	const auto name = document->filename();
	const auto path = TargetPath(item, name.isEmpty() ? u"file"_q : name);
	if (path.isEmpty()) {
		return QString();
	}
	const auto existing = document->filepath(true);
	if (!existing.isEmpty()) {
		QFile::remove(path);
		return QFile::copy(existing, path) ? path : QString();
	}
	const auto view = document->activeMediaView();
	return view ? WriteBytes(path, view->bytes()) : QString();
}

void FillMedia(DeletedRecord &record, not_null<HistoryItem*> item) {
	const auto media = item->media();
	if (!media || media->webpage()) {
		return;
	}
	const auto label = media->notificationText().text;
	if (media->ttlSeconds()) {
		record.mediaLabel = label;
	} else if (const auto photo = media->photo()) {
		record.media = SerializePhoto(photo);
		if (Enabled(Option::SaveDeletedMedia)) {
			record.savedFile = SavePhotoCopy(item, photo);
		}
	} else if (const auto document = media->document()) {
		record.media = SerializeDocument(document);
		if (Enabled(Option::SaveDeletedMedia)) {
			record.savedFile = SaveDocumentCopy(item, document);
		}
	} else {
		record.mediaLabel = label;
	}
}

[[nodiscard]] PhotoData *DeserializePhoto(
		not_null<Main::Session*> session,
		QDataStream &stream) {
	quint64 id = 0;
	quint64 access = 0;
	qint32 date = 0;
	qint32 dc = 0;
	qint32 hasStickers = 0;
	QByteArray fileReference, inlineThumbnail;
	stream
		>> id
		>> access
		>> fileReference
		>> date
		>> dc
		>> hasStickers
		>> inlineThumbnail;
	auto sizes = std::array<ImageWithLocation, 3>();
	for (auto &size : sizes) {
		auto location = QByteArray();
		qint32 bytesCount = 0;
		stream >> location >> bytesCount;
		if (const auto parsed = ImageLocation::FromSerialized(location)) {
			size.location = *parsed;
		}
		size.bytesCount = bytesCount;
	}
	if (stream.status() != QDataStream::Ok || !id) {
		return nullptr;
	}
	return session->data().photo(
		PhotoId(id),
		uint64(access),
		fileReference,
		TimeId(date),
		dc,
		(hasStickers != 0),
		inlineThumbnail,
		sizes[0],
		sizes[1],
		sizes[2],
		ImageWithLocation(),
		ImageWithLocation(),
		crl::time(0)).get();
}

[[nodiscard]] not_null<HistoryItem*> CreateItem(
		not_null<History*> history,
		const DeletedRecord &record) {
	const auto session = &history->session();
	const auto peerId = history->peer->id;
	auto flags = MessageFlags(MessageFlag::HistoryEntry);
	if (record.out) {
		flags |= MessageFlag::Outgoing;
	}
	if (record.post) {
		flags |= MessageFlag::Post;
	}
	if (record.from && record.from != peerId) {
		flags |= MessageFlag::HasFromId;
	}
	auto replyTo = FullReplyTo();
	const auto topicRootId = history->isForum()
		? record.topicRootId
		: MsgId();
	if (record.replyTo || topicRootId) {
		flags |= MessageFlag::HasReplyInfo;
		replyTo.messageId = FullMsgId(
			peerId,
			record.replyTo ? record.replyTo : topicRootId);
		replyTo.topicRootId = topicRootId;
	}
	auto fields = HistoryItemCommonFields{
		.id = record.id,
		.flags = flags,
		.from = record.from,
		.replyTo = replyTo,
		.date = record.date,
		.postAuthor = record.postAuthor,
	};
	auto text = record.text;
	if (!record.mediaLabel.isEmpty()) {
		auto label = TextWithEntities{ record.mediaLabel };
		if (!text.empty()) {
			label.append(u"\n"_q).append(text);
		}
		text = std::move(label);
	}
	if (!record.media.isEmpty()) {
		QDataStream stream(record.media);
		stream.setVersion(QDataStream::Qt_5_1);
		quint8 kind = 0;
		qint32 version = 0;
		stream >> kind >> version;
		if (kind == kMediaPhoto) {
			if (const auto photo = DeserializePhoto(session, stream)) {
				return history->makeMessage(
					std::move(fields),
					not_null<PhotoData*>(photo),
					text);
			}
		} else if (kind == kMediaDocument) {
			const auto document = Serialize::Document::readFromStream(
				session,
				version,
				stream);
			if (document) {
				if (!record.savedFile.isEmpty()
					&& QFile::exists(record.savedFile)) {
					document->setLocation(
						Core::FileLocation(record.savedFile));
				}
				return history->makeMessage(
					std::move(fields),
					not_null<DocumentData*>(document),
					text);
			}
		}
	}
	return history->makeMessage(
		std::move(fields),
		text,
		MTP_messageMediaEmpty());
}

} // namespace

void RememberDeleted(not_null<HistoryItem*> item) {
	if (item->isDeletedOnServer()) {
		return;
	}
	const auto history = item->history();
	const auto from = item->from();
	auto record = DeletedRecord{
		.id = item->id,
		.date = item->date(),
		.from = from->id,
		.fromName = from->name(),
		.postAuthor = item->originalPostAuthor(),
		.replyTo = item->replyToId(),
		.topicRootId = item->topicRootId(),
		.text = item->originalText(),
		.deletedAt = base::unixtime::now(),
		.out = item->out(),
		.post = item->isPost(),
	};
	FillMedia(record, item);
	auto &archive = For(&history->session());
	auto &peer = archive.peer(history->peer->id);
	peer.deleted[record.id] = std::move(record);
	archive.changed(history->peer->id);
}

void RememberEdit(
		not_null<HistoryItem*> item,
		const TextWithEntities &updated) {
	if (!Enabled(Option::EditHistory)
		|| !item->isRegular()
		|| item->isDeletedOnServer()) {
		return;
	}
	const auto &current = item->originalText();
	if (current.empty() || current == updated) {
		return;
	}
	const auto history = item->history();
	auto &archive = For(&history->session());
	auto &list = archive.peer(history->peer->id).edits[item->id];
	if (!list.empty() && list.back().text == current) {
		return;
	}
	list.push_back({
		.text = current,
		.replacedAt = base::unixtime::now(),
	});
	if (list.size() > kMaxEditsPerMessage) {
		list.erase(list.begin());
	}
	archive.changed(history->peer->id);
}

void ForgetDeleted(not_null<HistoryItem*> item) {
	DismissDeleted(item->history(), { MTP_int(int(item->id.bare)) });
	item->destroy();
}

void DismissDeleted(not_null<History*> history, const QVector<MTPint> &ids) {
	if (ids.isEmpty()) {
		return;
	}
	const auto peerId = history->peer->id;
	auto &archive = For(&history->session());
	auto &peer = archive.peer(peerId);
	for (const auto &id : ids) {
		const auto msgId = MsgId(id.v);
		peer.deleted.erase(msgId);
		peer.edits.erase(msgId);
		peer.dismissed.emplace(msgId);
	}
	archive.changed(peerId);
}

std::vector<EditRecord> EditsFor(not_null<HistoryItem*> item) {
	if (!item->isRegular()) {
		return {};
	}
	const auto history = item->history();
	auto &peer = For(&history->session()).peer(history->peer->id);
	const auto i = peer.edits.find(item->id);
	return (i != end(peer.edits)) ? i->second : std::vector<EditRecord>();
}

bool HasDeleted(not_null<PeerData*> peer) {
	return !For(&peer->session()).peer(peer->id).deleted.empty();
}

std::vector<LogEntry> DeletedLog(
		not_null<Main::Session*> session,
		PeerData *peer) {
	auto &archive = For(session);
	const auto peers = peer
		? std::vector<PeerId>{ peer->id }
		: archive.storedPeers();
	auto result = std::vector<LogEntry>();
	for (const auto peerId : peers) {
		for (const auto &[id, record] : archive.peer(peerId).deleted) {
			result.push_back({ .peer = peerId, .record = record });
		}
	}
	ranges::sort(result, ranges::greater(), [](const LogEntry &entry) {
		return entry.record.deletedAt;
	});
	return result;
}

std::vector<not_null<HistoryItem*>> RestoreDeleted(
		not_null<History*> history,
		MsgId after,
		MsgId before) {
	auto result = std::vector<not_null<HistoryItem*>>();
	if (!Core::App().settings().keepDeletedMessages() || before <= after) {
		return result;
	}
	const auto owner = &history->owner();
	const auto peerId = history->peer->id;
	auto &peer = For(&history->session()).peer(peerId);
	auto created = std::vector<not_null<HistoryItem*>>();
	auto i = peer.deleted.upper_bound(after);
	for (; i != end(peer.deleted) && i->first < before; ++i) {
		if (const auto existing = owner->message(peerId, i->first)) {
			// WHY: an unloaded history keeps its items without views, and
			// the server will not send a deleted one back to re-attach it.
			if (existing->isDeletedOnServer()
				&& !existing->mainView()
				&& existing->history() == history) {
				result.push_back(existing);
			}
		} else {
			created.push_back(CreateItem(history, i->second));
			result.push_back(created.back());
		}
	}
	for (const auto &item : created) {
		item->markDeletedOnServer();
	}
	return result;
}

bool ImportDeleted(
		not_null<Main::Session*> session,
		PeerId peerId,
		DeletedRecord record) {
	if (!record.id) {
		return false;
	}
	auto &archive = For(session);
	auto &peer = archive.peer(peerId);
	if (peer.dismissed.contains(record.id)) {
		return false;
	}
	const auto i = peer.deleted.find(record.id);
	if (i != end(peer.deleted)) {
		if (!i->second.media.isEmpty() || record.media.isEmpty()) {
			return false;
		}
		i->second.media = std::move(record.media);
	} else {
		record.fromServer = true;
		const auto id = record.id;
		peer.deleted.emplace(id, std::move(record));
	}
	archive.changed(peerId);
	return true;
}

bool ImportEdit(
		not_null<Main::Session*> session,
		PeerId peerId,
		MsgId id,
		EditRecord record) {
	if (!id || record.text.empty()) {
		return false;
	}
	auto &archive = For(session);
	auto &peer = archive.peer(peerId);
	if (peer.dismissed.contains(id)) {
		return false;
	}
	auto &list = peer.edits[id];
	const auto same = [&](const EditRecord &existing) {
		return (existing.text == record.text);
	};
	if (ranges::any_of(list, same)) {
		return false;
	}
	list.push_back(std::move(record));
	ranges::sort(list, ranges::less(), &EditRecord::replacedAt);
	if (list.size() > kMaxEditsPerMessage) {
		list.erase(list.begin());
	}
	archive.changed(peerId);
	return true;
}

QByteArray SerializePhoto(not_null<PhotoData*> photo) {
	auto access = uint64();
	auto fileReference = QByteArray();
	const auto input = photo->mtpInput();
	if (input.type() == mtpc_inputPhoto) {
		const auto &data = input.c_inputPhoto();
		access = data.vaccess_hash().v;
		fileReference = data.vfile_reference().v;
	}
	const auto &large = photo->location(Data::PhotoSize::Large);
	const auto storage = std::get_if<StorageFileLocation>(&large.file().data);
	const auto dc = storage ? storage->dcId() : 0;
	auto result = QByteArray();
	{
		QDataStream stream(&result, QIODevice::WriteOnly);
		stream.setVersion(QDataStream::Qt_5_1);
		stream
			<< kMediaPhoto
			<< qint32(AppVersion)
			<< quint64(photo->id)
			<< quint64(access)
			<< fileReference
			<< qint32(photo->date())
			<< qint32(dc)
			<< qint32(photo->hasAttachedStickers() ? 1 : 0)
			<< photo->inlineThumbnailBytes();
		for (const auto size : {
				Data::PhotoSize::Small,
				Data::PhotoSize::Thumbnail,
				Data::PhotoSize::Large }) {
			stream
				<< photo->location(size).serialize()
				<< qint32(photo->imageByteSize(size));
		}
	}
	return result;
}

QByteArray SerializeDocument(not_null<DocumentData*> document) {
	auto result = QByteArray();
	{
		QDataStream stream(&result, QIODevice::WriteOnly);
		stream.setVersion(QDataStream::Qt_5_1);
		stream << kMediaDocument << qint32(AppVersion);
		Serialize::Document::writeToStream(stream, document);
	}
	return result;
}

QString DeletedMediaFolder() {
	return cWorkingDir() + u"TGPlus deleted media/"_q;
}

} // namespace Tgplus
