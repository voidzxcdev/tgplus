/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tgplus/tgplus_sync.h"

#include "api/api_text_entities.h"
#include "apiwrap.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "data/data_channel.h"
#include "data/data_document.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "storage/storage_account.h"
#include "tgplus/tgplus_archive.h"
#include "tgplus/tgplus_settings.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLocale>

namespace Tgplus {
namespace {

constexpr auto kLastIdPref = "tgplus_recorder_last_id";
constexpr auto kPageLimit = 100;
constexpr auto kMaxChunks = 64;
constexpr auto kInitialDelay = crl::time(8000);
constexpr auto kPeriod = crl::time(10 * 60 * 1000);
constexpr auto kNewItemDelay = crl::time(2500);

[[nodiscard]] QString ChannelTitle() {
	return u"TGPlus Sync"_q;
}

[[nodiscard]] QString ChunkPrefix() {
	return u"TGPLUS1 "_q;
}

[[nodiscard]] QString MediaPrefix() {
	return u"TGPLUS1M "_q;
}

struct ParsedChunk {
	QString key;
	int index = 0;
	int count = 0;
	QString payload;
};

struct PendingChunks {
	int count = 0;
	base::flat_map<int, QString> parts;
};

// Chunk format: "TGPLUS1 <key> <index>/<count>\n<json part>".
[[nodiscard]] std::optional<ParsedChunk> ParseChunk(const QString &text) {
	if (!text.startsWith(ChunkPrefix())) {
		return std::nullopt;
	}
	const auto newline = text.indexOf(QChar('\n'));
	if (newline < 0) {
		return std::nullopt;
	}
	const auto start = ChunkPrefix().size();
	const auto header = text.mid(start, newline - start).split(
		QChar(' '),
		Qt::SkipEmptyParts);
	if (header.size() != 2) {
		return std::nullopt;
	}
	const auto position = header[1].split(QChar('/'));
	if (position.size() != 2) {
		return std::nullopt;
	}
	auto result = ParsedChunk{
		.key = header[0],
		.index = position[0].toInt(),
		.count = position[1].toInt(),
		.payload = text.mid(newline + 1),
	};
	if (result.count < 1
		|| result.count > kMaxChunks
		|| result.index < 1
		|| result.index > result.count) {
		return std::nullopt;
	}
	return result;
}

[[nodiscard]] PeerId ParsePeer(const QJsonValue &value) {
	const auto list = value.toArray();
	if (list.size() != 2) {
		return PeerId();
	}
	const auto type = list.at(0).toString();
	const auto id = BareId(list.at(1).toDouble());
	if (!id) {
		return PeerId();
	} else if (type == u"u"_q) {
		return peerFromUser(UserId(id));
	} else if (type == u"c"_q) {
		return peerFromChat(ChatId(id));
	} else if (type == u"ch"_q) {
		return peerFromChannel(ChannelId(id));
	}
	return PeerId();
}

[[nodiscard]] QVector<MTPMessageEntity> ParseEntities(const QJsonArray &list) {
	auto result = QVector<MTPMessageEntity>();
	for (const auto &value : list) {
		const auto entity = value.toObject();
		const auto type = entity.value(u"_"_q).toString();
		const auto offset = MTP_int(entity.value(u"o"_q).toInt());
		const auto length = MTP_int(entity.value(u"l"_q).toInt());
		const auto string = [&](const QString &key) {
			return MTP_string(entity.value(key).toString());
		};
		const auto bigId = [&](const QString &key) {
			return MTP_long(entity.value(key).toString().toLongLong());
		};
		if (type == u"bold"_q) {
			result.push_back(MTP_messageEntityBold(offset, length));
		} else if (type == u"italic"_q) {
			result.push_back(MTP_messageEntityItalic(offset, length));
		} else if (type == u"underline"_q) {
			result.push_back(MTP_messageEntityUnderline(offset, length));
		} else if (type == u"strike"_q) {
			result.push_back(MTP_messageEntityStrike(offset, length));
		} else if (type == u"spoiler"_q) {
			result.push_back(MTP_messageEntitySpoiler(offset, length));
		} else if (type == u"code"_q) {
			result.push_back(MTP_messageEntityCode(offset, length));
		} else if (type == u"pre"_q) {
			result.push_back(MTP_messageEntityPre(
				offset,
				length,
				string(u"lang"_q)));
		} else if (type == u"url"_q) {
			result.push_back(MTP_messageEntityUrl(offset, length));
		} else if (type == u"texturl"_q) {
			result.push_back(MTP_messageEntityTextUrl(
				offset,
				length,
				string(u"url"_q)));
		} else if (type == u"email"_q) {
			result.push_back(MTP_messageEntityEmail(offset, length));
		} else if (type == u"mention"_q) {
			result.push_back(MTP_messageEntityMention(offset, length));
		} else if (type == u"mentionname"_q) {
			result.push_back(MTP_messageEntityMentionName(
				offset,
				length,
				bigId(u"uid"_q)));
		} else if (type == u"hashtag"_q) {
			result.push_back(MTP_messageEntityHashtag(offset, length));
		} else if (type == u"cashtag"_q) {
			result.push_back(MTP_messageEntityCashtag(offset, length));
		} else if (type == u"botcommand"_q) {
			result.push_back(MTP_messageEntityBotCommand(offset, length));
		} else if (type == u"phone"_q) {
			result.push_back(MTP_messageEntityPhone(offset, length));
		} else if (type == u"customemoji"_q) {
			result.push_back(MTP_messageEntityCustomEmoji(
				offset,
				length,
				bigId(u"doc"_q)));
		} else if (type == u"blockquote"_q) {
			using Flag = MTPDmessageEntityBlockquote::Flag;
			result.push_back(MTP_messageEntityBlockquote(
				MTP_flags(entity.value(u"c"_q).toBool()
					? Flag::f_collapsed
					: Flag(0)),
				offset,
				length));
		}
	}
	return result;
}

[[nodiscard]] TextWithEntities ParseText(
		not_null<Main::Session*> session,
		const QJsonObject &object) {
	return {
		object.value(u"text"_q).toString(),
		Api::EntitiesFromMTP(
			session,
			ParseEntities(object.value(u"ent"_q).toArray())),
	};
}

[[nodiscard]] QByteArray SerializeMedia(
		not_null<Main::Session*> session,
		const MTPMessageMedia &media) {
	auto &owner = session->data();
	if (media.type() == mtpc_messageMediaPhoto) {
		if (const auto photo = media.c_messageMediaPhoto().vphoto()) {
			return SerializePhoto(owner.processPhoto(*photo));
		}
	} else if (media.type() == mtpc_messageMediaDocument) {
		const auto &data = media.c_messageMediaDocument();
		if (const auto document = data.vdocument()) {
			return SerializeDocument(owner.processDocument(*document));
		}
	}
	return QByteArray();
}

[[nodiscard]] MsgId ReadMsgId(const QJsonObject &object, const QString &key) {
	return MsgId(int64(object.value(key).toDouble()));
}

[[nodiscard]] DeletedRecord ParseDeleted(
		not_null<Main::Session*> session,
		const QJsonObject &object,
		QByteArray media) {
	auto result = DeletedRecord();
	result.id = ReadMsgId(object, u"id"_q);
	result.date = TimeId(object.value(u"date"_q).toDouble());
	result.from = ParsePeer(object.value(u"from"_q));
	result.fromName = object.value(u"fn"_q).toString();
	result.postAuthor = object.value(u"pa"_q).toString();
	result.replyTo = ReadMsgId(object, u"rt"_q);
	result.topicRootId = ReadMsgId(object, u"tr"_q);
	result.text = ParseText(session, object);
	result.mediaLabel = object.value(u"ml"_q).toString();
	result.media = std::move(media);
	result.deletedAt = TimeId(object.value(u"at"_q).toDouble());
	result.out = object.value(u"out"_q).toBool();
	result.post = object.value(u"post"_q).toBool();
	return result;
}

class RecorderSync final : public base::has_weak_ptr {
public:
	explicit RecorderSync(not_null<Main::Session*> session);
	~RecorderSync();

