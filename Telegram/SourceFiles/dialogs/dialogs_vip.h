/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/unique_qptr.h"
#include "ui/abstract_button.h"
#include "ui/text/text.h"
#include "ui/userpic_view.h"

class PeerData;

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

namespace Dialogs {

[[nodiscard]] PeerId VipPeerId(not_null<Main::Session*> session);
[[nodiscard]] rpl::producer<PeerId> VipPeerIdValue(
	not_null<Main::Session*> session);
[[nodiscard]] bool IsVipPeer(not_null<PeerData*> peer);
[[nodiscard]] bool CanBeVipPeer(not_null<PeerData*> peer);
void SetVipPeer(not_null<Main::Session*> session, PeerId peerId);
void ToggleVipPeer(not_null<PeerData*> peer);

class VipBar final : public Ui::AbstractButton {
public:
	VipBar(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer);

	[[nodiscard]] not_null<PeerData*> peer() const;

private:
	void paintEvent(QPaintEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;

	void refreshName();

	const not_null<Window::SessionController*> _controller;
	const not_null<PeerData*> _peer;
	Ui::PeerUserpicView _userpic;
	Ui::Text::String _name;
	base::unique_qptr<Ui::PopupMenu> _menu;

};

} // namespace Dialogs
