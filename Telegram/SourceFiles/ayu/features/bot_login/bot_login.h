// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
//
// Bot accounts: log in with a bot token and use the bot like an account.
// Bots can't load the chats list and the history, so the known chats are
// remembered locally and the history is loaded by message ids.
#pragma once

namespace Main {
class Session;
} // namespace Main

namespace AyuBotLogin {

void Init();

[[nodiscard]] bool IsBot(not_null<Main::Session*> session);

// Called instead of loading the chats list for a bot account.
void SessionStarted(not_null<Main::Session*> session);

} // namespace AyuBotLogin