	void start();
	void syncNow();
	[[nodiscard]] rpl::producer<QString> status() const;

private:
	void findChannel(int folderId);
	void requestNext();
	void applyMessages(const QVector<MTPMessage> &messages);
	void applyPayload(const QString &payload);
	void finish(const QString &status);

	const not_null<Main::Session*> _session;
	ChannelData *_channel = nullptr;
	MsgId _lastId = 0;
	MsgId _cursor = 0;
	mtpRequestId _requestId = 0;
	bool _started = false;
	bool _syncing = false;
	int _imported = 0;
	base::Timer _periodic;
	base::Timer _soon;
	rpl::variable<QString> _status;
	base::flat_map<QString, PendingChunks> _chunks;
	base::flat_map<QString, MTPMessageMedia> _media;
	rpl::lifetime _lifetime;

};

RecorderSync::RecorderSync(not_null<Main::Session*> session)
: _session(session)
, _periodic([=] { syncNow(); })
, _soon([=] { syncNow(); }) {
}

RecorderSync::~RecorderSync() {
	_session->api().request(base::take(_requestId)).cancel();
}

void RecorderSync::start() {
	if (_started) {
		return;
	}
	_started = true;
	_lastId = MsgId(
		_session->local().readPref<QString>(kLastIdPref).toLongLong());
	_cursor = _lastId;
	_periodic.callEach(kPeriod);
	_soon.callOnce(kInitialDelay);

	_session->data().newItemAdded(
	) | rpl::filter([=](not_null<HistoryItem*> item) {
		return _channel && (item->history()->peer->id == _channel->id);
	}) | rpl::on_next([=] {
		if (!_soon.isActive()) {
			_soon.callOnce(kNewItemDelay);
		}
	}, _lifetime);
}

void RecorderSync::syncNow() {
	if (_syncing || !Enabled(Option::RecorderSync)) {
		return;
	}
	_syncing = true;
	_imported = 0;
	_status = tr::lng_tgplus_sync_running(tr::now);
	if (_channel) {
		requestNext();
	} else {
		findChannel(1);
	}
}

rpl::producer<QString> RecorderSync::status() const {
	return _status.value();
}

void RecorderSync::findChannel(int folderId) {
	using Flag = MTPmessages_GetDialogs::Flag;
	_requestId = _session->api().request(MTPmessages_GetDialogs(
		MTP_flags(Flag::f_folder_id),
		MTP_int(folderId),
		MTP_int(0),
		MTP_int(0),
		MTP_inputPeerEmpty(),
		MTP_int(kPageLimit),
		MTP_long(0)
	)).done(crl::guard(this, [=](const MTPmessages_Dialogs &result) {
		_requestId = 0;
		const auto chats = result.match([](
				const MTPDmessages_dialogsNotModified &) {
			return QVector<MTPChat>();
		}, [&](const auto &data) {
			_session->data().processUsers(data.vusers());
			_session->data().processChats(data.vchats());
			return data.vchats().v;
		});
		for (const auto &chat : chats) {
			if (chat.type() != mtpc_channel) {
				continue;
			}
			const auto channel = _session->data().channelLoaded(
				ChannelId(chat.c_channel().vid().v));
			if (channel
				&& channel->amCreator()
				&& channel->name() == ChannelTitle()) {
				_channel = channel;
				break;
			}
		}
		if (_channel) {
			requestNext();
		} else if (folderId) {
			findChannel(0);
		} else {
			finish(tr::lng_tgplus_sync_no_channel(tr::now));
		}
	})).fail(crl::guard(this, [=](const MTP::Error &error) {
		_requestId = 0;
		finish(error.type());
	})).send();
}

void RecorderSync::requestNext() {
	_requestId = _session->api().request(MTPmessages_GetHistory(
		_channel->input(),
		MTP_int(int(_cursor.bare) + 1),
		MTP_int(0),
		MTP_int(-kPageLimit),
		MTP_int(kPageLimit),
		MTP_int(0),
		MTP_int(int(_cursor.bare)),
		MTP_long(0)
	)).done(crl::guard(this, [=](const MTPmessages_Messages &result) {
		_requestId = 0;
		const auto messages = result.match([](
				const MTPDmessages_messagesNotModified &) {
			return QVector<MTPMessage>();
		}, [&](const auto &data) {
			_session->data().processUsers(data.vusers());
			_session->data().processChats(data.vchats());
			return data.vmessages().v;
		});
		const auto was = _cursor;
		applyMessages(messages);
		if (messages.size() >= kPageLimit && _cursor > was) {
			requestNext();
		} else {
			finish(tr::lng_tgplus_sync_done(
				tr::now,
				lt_time,
				QLocale().toString(QTime::currentTime(), QLocale::ShortFormat),
				lt_amount,
				QString::number(_imported)));
		}
	})).fail(crl::guard(this, [=](const MTP::Error &error) {
		_requestId = 0;
		finish(error.type());
	})).send();
}

void RecorderSync::applyMessages(const QVector<MTPMessage> &messages) {
	auto list = std::vector<const MTPDmessage*>();
	for (const auto &message : messages) {
		const auto id = message.match([](const auto &data) {
			return MsgId(data.vid().v);
		});
		_cursor = std::max(_cursor, id);
		if (message.type() == mtpc_message) {
			list.push_back(&message.c_message());
		}
	}
	ranges::sort(list, ranges::less(), [](const MTPDmessage *data) {
		return data->vid().v;
	});
	for (const auto data : list) {
		const auto text = qs(data->vmessage());
		if (text.startsWith(MediaPrefix())) {
			if (const auto media = data->vmedia()) {
				const auto key = text.mid(MediaPrefix().size()).trimmed();
				_media.remove(key);
				_media.emplace(key, *media);
			}
		} else if (const auto chunk = ParseChunk(text)) {
			auto &pending = _chunks[chunk->key];
			pending.count = chunk->count;
			pending.parts[chunk->index] = chunk->payload;
			if (int(pending.parts.size()) < pending.count) {
				continue;
			}
			auto payload = QString();
			for (const auto &[index, part] : pending.parts) {
				payload += part;
			}
			_chunks.remove(chunk->key);
			applyPayload(payload);
		}
	}
}

void RecorderSync::applyPayload(const QString &payload) {
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(payload.toUtf8(), &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		LOG(("TGPlus: bad recorder record."));
		return;
	}
	const auto object = document.object();
	const auto peerId = ParsePeer(object.value(u"p"_q));
	if (!peerId) {
		return;
	}
	const auto type = object.value(u"t"_q).toString();
	if (type == u"del"_q) {
		auto media = QByteArray();
		const auto key = object.value(u"mkey"_q).toString();
		if (!key.isEmpty()) {
			if (const auto i = _media.find(key); i != end(_media)) {
				media = SerializeMedia(_session, i->second);
				_media.erase(i);
			}
		}
		auto record = ParseDeleted(_session, object, std::move(media));
		if (ImportDeleted(_session, peerId, std::move(record))) {
			++_imported;
		}
	} else if (type == u"edit"_q) {
		auto record = EditRecord{
			.text = ParseText(_session, object),
			.replacedAt = TimeId(object.value(u"at"_q).toDouble()),
		};
		const auto id = ReadMsgId(object, u"id"_q);
		if (ImportEdit(_session, peerId, id, std::move(record))) {
			++_imported;
		}
	}
}

void RecorderSync::finish(const QString &status) {
	_syncing = false;
	if (_cursor > _lastId) {
		_lastId = _cursor;
		_session->local().writePref<QString>(
			kLastIdPref,
			QString::number(_lastId.bare));
	}
	_status = status;
}

[[nodiscard]] std::map<
	not_null<Main::Session*>,
	std::unique_ptr<RecorderSync>> &Syncs() {
	// WHY: never destroyed, each sync is removed together with its session.
	static const auto result = new std::map<
		not_null<Main::Session*>,
		std::unique_ptr<RecorderSync>>();
	return *result;
}

[[nodiscard]] RecorderSync &SyncFor(not_null<Main::Session*> session) {
	auto &map = Syncs();
	auto i = map.find(session);
	if (i == end(map)) {
		i = map.emplace(
			session,
			std::make_unique<RecorderSync>(session)).first;
		session->lifetime().add([=] {
			Syncs().erase(session);
		});
	}
	return *i->second;
}

} // namespace

void StartRecorderSync(not_null<Main::Session*> session) {
	SyncFor(session).start();
}

void SyncRecorderNow(not_null<Main::Session*> session) {
	auto &sync = SyncFor(session);
	sync.start();
	sync.syncNow();
}

rpl::producer<QString> RecorderSyncStatus(not_null<Main::Session*> session) {
	return SyncFor(session).status();
}

} // namespace Tgplus
