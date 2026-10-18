// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/ai/ai_multi_reply.h"

#include "apiwrap.h"
#include "ayu/features/account_groups/account_groups.h"
#include "ayu/features/ai/ai_core.h"
#include "base/call_delayed.h"
#include "base/random.h"
#include "data/data_channel.h"
#include "data/data_file_origin.h"
#include "data/data_histories.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "ui/image/image.h"

#include <QtCore/QBuffer>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QUrlQuery>
#include <QtGui/QPainter>

namespace AyuMultiReply {
namespace {

using AyuAccountGroups::Tr;

constexpr auto kImageTimeout = crl::time(20000);

[[nodiscard]] QString ErrorText(const QString &type) {
	if (type == u"USERNAME_NOT_OCCUPIED"_q
		|| type == u"USERNAME_INVALID"_q) {
		return Tr("Chat not found.", "چت پیدا نشد.");
	} else if (type == u"CHANNEL_PRIVATE"_q) {
		return Tr(
			"This account is not a member of the chat.",
			"این اکانت عضو چت نیست.");
	} else if (type == u"CHAT_WRITE_FORBIDDEN"_q
		|| type == u"CHAT_SEND_PLAIN_FORBIDDEN"_q
		|| type == u"CHAT_GUEST_SEND_FORBIDDEN"_q
		|| type == u"USER_BANNED_IN_CHANNEL"_q) {
		return Tr(
			"This account can't write there (join the chat first).",
			"این اکانت اجازه نوشتن نداره (اول باید عضو بشه).");
	} else if (type == u"MSG_ID_INVALID"_q) {
		return Tr("Message not found.", "پیام پیدا نشد.");
	} else if (type.startsWith(u"FLOOD_WAIT_"_q)
		|| type.startsWith(u"SLOWMODE_WAIT_"_q)) {
		return Tr(
			"Telegram asks to wait (too many messages).",
			"تلگرام گفت صبر کن (پیام زیاد فرستاده شده).");
	} else if (type == u"CHANNELS_TOO_MUCH"_q) {
		return Tr(
			"This account is in too many chats.",
			"این اکانت عضو چت‌های خیلی زیادیه.");
	}
	return type;
}

void ResolvePeer(
		not_null<Main::Session*> session,
		const Link &link,
		Fn<void(PeerData*, QString)> done) {
	if (link.channel) {
		const auto channel = session->data().channelLoaded(link.channel);
		if (!channel) {
			done(nullptr, Tr(
				"This account doesn't have this chat in its list.",
				"این چت توی لیست این اکانت نیست."));
		} else {
			done(channel, QString());
		}
		return;
	}
	session->api().request(MTPcontacts_ResolveUsername(
		MTP_flags(0),
		MTP_string(link.username),
		MTP_string()
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		const auto &data = result.data();
		session->data().processUsers(data.vusers());
		session->data().processChats(data.vchats());
		const auto peerId = peerFromMTP(data.vpeer());
		const auto peer = peerId ? session->data().peerLoaded(peerId) : nullptr;
		done(peer, peer ? QString() : Tr("Chat not found.", "چت پیدا نشد."));
	}).fail([=](const MTP::Error &error) {
		done(nullptr, ErrorText(error.type()));
	}).send();
}

[[nodiscard]] std::vector<QString> ParseReplies(const QString &answer) {
	auto result = std::vector<QString>();
	const auto from = answer.indexOf('[');
	const auto till = answer.lastIndexOf(']');
	if (from >= 0 && till > from) {
		const auto document = QJsonDocument::fromJson(
			answer.mid(from, till - from + 1).toUtf8());
		for (const auto &value : document.array()) {
			const auto text = value.toString().trimmed();
			if (!text.isEmpty()) {
				result.push_back(text);
			}
		}
	}
	if (result.empty()) {
		static const auto numbering = QRegularExpression(
			u"^\\s*(\\d+[\\.\\)\\-:]|[-*•])\\s*"_q);
		for (auto line : answer.split('\n')) {
			line = line.trimmed();
			line.remove(numbering);
			if (line.size() > 1 && line.startsWith('"') && line.endsWith('"')) {
				line = line.mid(1, line.size() - 2);
			}
			if (!line.isEmpty()) {
				result.push_back(line);
			}
		}
	}
	return result;
}

} // namespace

Link ParseLink(const QString &text) {
	auto result = Link();
	auto value = text.trimmed();
	if (value.isEmpty()) {
		return result;
	}
	if (!value.contains(u"://"_q)) {
		value = u"https://"_q + value;
	}
	const auto url = QUrl(value);
	const auto host = url.host().toLower();
	if (host != u"t.me"_q
		&& host != u"telegram.me"_q
		&& host != u"telegram.dog"_q
		&& !host.endsWith(u".t.me"_q)) {
		return result;
	}
	auto parts = url.path().split('/', Qt::SkipEmptyParts);
	if (host.endsWith(u".t.me"_q)) {
		parts.push_front(host.mid(0, host.size() - 5));
	}
	if (parts.isEmpty()) {
		return result;
	}
	const auto number = [](const QString &part) {
		auto ok = false;
		const auto value = part.toLongLong(&ok);
		return ok ? value : 0LL;
	};
	auto numbers = QStringList();
	if (parts[0] == u"c"_q) {
		if (parts.size() < 3) {
			return result;
		}
		result.channel = ChannelId(number(parts[1]));
		numbers = parts.mid(2);
	} else if (parts[0] == u"s"_q || parts[0].startsWith('+')) {
		return result;
	} else {
		result.username = parts[0];
		numbers = parts.mid(1);
	}
	if (numbers.size() >= 2) {
		result.topic = MsgId(number(numbers[0]));
		result.post = MsgId(number(numbers[1]));
	} else if (numbers.size() == 1) {
		result.post = MsgId(number(numbers[0]));
	}
	const auto query = QUrlQuery(url);
	result.comment = MsgId(number(query.queryItemValue(u"comment"_q)));
	if (const auto thread = number(query.queryItemValue(u"thread"_q))) {
		result.topic = MsgId(thread);
	}
	return result;
}

QString MakeLink(not_null<HistoryItem*> item) {
	const auto peer = item->history()->peer;
	const auto channel = peer->asChannel();
	if (!channel) {
		return QString();
	}
	const auto topic = item->topicRootId();
	const auto username = channel->username();
	auto base = username.isEmpty()
		? (u"https://t.me/c/"_q + QString::number(peerToChannel(peer->id).bare))
		: (u"https://t.me/"_q + username);
	if (topic && channel->isForum()) {
		base += '/' + QString::number(topic.bare);
	}
	return base + '/' + QString::number(item->id.bare);
}

void Resolve(
		not_null<Main::Session*> session,
		const Link &link,
		Fn<void(Target)> done) {
	if (!link.valid()) {
		done({ .error = Tr("Wrong link.", "لینک درست نیست.") });
		return;
	}
	ResolvePeer(session, link, [=](PeerData *peer, QString error) {
		if (!peer) {
			done({ .error = error });
			return;
		} else if (!peer->isBroadcast()) {
			done({
				.peer = peer,
				.msgId = link.comment ? link.comment : link.post,
				.topicRootId = link.topic,
			});
			return;
		}
		// A channel post: the reply goes to its comments.
		session->api().request(MTPmessages_GetDiscussionMessage(
			peer->input(),
			MTP_int(link.post)
		)).done([=](const MTPmessages_DiscussionMessage &result) {
			const auto &data = result.data();
			session->data().processUsers(data.vusers());
			session->data().processChats(data.vchats());
			const auto &list = data.vmessages().v;
			if (list.isEmpty()) {
				done({ .error = Tr(
					"Comments are off for this post.",
					"کامنت این پست بسته‌ست.") });
				return;
			}
			const auto discussionId = PeerFromMessage(list.front());
			const auto rootId = IdFromMessage(list.front());
			const auto discussion = session->data().peerLoaded(discussionId);
			if (!discussion) {
				done({ .error = Tr(
					"Comments are off for this post.",
					"کامنت این پست بسته‌ست.") });
				return;
			}
			done({
				.peer = discussion,
				.msgId = link.comment ? link.comment : rootId,
				.topicRootId = rootId,
			});
		}).fail([=](const MTP::Error &error) {
			done({ .error = ErrorText(error.type()) });
		}).send();
	});
}

QByteArray ImageToJpeg(QImage image) {
	if (image.isNull()) {
		return {};
	}
	constexpr auto kMax = 1280;
	if (image.width() > kMax || image.height() > kMax) {
		image = image.scaled(
			kMax,
			kMax,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation);
	}
	if (image.hasAlphaChannel()) {
		auto copy = QImage(image.size(), QImage::Format_RGB32);
		copy.fill(Qt::white);
		{
			QPainter p(&copy);
			p.drawImage(0, 0, image);
		}
		image = std::move(copy);
	}
	auto result = QByteArray();
	auto buffer = QBuffer(&result);
	buffer.open(QIODevice::WriteOnly);
	image.save(&buffer, "JPG", 85);
	return result;
}

void LoadContent(
		not_null<Main::Session*> session,
		const Target &target,
		Fn<void(Content)> done) {
	const auto peer = target.peer;
	const auto msgId = target.msgId;
	if (!peer) {
		done({ .error = target.error });
		return;
	}
	const auto withItem = [=] {
		const auto item = session->data().message(peer->id, msgId);
		if (!item) {
			done({ .error = Tr("Message not found.", "پیام پیدا نشد.") });
			return;
		}
		auto content = Content{
			.author = item->from()->name(),
			.text = item->originalText().text,
		};
		const auto media = item->media();
		const auto photo = media ? media->photo() : nullptr;
		if (!photo) {
			if (content.text.isEmpty() && media) {
				content.text = u"["_q
					+ Tr("media without text", "رسانه بدون متن")
					+ u"]"_q;
			}
			done(content);
			return;
		}
		struct State {
			std::shared_ptr<Data::PhotoMedia> view;
			rpl::lifetime lifetime;
			bool finished = false;
		};
		const auto state = std::make_shared<State>();
		state->view = photo->createMediaView();
		state->view->wanted(Data::PhotoSize::Large, item->fullId());
		const auto finish = [=](bool timeout) {
			if (state->finished) {
				return true;
			}
			const auto image = state->view->image(Data::PhotoSize::Large);
			if (!image && !timeout) {
				return false;
			}
			state->finished = true;
			auto result = content;
			if (image) {
				result.imageJpeg = ImageToJpeg(image->original());
			}
			done(result);
			state->lifetime.destroy();
			return true;
		};
		if (finish(false)) {
			return;
		}
		session->downloaderTaskFinished(
		) | rpl::on_next([=] {
			finish(false);
		}, state->lifetime);
		base::call_delayed(kImageTimeout, session, [=] {
			finish(true);
		});
	};
	if (session->data().message(peer->id, msgId)) {
		withItem();
	} else {
		session->api().requestMessageData(peer, msgId, withItem);
	}
}

void Generate(
		uint64 providerId,
		const Content &content,
		const QString &chatName,
		const QString &instructions,
		int count,
		Fn<void(std::vector<QString>)> done,
		Fn<void(QString)> failed) {
	auto system = AyuAi::BaseSystemPrompt();
	system += u"\n\nYou write replies to a Telegram message on behalf of "
		"several different people. Each reply must sound like a different "
		"real person: different wording, different length and a slightly "
		"different angle, but all of them follow the owner's instructions. "
		"Never mention that you are an AI or that the replies were "
		"generated. Use the language of the message unless the instructions "
		"say otherwise. Return ONLY a JSON array of exactly "_q
		+ QString::number(count)
		+ u" strings, nothing else."_q;
	auto text = u"Chat: "_q + chatName + u"\n"_q
		+ u"Message from "_q + content.author + u":\n"_q
		+ content.text
		+ (content.imageJpeg.isEmpty()
			? QString()
			: u"\n[The image of this message is attached]"_q)
		+ u"\n\nInstructions from the owner: "_q
		+ (instructions.trimmed().isEmpty()
			? u"reply naturally."_q
			: instructions.trimmed())
		+ u"\n\nWrite "_q + QString::number(count) + u" replies."_q;
	auto message = AyuAi::ChatMessage{ .role = u"user"_q, .text = text };
	const auto provider = AyuAi::FindProvider(providerId);
	if (!content.imageJpeg.isEmpty() && provider && provider->vision) {
		message.imageJpeg = content.imageJpeg;
	}
	AyuAi::Complete(
		AyuAi::Request{
			.providerId = providerId,
			.system = system,
			.messages = { message },
			.temperature = 1.0,
			.maxTokens = std::min(300 + count * 200, 4000),
		},
		[=](QString answer) {
			auto replies = ParseReplies(answer);
			if (replies.empty()) {
				failed(Tr(
					"The AI answer was empty, try again.",
					"جواب هوش مصنوعی خالی بود، دوباره امتحان کن."));
				return;
			}
			const auto original = replies.size();
			for (auto i = 0; int(replies.size()) < count; ++i) {
				replies.push_back(replies[i % original]);
			}
			replies.resize(count);
			done(std::move(replies));
		},
		failed);
}

void Send(
		not_null<Main::Session*> session,
		const Target &target,
		const QString &text,
		bool join,
		Fn<void(QString)> done) {
	const auto peer = target.peer;
	if (!peer) {
		done(target.error);
		return;
	}
	const auto send = [=] {
		using Flag = MTPmessages_SendMessage::Flag;
		const auto history = session->data().history(peer);
		session->api().request(MTPmessages_SendMessage(
			MTP_flags(Flag::f_reply_to),
			peer->input(),
			Data::ReplyToForMTP(history, FullReplyTo{
				.messageId = FullMsgId(peer->id, target.msgId),
				.topicRootId = target.topicRootId,
			}),
			MTP_string(text),
			MTP_long(base::RandomValue<uint64>()),
			MTPReplyMarkup(),
			MTPVector<MTPMessageEntity>(),
			MTP_int(0),
			MTP_int(0),
			MTP_inputPeerEmpty(),
			MTPInputQuickReplyShortcut(),
			MTP_long(0),
			MTP_long(0),
			MTPSuggestedPost(),
			MTPInputRichMessage()
		)).done([=](const MTPUpdates &result) {
			session->api().applyUpdates(result);
			done(QString());
		}).fail([=](const MTP::Error &error) {
			done(ErrorText(error.type()));
		}).send();
	};
	const auto channel = peer->asChannel();
	if (!join || !channel || channel->amIn() || channel->isBroadcast()) {
		send();
		return;
	}
	session->api().request(MTPchannels_JoinChannel(
		channel->inputChannel()
	)).done([=](const MTPmessages_ChatInviteJoinResult &result) {
		result.match([&](const MTPDmessages_chatInviteJoinResultOk &data) {
			session->api().applyUpdates(data.vupdates());
			send();
		}, [&](const auto &) {
			done(Tr(
				"Joining needs a verification, join by hand.",
				"عضویت نیاز به تایید داره، دستی عضو شو."));
		});
	}).fail([=](const MTP::Error &error) {
		if (error.type() == u"USER_ALREADY_PARTICIPANT"_q) {
			send();
		} else {
			done(ErrorText(error.type()));
		}
	}).send();
}

} // namespace AyuMultiReply
