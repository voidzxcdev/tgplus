/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class History;
class HistoryItem;
class PeerData;
class PhotoData;
class DocumentData;

namespace Main {
class Session;
} // namespace Main

namespace Tgplus {

struct DeletedRecord {
	MsgId id = 0;
	TimeId date = 0;
	PeerId from = 0;
	QString fromName;
	QString postAuthor;
	MsgId replyTo = 0;
	MsgId topicRootId = 0;
	TextWithEntities text;
	QString mediaLabel;
	QByteArray media;
	QString savedFile;
	TimeId deletedAt = 0;
	bool out = false;
	bool post = false;
	bool fromServer = false;
};

struct EditRecord {
	TextWithEntities text;
	TimeId replacedAt = 0;
};

struct LogEntry {
	PeerId peer = 0;
	DeletedRecord record;
};

void RememberDeleted(not_null<HistoryItem*> item);
void RememberEdit(
	not_null<HistoryItem*> item,
	const TextWithEntities &updated);
void ForgetDeleted(not_null<HistoryItem*> item);

// Messages deleted from this app never come back from the recorder.
void DismissDeleted(not_null<History*> history, const QVector<MTPint> &ids);

[[nodiscard]] std::vector<EditRecord> EditsFor(not_null<HistoryItem*> item);
[[nodiscard]] bool HasDeleted(not_null<PeerData*> peer);
[[nodiscard]] std::vector<LogEntry> DeletedLog(
	not_null<Main::Session*> session,
	PeerData *peer);

// Recreates archived messages with ids strictly between after and before.
[[nodiscard]] std::vector<not_null<HistoryItem*>> RestoreDeleted(
	not_null<History*> history,
	MsgId after,
	MsgId before);

// Records captured by the server recorder; returns false for duplicates.
bool ImportDeleted(
	not_null<Main::Session*> session,
	PeerId peer,
	DeletedRecord record);
bool ImportEdit(
	not_null<Main::Session*> session,
	PeerId peer,
	MsgId id,
	EditRecord record);

[[nodiscard]] QByteArray SerializePhoto(not_null<PhotoData*> photo);
[[nodiscard]] QByteArray SerializeDocument(not_null<DocumentData*> document);
[[nodiscard]] QString DeletedMediaFolder();

} // namespace Tgplus
