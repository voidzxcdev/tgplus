/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tgplus/tgplus_settings.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "history/history_item.h"

#include <QtCore/QLocale>

namespace Tgplus {
namespace {

struct OptionInfo {
	std::string_view key;
	bool fallback = false;
};

constexpr auto kOptions = std::array<OptionInfo, size_t(Option::kCount)>{{
	{ "tgplus_ghost_mode", false },
	{ "tgplus_ghost_reads", true },
	{ "tgplus_ghost_online", true },
	{ "tgplus_ghost_typing", true },
	{ "tgplus_ghost_stories", true },
	{ "tgplus_show_seconds", false },
	{ "tgplus_streamer_mode", false },
	{ "tgplus_screen_shield", false },
	{ "tgplus_keyword_alerts", false },
	{ "tgplus_save_deleted_media", true },
	{ "tgplus_recorder_sync", true },
	{ "tgplus_edit_history", true },
}};

constexpr auto kKeywordsPref = "tgplus_alert_keywords";

struct State {
	std::array<
		std::optional<rpl::variable<bool>>,
		size_t(Option::kCount)> values;
	std::optional<rpl::variable<QStringList>> keywords;
};

State &Cache() {
	// WHY: never destroyed, so no rpl::variable dies during static
	// destruction after the application and its settings are gone.
	static const auto result = new State();
	return *result;
}

rpl::variable<bool> &Variable(Option option) {
	auto &slot = Cache().values[size_t(option)];
	if (!slot) {
		const auto &info = kOptions[size_t(option)];
		slot.emplace(Core::App().settings().readPref<bool>(
			info.key,
			info.fallback));
	}
	return *slot;
}

rpl::variable<QStringList> &Keywords() {
	auto &cached = Cache().keywords;
	if (!cached) {
		const auto stored = Core::App().settings().readPref<QByteArray>(
			kKeywordsPref);
		cached.emplace(QString::fromUtf8(stored).split(
			QChar('\n'),
			Qt::SkipEmptyParts));
	}
	return *cached;
}

} // namespace

bool Enabled(Option option) {
	return Variable(option).current();
}

rpl::producer<bool> Value(Option option) {
	return Variable(option).value();
}

void SetEnabled(Option option, bool enabled) {
	auto &variable = Variable(option);
	if (variable.current() == enabled) {
		return;
	}
	Core::App().settings().writePref<bool>(
		kOptions[size_t(option)].key,
		enabled);
	variable = enabled;
}

bool GhostHides(Option what) {
	return Enabled(Option::GhostMode) && Enabled(what);
}

QStringList AlertKeywords() {
	return Keywords().current();
}

rpl::producer<QStringList> AlertKeywordsValue() {
	return Keywords().value();
}

void SetAlertKeywords(const QStringList &keywords) {
	auto cleaned = QStringList();
	for (const auto &keyword : keywords) {
		const auto trimmed = keyword.trimmed();
		if (!trimmed.isEmpty() && !cleaned.contains(trimmed)) {
			cleaned.push_back(trimmed);
		}
	}
	Core::App().settings().writePref<QByteArray>(
		kKeywordsPref,
		cleaned.join(QChar('\n')).toUtf8());
	Keywords() = cleaned;
}

bool MatchesAlertKeywords(not_null<HistoryItem*> item) {
	if (!Enabled(Option::KeywordAlerts)) {
		return false;
	}
	const auto text = item->originalText().text;
	if (text.isEmpty()) {
		return false;
	}
	for (const auto &keyword : AlertKeywords()) {
		if (text.contains(keyword, Qt::CaseInsensitive)) {
			return true;
		}
	}
	return false;
}

QString FormatMessageTime(const QTime &time) {
	const auto locale = QLocale();
	if (!Enabled(Option::ShowSeconds)) {
		return locale.toString(time, QLocale::ShortFormat);
	}
	auto format = locale.timeFormat(QLocale::ShortFormat);
	if (!format.contains(u"ss"_q)) {
		format.replace(u"mm"_q, u"mm:ss"_q);
	}
	return locale.toString(time, format);
}

} // namespace Tgplus
