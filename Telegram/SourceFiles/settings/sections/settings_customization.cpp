/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/sections/settings_customization.h"

#include "base/options.h"
#include "core/application.h"
#include "dialogs/dialogs_widget.h"
#include "dialogs/ui/dialogs_layout.h"
#include "history/view/history_view_message.h"
#include "info/profile/info_profile_actions.h"
#include "lang/lang_keys.h"
#include "settings/sections/settings_chat.h"
#include "settings/sections/settings_main.h"
#include "settings/settings_builder.h"
#include "settings/settings_common_session.h"
#include "ui/boxes/confirm_box.h"
#include "ui/chat/chat_style_radius.h"
#include "ui/controls/compose_ai_button_factory.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace Settings {
namespace {

using namespace Builder;

struct OptionToggleArgs {
	const char *optionId = nullptr;
	QString searchId;
	rpl::producer<QString> title;
	QStringList keywords;
};

// Toggle bound to an existing base::options flag. The value is saved by
// base::options itself; options that only apply on startup offer a
// restart right away instead of pretending to apply live.
void AddOptionToggle(SectionBuilder &builder, OptionToggleArgs &&args) {
	auto &option = base::options::lookup<bool>(args.optionId);
	const auto button = builder.addButton({
		.id = args.searchId,
		.title = std::move(args.title),
		.st = &st::settingsButtonNoIcon,
		.toggled = rpl::single(option.value()),
		.keywords = std::move(args.keywords),
	});
	if (!button) {
		return;
	}
	const auto controller = builder.controller();
	const auto optionPtr = &option;
	button->toggledChanges(
	) | rpl::filter([=](bool toggled) {
		return (toggled != optionPtr->value());
	}) | rpl::on_next([=](bool toggled) {
		optionPtr->set(toggled);
		if (optionPtr->restartRequired() && controller) {
			controller->show(Ui::MakeConfirmBox({
				.text = tr::lng_settings_need_restart(),
				.confirmed = [] { Core::Restart(); },
				.confirmText = tr::lng_settings_restart_now(),
				.cancelText = tr::lng_settings_restart_later(),
			}));
		}
	}, button->lifetime());
}

void BuildChatListOptions(SectionBuilder &builder) {
	builder.addSkip();
	builder.addSubsectionTitle({
		.id = u"customization/chat-list"_q,
		.title = tr::lng_tgplus_custom_chat_list(),
	});
	AddOptionToggle(builder, {
		.optionId = Dialogs::kOptionHideStoriesBar,
		.searchId = u"customization/hide-stories"_q,
		.title = tr::lng_tgplus_custom_hide_stories(),
		.keywords = { u"stories"_q, u"hide"_q },
	});
	AddOptionToggle(builder, {
		.optionId = Dialogs::Ui::kOptionDialogsMuteIcon,
		.searchId = u"customization/mute-icon"_q,
		.title = tr::lng_tgplus_custom_mute_icon(),
		.keywords = { u"mute"_q, u"icon"_q },
	});
	builder.addSkip();
}

void BuildMessagesOptions(SectionBuilder &builder) {
	builder.addDivider();
	builder.addSkip();
	builder.addSubsectionTitle({
		.id = u"customization/messages"_q,
		.title = tr::lng_tgplus_custom_messages(),
	});
	AddOptionToggle(builder, {
		.optionId = Ui::kOptionUseSmallMsgBubbleRadius,
		.searchId = u"customization/small-radius"_q,
		.title = tr::lng_tgplus_custom_small_radius(),
		.keywords = { u"bubble"_q, u"radius"_q, u"corners"_q },
	});
	AddOptionToggle(builder, {
		.optionId = HistoryView::kOptionUnlimitedMessageWidth,
		.searchId = u"customization/wide-messages"_q,
		.title = tr::lng_tgplus_custom_wide_messages(),
		.keywords = { u"width"_q, u"wide"_q },
	});
	AddOptionToggle(builder, {
		.optionId = Ui::kOptionHideAiButton,
		.searchId = u"customization/hide-ai"_q,
		.title = tr::lng_tgplus_custom_hide_ai(),
		.keywords = { u"ai"_q, u"button"_q },
	});
	builder.addSkip();
	builder.addDividerText(tr::lng_tgplus_custom_restart_note());
}

void BuildProfileOptions(SectionBuilder &builder) {
	builder.addSkip();
	builder.addSubsectionTitle({
		.id = u"customization/profiles"_q,
		.title = tr::lng_tgplus_custom_profiles(),
	});
	AddOptionToggle(builder, {
		.optionId = Info::Profile::kOptionShowPeerIdBelowAbout,
		.searchId = u"customization/peer-id"_q,
		.title = tr::lng_tgplus_custom_peer_id(),
		.keywords = { u"id"_q },
	});
	AddOptionToggle(builder, {
		.optionId = Info::Profile::kOptionShowChannelJoinedBelowAbout,
		.searchId = u"customization/joined-date"_q,
		.title = tr::lng_tgplus_custom_joined_date(),
		.keywords = { u"joined"_q, u"date"_q },
	});
	builder.addSkip();
}

void BuildAppearanceShortcuts(SectionBuilder &builder) {
	builder.addSkip();
	builder.addButton({
		.id = u"customization/theme"_q,
		.title = tr::lng_tgplus_custom_theme(),
		.st = &st::settingsButton,
		.icon = { &st::menuIconPalette },
		.onClick = [showOther = builder.showOther()] {
			if (showOther) {
				showOther(ChatId());
			}
		},
		.keywords = { u"accent"_q, u"color"_q, u"theme"_q },
	});
	if (HasInterfaceScale()) {
		builder.add([](const WidgetContext &ctx) {
			const auto window = &ctx.controller->window();
			auto wrap = object_ptr<Ui::VerticalLayout>(ctx.container);
			SetupInterfaceScale(window, wrap.data());
			return SectionBuilder::WidgetToAdd{ .widget = std::move(wrap) };
		}, [] {
			return SearchEntry{
				.id = u"customization/scale"_q,
				.title = tr::lng_settings_default_scale(tr::now),
				.keywords = { u"zoom"_q, u"size"_q, u"scale"_q },
			};
		});
	}
	builder.addSkip();
	builder.addDivider();
}

void BuildCustomizationContent(SectionBuilder &builder) {
	BuildAppearanceShortcuts(builder);
	BuildChatListOptions(builder);
	BuildMessagesOptions(builder);
	BuildProfileOptions(builder);
}

class Customization : public Section<Customization> {
public:
	Customization(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();

};

const auto kMeta = BuildHelper({
	.id = Customization::Id(),
	.parentId = MainId(),
	.title = &tr::lng_tgplus_custom_title,
	.icon = &st::menuIconCustomize,
}, [](SectionBuilder &builder) {
	BuildCustomizationContent(builder);
});

const SectionBuildMethod kCustomizationSection = kMeta.build;

Customization::Customization(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

rpl::producer<QString> Customization::title() {
	return tr::lng_tgplus_custom_title();
}

void Customization::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

	build(content, kCustomizationSection);

	Ui::ResizeFitChild(this, content);
}

} // namespace

Type CustomizationId() {
	return Customization::Id();
}

} // namespace Settings
