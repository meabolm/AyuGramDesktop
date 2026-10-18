// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "window/window_peer_menu.h"

namespace Ui {
class Show;
} // namespace Ui

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Data {
class Thread;
} // namespace Data

namespace Window {
class SessionController;
} // namespace Window

namespace AyuToolsUi {

void ShowAiProviders(std::shared_ptr<Ui::Show> show);
void ShowAllAiTasks(not_null<Window::SessionController*> controller);
void ShowRepeatingMessages(not_null<Window::SessionController*> controller);

void ShowChatAiTasks(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	MsgId topicRootId);

// `replyTo` may be empty. `insert` puts an answer in the message field.
// `extraContext` is added to the question (for example a stats report).
void ShowAiAsk(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	FullMsgId replyTo,
	QString question,
	Fn<void(QString)> insert,
	QString extraContext = QString());

void ShowChatStats(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer);

// Replies to one message from several of my accounts.
void ShowMultiReply(
	not_null<Window::SessionController*> controller,
	QString link = QString());

// Items for the context menu of a message.
void AddMessageToolsActions(
	not_null<Ui::PopupMenu*> menu,
	HistoryItem *item,
	not_null<Window::SessionController*> controller);

// "My tools" submenu of the chat menu.
void AddChatToolsMenu(
	not_null<PeerData*> peer,
	Data::Thread *thread,
	not_null<Window::SessionController*> controller,
	const Window::PeerMenuCallback &addAction);

} // namespace AyuToolsUi
