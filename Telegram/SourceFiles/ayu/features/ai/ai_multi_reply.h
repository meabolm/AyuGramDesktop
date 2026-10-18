// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
//
// Replies to one message from several of my accounts, written by AI.
#pragma once

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

[[nodiscard]] QByteArray ImageToJpeg(QImage image);

} // namespace AyuMultiReply
