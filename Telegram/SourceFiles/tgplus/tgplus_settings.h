/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class HistoryItem;

namespace Tgplus {

enum class Option : uchar {
	GhostMode,
	GhostHideReads,
	GhostHideOnline,
	GhostHideTyping,
	GhostHideStories,
	ShowSeconds,
	StreamerMode,
	ScreenShield,
	KeywordAlerts,
	SaveDeletedMedia,
	RecorderSync,
	EditHistory,

	kCount,
};

[[nodiscard]] bool Enabled(Option option);
[[nodiscard]] rpl::producer<bool> Value(Option option);
void SetEnabled(Option option, bool enabled);

// True when ghost mode is on and hides the given kind of activity.
[[nodiscard]] bool GhostHides(Option what);

[[nodiscard]] QStringList AlertKeywords();
[[nodiscard]] rpl::producer<QStringList> AlertKeywordsValue();
void SetAlertKeywords(const QStringList &keywords);
[[nodiscard]] bool MatchesAlertKeywords(not_null<HistoryItem*> item);

[[nodiscard]] QString FormatMessageTime(const QTime &time);

} // namespace Tgplus
