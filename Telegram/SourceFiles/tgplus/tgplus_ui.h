/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class HistoryItem;
class PeerData;

namespace Ui {
class GenericBox;
class PopupMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Tgplus {

void FillMessageMenu(
	not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> controller,
	not_null<HistoryItem*> item);

void EditHistoryBox(
	not_null<Ui::GenericBox*> box,
	not_null<HistoryItem*> item);

void DeletedLogBox(
	not_null<Ui::GenericBox*> box,
	not_null<Window::SessionController*> controller,
	PeerData *peer);

void AlertKeywordsBox(not_null<Ui::GenericBox*> box);

} // namespace Tgplus
