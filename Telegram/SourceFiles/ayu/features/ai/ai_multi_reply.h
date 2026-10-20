// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
//
// Replies to one message from several of my accounts, written by AI.
#pragma once

#include "ayu/features/ai/ai_conversation.h"

namespace Main {
class Session;
} // namespace Main

namespace AyuMultiReply {

struct Link {
	QString username;
	ChannelId channel = 0;
	MsgId post = 0;
	MsgId topic = 0;
	MsgId comment = 0;

	[[nodiscard]] bool valid() const {
		return (!username.isEmpty() || channel) && post;
	}
};
[[nodiscard]] Link ParseLink(const QString &text);
[[nodiscard]] QString MakeLink(not_null<HistoryItem*> item);

// Where the reply should go for the given account.
struct Target {
	PeerData *peer = nullptr;
	MsgId msgId = 0;
	MsgId topicRootId = 0;
	QString error;
};
void Resolve(
	not_null<Main::Session*> session,
	const Link &link,
	Fn<void(Target)> done);

// The message itself (text, author and an image if it has one).
struct Content {
	QString author;
	QString text;
	QByteArray imageJpeg;
	QString error;
};
void LoadContent(
	not_null<Main::Session*> session,
	const Target &target,
	Fn<void(Content)> done);

// Writes `count` different replies.
void Generate(
	uint64 providerId,
	const Content &content,
	const QString &chatName,
	const QString &instructions,
	int count,
	Fn<void(std::vector<QString>)> done,
	Fn<void(QString)> failed);

// Sends a reply, `done` gets an empty string on success.
void Send(
	not_null<Main::Session*> session,
	const Target &target,
	const QString &text,
	bool join,
	Fn<void(QString)> done);

// Puts a reaction, `done` gets an empty string on success.
void React(
	not_null<Main::Session*> session,
	const Target &target,
	const QString &emoji,
	bool join,
	Fn<void(QString)> done);

// Runs an action for several accounts one by one with random pauses.
// `progress` gets (index, status text), `finished` gets the success count.
using ActionFn = Fn<void(
	not_null<Main::Session*> session,
	const Target &target,
	Fn<void(QString)> done)>;
void RunForAccounts(
	std::vector<uint64> accounts,
	Link link,
	ActionFn action,
	int minDelay,
	int maxDelay,
	Fn<void(int, QString)> progress,
	Fn<void(int)> finished);

struct AccountInfo {
	uint64 id = 0; // Session unique id.
	QString name;
	QString group;
	int32 groupId = 0;
};
[[nodiscard]] std::vector<AccountInfo> Accounts();

// AI "agent": the AI may answer with actions in JSON.
struct Action {
	enum class Type {
		React,
		Reply,
		Conversation,
	};
	Type type = Type::React;
	QString link;
	QString emoji;
	QString instructions;
	std::vector<uint64> accounts;

	// Conversation.
	QString topic;
	std::vector<AyuConversation::Participant> participants;
	int minGap = 60;
	int maxGap = 180;
	int maxMessages = 30;
};
[[nodiscard]] QString AgentPrompt(
	uint64 currentAccount,
	const QString &repliedLink,
	const QString &chatName);
// Empty if the answer has no actions. `error` is set for bad actions.
[[nodiscard]] std::vector<Action> ParseActions(
	const QString &answer,
	uint64 currentAccount,
	const QString &repliedLink,
	QString *error);
[[nodiscard]] QString ActionText(const Action &action);

[[nodiscard]] QByteArray ImageToJpeg(QImage image);

} // namespace AyuMultiReply
