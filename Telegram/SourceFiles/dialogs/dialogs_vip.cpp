/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "dialogs/dialogs_vip.h"

#include "data/data_changes.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "storage/storage_account.h"
#include "ui/painter.h"
#include "ui/unread_badge_paint.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_dialogs.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

namespace Dialogs {
namespace {

constexpr auto kVipPeerPref = "tgplus_vip_peer";

[[nodiscard]] rpl::event_stream<not_null<Main::Session*>> &VipChanges() {
	static auto result = rpl::event_stream<not_null<Main::Session*>>();
	return result;
}

} // namespace

PeerId VipPeerId(not_null<Main::Session*> session) {
	const auto stored = session->local().readPref<QString>(kVipPeerPref);
	auto ok = false;
	const auto value = stored.toULongLong(&ok);
	return ok ? PeerId(value) : PeerId();
}

rpl::producer<PeerId> VipPeerIdValue(not_null<Main::Session*> session) {
	return rpl::single(
		VipPeerId(session)
	) | rpl::then(VipChanges().events(
	) | rpl::filter([=](not_null<Main::Session*> changed) {
		return (changed == session);
	}) | rpl::map([=] {
		return VipPeerId(session);
	}));
}

bool IsVipPeer(not_null<PeerData*> peer) {
	return (VipPeerId(&peer->session()) == peer->id);
}

bool CanBeVipPeer(not_null<PeerData*> peer) {
	const auto user = peer->asUser();
	return user && !user->isSelf() && !user->isBot();
}

void SetVipPeer(not_null<Main::Session*> session, PeerId peerId) {
	if (VipPeerId(session) == peerId) {
		return;
	}
	if (peerId) {
		session->local().writePref<QString>(
			kVipPeerPref,
			QString::number(peerId.value));
	} else {
		session->local().clearPref(kVipPeerPref);
	}
	VipChanges().fire_copy(session);
}

void ToggleVipPeer(not_null<PeerData*> peer) {
	SetVipPeer(&peer->session(), IsVipPeer(peer) ? PeerId() : peer->id);
}

VipBar::VipBar(
	QWidget *parent,
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer)
: AbstractButton(parent)
, _controller(controller)
, _peer(peer) {
	resize(width(), st::defaultDialogRow.height);
	refreshName();
	setClickedCallback([=] {
		_controller->showPeerHistory(
			_peer,
			Window::SectionShow::Way::ClearStack);
	});

	using PeerFlag = Data::PeerUpdate::Flag;
	_peer->session().changes().peerUpdates(
		_peer,
		PeerFlag::Name | PeerFlag::Photo
	) | rpl::on_next([=] {
		refreshName();
		update();
	}, lifetime());

	using HistoryFlag = Data::HistoryUpdate::Flag;
	_peer->session().changes().historyUpdates(
		_peer->owner().history(_peer),
		HistoryFlag::UnreadView
	) | rpl::on_next([=] {
		update();
	}, lifetime());
}

not_null<PeerData*> VipBar::peer() const {
	return _peer;
}

void VipBar::refreshName() {
	_name.setText(st::semiboldTextStyle, _peer->name());
}

void VipBar::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	const auto &row = st::defaultDialogRow;
	p.fillRect(rect(), st::dialogsBgActive);
	_peer->paintUserpicLeft(
		p,
		_userpic,
		row.padding.left(),
		(height() - row.photoSize) / 2,
		width(),
		row.photoSize);
	if (width() <= row.nameLeft) {
		return;
	}

	const auto history = _peer->owner().historyLoaded(_peer);
	const auto unread = history ? history->unreadCount() : 0;
	auto right = width() - row.padding.right();
	if (unread > 0) {
		auto badge = Ui::UnreadBadgeStyle();
		badge.active = true;
		const auto rect = Ui::PaintUnreadBadge(
			p,
			QString::number(unread),
			right,
			row.textTop,
			badge);
		right = rect.x() - st::dialogsUnreadPadding;
	}

	const auto &star = st::dialogsVipStar;
	star.paint(
		p,
		row.nameLeft,
		row.nameTop + st::dialogsVipStarTop,
		width());
	const auto nameLeft = row.nameLeft + star.width() + st::dialogsVipStarSkip;
	p.setPen(st::dialogsNameFgActive);
	_name.drawLeftElided(
		p,
		nameLeft,
		row.nameTop,
		std::max(width() - row.padding.right() - nameLeft, 0),
		width());

	const auto about = tr::lng_vip_peer_about(tr::now);
	const auto aboutWidth = std::max(right - row.textLeft, 0);
	p.setFont(st::dialogsTextFont);
	p.setPen(st::dialogsTextFgActive);
	p.drawTextLeft(
		row.textLeft,
		row.textTop,
		width(),
		st::dialogsTextFont->elided(about, aboutWidth));
}

void VipBar::contextMenuEvent(QContextMenuEvent *e) {
	_menu = base::make_unique_q<Ui::PopupMenu>(
		this,
		st::popupMenuWithIcons);
	const auto session = &_peer->session();
	_menu->addAction(tr::lng_vip_peer_remove(tr::now), [=] {
		SetVipPeer(session, PeerId());
	}, &st::menuIconFave);
	_menu->popup(e->globalPos());
}

} // namespace Dialogs
