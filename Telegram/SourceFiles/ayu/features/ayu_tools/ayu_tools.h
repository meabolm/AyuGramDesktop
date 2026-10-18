// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
//
// Shared helpers of the personal tools (repeating messages, AI, stats).
#pragma once

#include "api/api_common.h"

#include <QtCore/QJsonObject>

namespace Main {
class Session;
} // namespace Main

namespace AyuTools {

// Starts the background parts (repeating messages, AI tasks).
void Init();

[[nodiscard]] Main::Session *FindSession(uint64 accountKey);
[[nodiscard]] std::vector<not_null<Main::Session*>> Sessions();

[[nodiscard]] QString DataPath(const QString &fileName);
[[nodiscard]] QJsonObject LoadJson(const QString &fileName);
void SaveJson(const QString &fileName, const QJsonObject &object);

[[nodiscard]] QJsonArray TagsToJson(const TextWithTags::Tags &tags);
[[nodiscard]] TextWithTags::Tags TagsFromJson(const QJsonArray &array);

void SendText(
	not_null<Main::Session*> session,
	not_null<PeerData*> peer,
	MsgId topicRootId,
	TextWithTags text,
	MsgId replyTo = 0,
	Api::SendOptions options = {});

[[nodiscard]] QString FormatTime(TimeId time);
[[nodiscard]] QString FormatDate(TimeId time);

} // namespace AyuTools
