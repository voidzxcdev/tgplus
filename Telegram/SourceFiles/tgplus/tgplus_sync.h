/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Main {
class Session;
} // namespace Main

namespace Tgplus {

// Imports what the server recorder captured while this app was closed.
void StartRecorderSync(not_null<Main::Session*> session);
void SyncRecorderNow(not_null<Main::Session*> session);
[[nodiscard]] rpl::producer<QString> RecorderSyncStatus(
	not_null<Main::Session*> session);

} // namespace Tgplus
