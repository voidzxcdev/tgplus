/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tgplus/tgplus_ui.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/unixtime.h"
#include "core/file_utilities.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_schedule_box.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "menu/menu_send.h"
#include "tgplus/tgplus_archive.h"
#include "tgplus/tgplus_settings.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_utilities.h"
#include "ui/vertical_list.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"

#include <QtCore/QDir>
#include <QtCore/QLocale>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

namespace Tgplus {
namespace {

constexpr auto kLogLimit = 300;
constexpr auto kPreviewLength = 400;

[[nodiscard]] QString FormatDateTime(TimeId date) {
	return QLocale().toString(
		base::unixtime::parse(date),
		QLocale::ShortFormat);
}

[[nodiscard]] QString MediaKindLabel(const DeletedRecord &record) {
	if (!record.mediaLabel.isEmpty()) {
		return record.mediaLabel;
	} else if (record.media.isEmpty()) {
		return QString();
	}
	return (record.media.at(0) == 'p')
		? tr::lng_tgplus_log_photo(tr::now)
		: tr::lng_tgplus_log_file(tr::now);
}

[[nodiscard]] QString PeerName(
		not_null<Main::Session*> session,
		PeerId peerId) {
	if (const auto peer = session->data().peerLoaded(peerId)) {
		return peer->isSelf()
			? tr::lng_saved_messages(tr::now)
			: peer->name();
	}
	return tr::lng_tgplus_log_unknown_chat(tr::now);
}

[[nodiscard]] TextWithEntities EntryText(
		not_null<Main::Session*> session,
		const LogEntry &entry) {
	const auto &record = entry.record;
	const auto separator = u" \u00B7 "_q;
	auto header = PeerName(session, entry.peer);
	if (!record.fromName.isEmpty() && record.from != entry.peer) {
		header += separator + record.fromName;
	}
	header += separator + FormatDateTime(record.date);

	auto body = record.text.text;
	if (body.size() > kPreviewLength) {
		body = body.left(kPreviewLength) + QChar(0x2026);
	}
	const auto media = MediaKindLabel(record);
	if (!media.isEmpty()) {
		body = body.isEmpty() ? media : (media + QChar('\n') + body);
	}
	auto result = Ui::Text::Link(header, 1);
	result.append(QChar('\n')).append(body);
	if (record.fromServer) {
		result.append(QChar('\n')).append(
			Ui::Text::Italic(tr::lng_tgplus_log_from_server(tr::now)));
	}
	return result;
}

[[nodiscard]] bool EntryMatches(
		not_null<Main::Session*> session,
		const LogEntry &entry,
		const QString &query) {
	const auto contains = [&](const QString &text) {
		return text.contains(query, Qt::CaseInsensitive);
	};
	return contains(entry.record.text.text)
		|| contains(entry.record.fromName)
		|| contains(entry.record.mediaLabel)
		|| contains(PeerName(session, entry.peer));
}

void ShowRemindBox(
		not_null<Window::SessionController*> controller,
		FullMsgId itemId) {
	const auto session = &controller->session();
	const auto send = [=](Api::SendOptions options) {
		const auto item = session->data().message(itemId);
		if (!item || !options.scheduled) {
			return;
		}
		auto action = Api::SendAction(
			session->data().history(session->user()));
		action.options = options;
		action.clearDraft = false;
		action.generateLocal = false;
		session->api().forwardMessages(
			Data::ResolvedForwardDraft{
				.items = { not_null<HistoryItem*>(item) },
			},
			action,
			crl::guard(controller, [=] {
				controller->showToast(tr::lng_tgplus_remind_done(tr::now));
			}));
	};
	controller->show(HistoryView::PrepareScheduleBox(
		controller,
		controller->uiShow(),
		SendMenu::Details{ .type = SendMenu::Type::Reminder },
		send));
}

} // namespace

void FillMessageMenu(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item) {
	if (!item->isRegular()) {
		return;
	}
	const auto itemId = item->fullId();
	const auto session = &item->history()->session();
	if (!EditsFor(item).empty()) {
		menu->addAction(tr::lng_tgplus_edit_history(tr::now), [=] {
			if (const auto item = session->data().message(itemId)) {
				controller->show(Box(EditHistoryBox, item));
			}
		}, &st::menuIconEdit);
	}
	menu->addAction(tr::lng_tgplus_copy_message_id(tr::now), [=] {
		QGuiApplication::clipboard()->setText(
			QString::number(itemId.msg.bare));
		controller->showToast(tr::lng_tgplus_copied_id(tr::now));
	}, &st::menuIconCopy);
	if (item->isDeletedOnServer()) {
		menu->addAction(tr::lng_tgplus_forget_deleted(tr::now), [=] {
			if (const auto item = session->data().message(itemId)) {
				ForgetDeleted(item);
			}
		}, &st::menuIconDelete);
	} else {
		menu->addAction(tr::lng_tgplus_remind_me(tr::now), [=] {
			ShowRemindBox(controller, itemId);
		}, &st::menuIconSchedule);
	}
}

void EditHistoryBox(
		not_null<Ui::GenericBox*> box,
		not_null<HistoryItem*> item) {
	box->setTitle(tr::lng_tgplus_edit_history());
	box->setWidth(st::boxWideWidth);
	box->setMaxHeight(st::boxMaxListHeight);

	const auto addVersion = [&](
			const QString &caption,
			const TextWithEntities &text) {
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			caption,
			st::boxDividerLabel));
		const auto label = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(text),
			st::boxLabel));
		label->setSelectable(true);
		Ui::AddSkip(box->verticalLayout());
	};
	for (const auto &edit : EditsFor(item)) {
		addVersion(
			tr::lng_tgplus_edit_until(
				tr::now,
				lt_time,
				FormatDateTime(edit.replacedAt)),
			edit.text);
	}
	addVersion(tr::lng_tgplus_edit_current(tr::now), item->originalText());
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

void DeletedLogBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		PeerData *peer) {
	const auto session = &controller->session();
	box->setTitle(peer
		? tr::lng_tgplus_log_title_chat(lt_chat, rpl::single(peer->name()))
		: tr::lng_tgplus_log_title());
	box->setWidth(st::boxWideWidth);
	box->setMaxHeight(st::boxMaxListHeight);

	const auto entries = std::make_shared<std::vector<LogEntry>>(
		DeletedLog(session, peer));

	auto top = object_ptr<Ui::VerticalLayout>(box);
	const auto search = top->add(
		object_ptr<Ui::InputField>(
			top,
			st::defaultInputField,
			tr::lng_tgplus_log_search()),
		st::boxRowPadding);
	box->setPinnedToTopContent(std::move(top));
	box->setFocusCallback([=] { search->setFocusFast(); });

	const auto list = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		style::margins());
	const auto rebuild = [=](const QString &query) {
		list->clear();
		auto shown = 0;
		for (const auto &entry : *entries) {
			if (!query.isEmpty() && !EntryMatches(session, entry, query)) {
				continue;
			} else if (++shown > kLogLimit) {
				break;
			}
			const auto label = list->add(
				object_ptr<Ui::FlatLabel>(
					list,
					rpl::single(EntryText(session, entry)),
					st::boxLabel),
				st::boxRowPadding);
			const auto peerId = entry.peer;
			const auto msgId = entry.record.id;
			label->setLink(1, std::make_shared<LambdaClickHandler>(
				crl::guard(box, [=] {
					box->closeBox();
					controller->showPeerHistory(
						peerId,
						Window::SectionShow::Way::ClearStack,
						msgId);
				})));
			Ui::AddSkip(list);
		}
		if (!shown) {
			list->add(
				object_ptr<Ui::FlatLabel>(
					list,
					(query.isEmpty()
						? tr::lng_tgplus_log_empty()
						: tr::lng_tgplus_log_not_found()),
					st::boxDividerLabel),
				st::boxRowPadding);
		}
	};
	rebuild(QString());
	search->changes() | rpl::on_next([=] {
		rebuild(search->getLastText().trimmed());
	}, search->lifetime());

	if (QDir(DeletedMediaFolder()).exists()) {
		box->addLeftButton(tr::lng_tgplus_log_media_folder(), [] {
			File::ShowInFolder(DeletedMediaFolder());
		});
	}
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

void AlertKeywordsBox(not_null<Ui::GenericBox*> box) {
	box->setTitle(tr::lng_tgplus_keywords_title());
	box->setWidth(st::boxWideWidth);
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		tr::lng_tgplus_keywords_about(),
		st::boxDividerLabel));
	Ui::AddSkip(box->verticalLayout());
	const auto field = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		Ui::InputField::Mode::MultiLine,
		tr::lng_tgplus_keywords_placeholder(),
		AlertKeywords().join(QChar('\n'))));
	box->setFocusCallback([=] { field->setFocusFast(); });
	box->addButton(tr::lng_settings_save(), [=] {
		SetAlertKeywords(field->getLastText().split(QChar('\n')));
		SetEnabled(Option::KeywordAlerts, !AlertKeywords().isEmpty());
		box->closeBox();
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

} // namespace Tgplus
