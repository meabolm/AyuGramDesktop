// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/ayu_tools/ayu_tools.h"

#include "apiwrap.h"
#include "base/unixtime.h"
#include "ayu/features/ai/ai_conversation.h"
#include "ayu/features/ai/ai_core.h"
#include "ayu/features/repeat_schedule/repeat_schedule.h"
#include "core/application.h"
#include "data/data_forum_topic.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "settings.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>

namespace AyuTools {

void Init() {
	AyuRepeat::Init();
	AyuAi::Init();
	AyuConversation::Init();
}

Main::Session *FindSession(uint64 accountKey) {
	for (const auto &session : Sessions()) {
		if (session->uniqueId() == accountKey) {
			return session.get();
		}
	}
	return nullptr;
}

std::vector<not_null<Main::Session*>> Sessions() {
	auto result = std::vector<not_null<Main::Session*>>();
	if (!Core::IsAppLaunched()) {
		return result;
	}
	for (const auto &[index, account] : Core::App().domain().accounts()) {
		if (const auto session = account->maybeSession()) {
			result.push_back(session);
		}
	}
	return result;
}

QString DataPath(const QString &fileName) {
	return cWorkingDir() + u"tdata/"_q + fileName;
}

QJsonObject LoadJson(const QString &fileName) {
	auto file = QFile(DataPath(fileName));
	if (!file.open(QIODevice::ReadOnly)) {
		return QJsonObject();
	}
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(file.readAll(), &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		LOG(("AyuTools: bad file '%1'.").arg(fileName));
		return QJsonObject();
	}
	return document.object();
}

void SaveJson(const QString &fileName, const QJsonObject &object) {
	QDir().mkpath(cWorkingDir() + u"tdata"_q);
	auto file = QSaveFile(DataPath(fileName));
	if (!file.open(QIODevice::WriteOnly)) {
		LOG(("AyuTools: could not write '%1'.").arg(fileName));
		return;
	}
	file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
	if (!file.commit()) {
		LOG(("AyuTools: could not save '%1'.").arg(fileName));
	}
}

QJsonArray TagsToJson(const TextWithTags::Tags &tags) {
	auto result = QJsonArray();
	for (const auto &tag : tags) {
		auto object = QJsonObject();
		object.insert(u"offset"_q, tag.offset);
		object.insert(u"length"_q, tag.length);
		object.insert(u"id"_q, tag.id);
		result.append(object);
	}
	return result;
}

TextWithTags::Tags TagsFromJson(const QJsonArray &array) {
	auto result = TextWithTags::Tags();
	for (const auto &value : array) {
		const auto object = value.toObject();
		result.push_back({
			.offset = object.value(u"offset"_q).toInt(),
			.length = object.value(u"length"_q).toInt(),
			.id = object.value(u"id"_q).toString(),
		});
	}
	return result;
}

void SendText(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		MsgId topicRootId,
		TextWithTags text,
		MsgId replyTo,
		Api::SendOptions options) {
	const auto history = session->data().history(peer);
	const auto topic = topicRootId
		? peer->forumTopicFor(topicRootId)
		: nullptr;
	const auto thread = topic
		? static_cast<Data::Thread*>(topic)
		: static_cast<Data::Thread*>(history.get());
	auto action = Api::SendAction(thread, options);
	action.clearDraft = false;
	if (replyTo) {
		action.replyTo.messageId = FullMsgId(peer->id, replyTo);
	}
	auto message = Api::MessageToSend(action);
	message.textWithTags = std::move(text);
	session->api().sendMessage(std::move(message));
}

QString FormatTime(TimeId time) {
	return QLocale().toString(
		base::unixtime::parse(time).time(),
		QLocale::ShortFormat);
}

QString FormatDate(TimeId time) {
	return QLocale().toString(
		base::unixtime::parse(time).date(),
		QLocale::ShortFormat);
}

} // namespace AyuTools
