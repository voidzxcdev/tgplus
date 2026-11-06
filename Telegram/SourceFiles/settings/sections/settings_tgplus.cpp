/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/sections/settings_tgplus.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "lang/lang_keys.h"
#include "settings/sections/settings_main.h"
#include "settings/settings_builder.h"
#include "settings/settings_common_session.h"
#include "tgplus/tgplus_archive.h"
#include "tgplus/tgplus_settings.h"
#include "tgplus/tgplus_sync.h"
#include "tgplus/tgplus_ui.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

#include <QtCore/QDir>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>

namespace Settings {
namespace {

using namespace Builder;
using Tgplus::Option;

struct ToggleArgs {
	Option option = Option::GhostMode;
	QString searchId;
	rpl::producer<QString> title;
	QStringList keywords;
	Fn<void(bool)> changed;
};

void AddToggle(SectionBuilder &builder, ToggleArgs &&args) {
	const auto option = args.option;
	const auto button = builder.addButton({
		.id = std::move(args.searchId),
		.title = std::move(args.title),
		.st = &st::settingsButtonNoIcon,
		.toggled = Tgplus::Value(option),
		.keywords = std::move(args.keywords),
	});
	if (!button) {
		return;
	}
	const auto changed = std::move(args.changed);
	button->toggledChanges(
	) | rpl::filter([=](bool toggled) {
		return (toggled != Tgplus::Enabled(option));
	}) | rpl::on_next([=](bool toggled) {
		Tgplus::SetEnabled(option, toggled);
		if (changed) {
			changed(toggled);
		}
	}, button->lifetime());
}

void BuildArchiveOptions(SectionBuilder &builder) {
	const auto controller = builder.controller();
	builder.addSkip();
	builder.addSubsectionTitle({
		.id = u"tgplus/archive"_q,
		.title = tr::lng_tgplus_settings_archive(),
	});
	const auto keep = builder.addButton({
		.id = u"tgplus/keep-deleted"_q,
		.title = tr::lng_tgplus_settings_keep_deleted(),
		.st = &st::settingsButtonNoIcon,
		.toggled = rpl::single(
			Core::App().settings().keepDeletedMessages()),
		.keywords = { u"deleted"_q, u"anti"_q, u"delete"_q },
	});
	if (keep) {
		keep->toggledChanges(
		) | rpl::filter([](bool toggled) {
			return (toggled != Core::App().settings().keepDeletedMessages());
		}) | rpl::on_next([](bool toggled) {
			Core::App().settings().setKeepDeletedMessages(toggled);
		}, keep->lifetime());
	}
	AddToggle(builder, {
		.option = Option::EditHistory,
		.searchId = u"tgplus/edit-history"_q,
		.title = tr::lng_tgplus_settings_edit_history(),
		.keywords = { u"edit"_q, u"history"_q, u"versions"_q },
	});
	AddToggle(builder, {
		.option = Option::SaveDeletedMedia,
		.searchId = u"tgplus/deleted-media"_q,
		.title = tr::lng_tgplus_settings_save_media(),
		.keywords = { u"media"_q, u"photo"_q, u"file"_q, u"deleted"_q },
	});
	builder.addButton({
		.id = u"tgplus/deleted-log"_q,
		.title = tr::lng_tgplus_log_title(),
		.st = &st::settingsButton,
		.icon = { &st::menuIconDelete },
		.onClick = [=] {
			if (controller) {
				controller->show(Box(
					Tgplus::DeletedLogBox,
					controller,
					static_cast<PeerData*>(nullptr)));
			}
		},
		.keywords = { u"deleted"_q, u"log"_q, u"search"_q },
	});
	builder.addButton({
		.id = u"tgplus/media-folder"_q,
		.title = tr::lng_tgplus_log_media_folder(),
		.st = &st::settingsButton,
		.icon = { &st::menuIconShowInFolder },
		.onClick = [] {
			const auto folder = Tgplus::DeletedMediaFolder();
			QDir().mkpath(folder);
			QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
		},
		.keywords = { u"folder"_q, u"media"_q },
	});
	builder.addSkip();
	builder.addDividerText(tr::lng_tgplus_settings_archive_about());
}

void BuildRecorderOptions(SectionBuilder &builder) {
	const auto session = builder.session();
	builder.addSkip();
	builder.addSubsectionTitle({
		.id = u"tgplus/recorder"_q,
		.title = tr::lng_tgplus_settings_recorder(),
	});
	AddToggle(builder, {
		.option = Option::RecorderSync,
		.searchId = u"tgplus/recorder-sync"_q,
		.title = tr::lng_tgplus_settings_recorder_sync(),
		.keywords = { u"server"_q, u"render"_q, u"recorder"_q },
		.changed = [=](bool enabled) {
			if (enabled) {
				Tgplus::SyncRecorderNow(session);
			}
		},
	});
	builder.addButton({
		.id = u"tgplus/sync-now"_q,
		.title = tr::lng_tgplus_settings_sync_now(),
		.st = &st::settingsButtonNoIcon,
		.label = rpl::combine(
			Tgplus::RecorderSyncStatus(session),
			tr::lng_tgplus_sync_never()
		) | rpl::map([](const QString &status, const QString &never) {
			return status.isEmpty() ? never : status;
		}),
		.onClick = [=] { Tgplus::SyncRecorderNow(session); },
		.keywords = { u"sync"_q },
		.shown = Tgplus::Value(Option::RecorderSync),
	});
	builder.addSkip();
	builder.addDividerText(tr::lng_tgplus_settings_recorder_about());
}

void BuildGhostOptions(SectionBuilder &builder) {
	builder.addSkip();
	builder.addSubsectionTitle({
		.id = u"tgplus/ghost"_q,
		.title = tr::lng_tgplus_settings_ghost(),
	});
	AddToggle(builder, {
		.option = Option::GhostMode,
		.searchId = u"tgplus/ghost-mode"_q,
		.title = tr::lng_tgplus_settings_ghost_mode(),
		.keywords = { u"ghost"_q, u"stealth"_q, u"invisible"_q },
	});
	builder.scope([&] {
		AddToggle(builder, {
			.option = Option::GhostHideReads,
			.searchId = u"tgplus/ghost-reads"_q,
			.title = tr::lng_tgplus_settings_ghost_reads(),
			.keywords = { u"read"_q, u"receipts"_q },
		});
		AddToggle(builder, {
			.option = Option::GhostHideOnline,
			.searchId = u"tgplus/ghost-online"_q,
			.title = tr::lng_tgplus_settings_ghost_online(),
			.keywords = { u"online"_q, u"last seen"_q },
		});
		AddToggle(builder, {
			.option = Option::GhostHideTyping,
			.searchId = u"tgplus/ghost-typing"_q,
			.title = tr::lng_tgplus_settings_ghost_typing(),
			.keywords = { u"typing"_q },
		});
		AddToggle(builder, {
			.option = Option::GhostHideStories,
			.searchId = u"tgplus/ghost-stories"_q,
			.title = tr::lng_tgplus_settings_ghost_stories(),
			.keywords = { u"stories"_q, u"views"_q },
		});
	}, Tgplus::Value(Option::GhostMode));
	builder.addSkip();
	builder.addDividerText(tr::lng_tgplus_settings_ghost_about());
}

void BuildPrivacyOptions(SectionBuilder &builder) {
	builder.addSkip();
	builder.addSubsectionTitle({
		.id = u"tgplus/privacy"_q,
		.title = tr::lng_tgplus_settings_privacy(),
	});
	AddToggle(builder, {
		.option = Option::StreamerMode,
		.searchId = u"tgplus/streamer"_q,
		.title = tr::lng_tgplus_settings_streamer(),
		.keywords = { u"streamer"_q, u"phone"_q, u"hide"_q },
	});
	AddToggle(builder, {
		.option = Option::ScreenShield,
		.searchId = u"tgplus/shield"_q,
		.title = tr::lng_tgplus_settings_shield(),
		.keywords = { u"screenshot"_q, u"screen"_q, u"capture"_q },
	});
	builder.addSkip();
	builder.addDividerText(tr::lng_tgplus_settings_privacy_about());
}

void BuildMessagesOptions(SectionBuilder &builder) {
	const auto controller = builder.controller();
	builder.addSkip();
	builder.addSubsectionTitle({
		.id = u"tgplus/messages"_q,
		.title = tr::lng_tgplus_settings_messages(),
	});
	AddToggle(builder, {
		.option = Option::ShowSeconds,
		.searchId = u"tgplus/seconds"_q,
		.title = tr::lng_tgplus_settings_seconds(),
		.keywords = { u"seconds"_q, u"time"_q },
	});
	AddToggle(builder, {
		.option = Option::KeywordAlerts,
		.searchId = u"tgplus/keyword-alerts"_q,
		.title = tr::lng_tgplus_settings_keyword_alerts(),
		.keywords = { u"keyword"_q, u"alert"_q, u"mute"_q },
	});
	builder.addButton({
		.id = u"tgplus/keywords"_q,
		.title = tr::lng_tgplus_keywords_title(),
		.st = &st::settingsButtonNoIcon,
		.label = Tgplus::AlertKeywordsValue(
		) | rpl::map([](const QStringList &keywords) {
			return keywords.isEmpty()
				? tr::lng_tgplus_keywords_none(tr::now)
				: QString::number(keywords.size());
		}),
		.onClick = [=] {
			if (controller) {
				controller->show(Box(Tgplus::AlertKeywordsBox));
			}
		},
		.keywords = { u"keywords"_q },
	});
	builder.addSkip();
	builder.addDividerText(tr::lng_tgplus_settings_messages_about());
}

void BuildTgplusContent(SectionBuilder &builder) {
	BuildArchiveOptions(builder);
	BuildRecorderOptions(builder);
	BuildGhostOptions(builder);
	BuildPrivacyOptions(builder);
	BuildMessagesOptions(builder);
}

class TgplusFeatures : public Section<TgplusFeatures> {
public:
	TgplusFeatures(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();

};

const auto kMeta = BuildHelper({
	.id = TgplusFeatures::Id(),
	.parentId = MainId(),
	.title = &tr::lng_tgplus_settings_title,
	.icon = &st::menuIconStealth,
}, [](SectionBuilder &builder) {
	BuildTgplusContent(builder);
});

const SectionBuildMethod kTgplusSection = kMeta.build;

TgplusFeatures::TgplusFeatures(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

rpl::producer<QString> TgplusFeatures::title() {
	return tr::lng_tgplus_settings_title();
}

void TgplusFeatures::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

	build(content, kTgplusSection);

	Ui::ResizeFitChild(this, content);
}

} // namespace

Type TgplusId() {
	return TgplusFeatures::Id();
}

} // namespace Settings
