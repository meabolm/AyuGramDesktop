// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/ayu_tools/history_loader.h"

#include "apiwrap.h"
#include "ayu/features/account_groups/account_groups.h"
#include "base/call_delayed.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "main/main_session.h"

namespace AyuTools {
namespace {

constexpr auto kPageSize = 100;
constexpr auto kPageDelay = crl::time(350);

[[nodiscard]] MediaKind DocumentKind(const MTPDmessageMediaDocument &data) {
	if (data.is_round()) {
		return MediaKind::VideoNote;
	} else if (data.is_voice()) {
		return MediaKind::Voice;
	}
	auto result = data.is_video() ? MediaKind::Video : MediaKind::File;
	if (const auto document = data.vdocument()) {
		document->match([&](const MTPDdocument &document) {
			auto animated = false;
			for (const auto &attribute : document.vattributes().v) {
				attribute.match([&](const MTPDdocumentAttributeSticker &) {
					result = MediaKind::Sticker;
				}, [&](const MTPDdocumentAttributeVideo &video) {
					if (result != MediaKind::Sticker) {
						result = video.is_round_message()
							? MediaKind::VideoNote
							: MediaKind::Video;
					}
				}, [&](const MTPDdocumentAttributeAudio &audio) {
					if (result == MediaKind::File) {
						result = audio.is_voice()
							? MediaKind::Voice
							: MediaKind::Audio;
					}
				}, [&](const MTPDdocumentAttributeAnimated &) {
					animated = true;
				}, [](const auto &) {
				});
			}
			if (animated && result != MediaKind::Sticker) {
				result = MediaKind::Gif;
			}
		}, [](const auto &) {
		});
	}
	return result;
}

[[nodiscard]] MediaKind ParseMedia(const MTPMessageMedia &media) {
	return media.match([](const MTPDmessageMediaPhoto &) {
		return MediaKind::Photo;
	}, [](const MTPDmessageMediaDocument &data) {
		return DocumentKind(data);
	}, [](const MTPDmessageMediaWebPage &) {
		return MediaKind::Link;
	}, [](const MTPDmessageMediaPoll &) {
		return MediaKind::Poll;
	}, [](const MTPDmessageMediaGeo &) {
		return MediaKind::Location;
	}, [](const MTPDmessageMediaGeoLive &) {
		return MediaKind::Location;
	}, [](const MTPDmessageMediaVenue &) {
		return MediaKind::Location;
	}, [](const MTPDmessageMediaContact &) {
		return MediaKind::Contact;
	}, [](const MTPDmessageMediaEmpty &) {
		return MediaKind::None;
	}, [](const auto &) {
		return MediaKind::Other;
	});
}

void ParseReactions(
		const MTPMessageReactions &reactions,
		std::vector<ReactionCount> &to) {
	reactions.match([&](const MTPDmessageReactions &data) {
		for (const auto &count : data.vresults().v) {
			const auto &fields = count.data();
			auto entry = ReactionCount{
				.count = fields.vcount().v,
				.chosen = fields.vchosen_order().has_value(),
			};
			auto skip = false;
			fields.vreaction().match([&](const MTPDreactionEmoji &emoji) {
				entry.emoji = qs(emoji.vemoticon());
			}, [&](const MTPDreactionCustomEmoji &custom) {
				entry.customId = custom.vdocument_id().v;
			}, [&](const auto &) {
				skip = true; // Paid or empty reactions.
			});
			if (!skip && entry.count > 0) {
				to.push_back(std::move(entry));
			}
		}
	});
}

[[nodiscard]] MsgId ParseReplyTo(const MTPMessageReplyHeader &header) {
	return header.match([](const MTPDmessageReplyHeader &data) {
		if (data.vreply_to_peer_id()) {
			return MsgId(); // Reply to another chat.
		}
		return MsgId(data.vreply_to_msg_id().value_or_empty());
	}, [](const auto &) {
		return MsgId();
	});
}

[[nodiscard]] std::optional<RawMessage> Parse(const MTPMessage &message) {
	return message.match([](const MTPDmessage &data) {
		auto result = RawMessage{
			.id = data.vid().v,
			.date = data.vdate().v,
			.from = data.vfrom_id()
				? peerFromMTP(*data.vfrom_id())
				: peerFromMTP(data.vpeer_id()),
			.text = qs(data.vmessage()),
			.out = data.is_out(),
			.forwarded = data.vfwd_from().has_value(),
		};
		if (const auto media = data.vmedia()) {
			result.media = ParseMedia(*media);
		}
		if (const auto reply = data.vreply_to()) {
			result.replyTo = ParseReplyTo(*reply);
		}
		if (const auto replies = data.vreplies()) {
			result.repliesCount = replies->data().vreplies().v;
		}
		if (const auto views = data.vviews()) {
			result.views = views->v;
		}
		if (const auto forwards = data.vforwards()) {
			result.forwards = forwards->v;
		}
		if (const auto reactions = data.vreactions()) {
			ParseReactions(*reactions, result.reactions);
		}
		return std::make_optional(std::move(result));
	}, [](const MTPDmessageService &data) {
		auto result = RawMessage{
			.id = data.vid().v,
			.date = data.vdate().v,
			.from = data.vfrom_id()
				? peerFromMTP(*data.vfrom_id())
				: peerFromMTP(data.vpeer_id()),
			.out = data.is_out(),
			.service = true,
		};
		return std::make_optional(std::move(result));
	}, [](const MTPDmessageEmpty &) {
		return std::optional<RawMessage>();
	});
}

struct Page {
	std::vector<RawMessage> messages;
	bool notModified = false;
};

[[nodiscard]] Page ParsePage(
		not_null<Main::Session*> session,
		const MTPmessages_Messages &result) {
	auto page = Page();
	result.match([&](const MTPDmessages_messagesNotModified &) {
		page.notModified = true;
	}, [&](const auto &data) {
		session->data().processUsers(data.vusers());
		session->data().processChats(data.vchats());
		for (const auto &message : data.vmessages().v) {
			if (auto parsed = Parse(message)) {
				page.messages.push_back(std::move(*parsed));
			}
		}
	});
	return page;
}

struct LoadState {
	not_null<Main::Session*> session;
	not_null<PeerData*> peer;
	TimeId from = 0;
	TimeId till = 0;
	int limit = 0;
	Fn<void(LoadProgress)> progress;
	Fn<void(std::vector<RawMessage>)> done;
	Fn<void(QString)> failed;
	std::vector<RawMessage> collected;
	MsgId offsetId = 0;
	bool cancelled = false;
	base::has_weak_ptr guard;
};

void Finish(const std::shared_ptr<LoadState> &state) {
	auto result = base::take(state->collected);
	ranges::sort(result, ranges::less(), &RawMessage::id);
	if (state->done && !state->cancelled) {
		state->done(std::move(result));
	}
}

void RequestPage(const std::shared_ptr<LoadState> &state) {
	if (state->cancelled) {
		return;
	}
	const auto session = state->session;
	session->api().request(MTPmessages_GetHistory(
		state->peer->input(),
		MTP_int(state->offsetId),
		MTP_int(state->offsetId ? 0 : (state->till + 1)),
		MTP_int(0),
		MTP_int(kPageSize),
		MTP_int(0),
		MTP_int(0),
		MTP_long(0)
	)).done([=](const MTPmessages_Messages &result) {
		if (state->cancelled) {
			return;
		}
		auto page = ParsePage(session, result);
		if (page.messages.empty()) {
			Finish(state);
			return;
		}
		auto reachedStart = false;
		auto oldestId = MsgId();
		auto oldestDate = TimeId();
		for (auto &message : page.messages) {
			if (!oldestId || message.id < oldestId) {
				oldestId = message.id;
				oldestDate = message.date;
			}
			if (message.date < state->from) {
				reachedStart = true;
				continue;
			} else if (message.date > state->till) {
				continue;
			}
			state->collected.push_back(std::move(message));
		}
		if (state->progress) {
			state->progress({
				.loaded = int(state->collected.size()),
				.oldestDate = oldestDate,
			});
		}
		const auto full = (state->limit > 0)
			&& (int(state->collected.size()) >= state->limit);
		if (reachedStart
			|| full
			|| !oldestId
			|| (state->offsetId && oldestId >= state->offsetId)) {
			Finish(state);
			return;
		}
		state->offsetId = oldestId;
		base::call_delayed(kPageDelay, &state->guard, [=] {
			RequestPage(state);
		});
	}).fail([=](const MTP::Error &error) {
		if (state->cancelled) {
			return;
		}
		if (!state->collected.empty()) {
			Finish(state);
		} else if (state->failed) {
			state->failed(error.type());
		}
	}).send();
}

} // namespace

int RawMessage::reactionsTotal() const {
	auto result = 0;
	for (const auto &reaction : reactions) {
		result += reaction.count;
	}
	return result;
}

Fn<void()> LoadHistory(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		TimeId from,
		TimeId till,
		int limit,
		Fn<void(LoadProgress)> progress,
		Fn<void(std::vector<RawMessage>)> done,
		Fn<void(QString)> failed) {
	const auto state = std::make_shared<LoadState>(LoadState{
		.session = session,
		.peer = peer,
		.from = from,
		.till = till,
		.limit = limit,
		.progress = std::move(progress),
		.done = std::move(done),
		.failed = std::move(failed),
	});
	RequestPage(state);
	return [=] {
		state->cancelled = true;
	};
}

void LoadRecent(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		int limit,
		Fn<void(std::vector<RawMessage>)> done,
		Fn<void(QString)> failed) {
	session->api().request(MTPmessages_GetHistory(
		peer->input(),
		MTP_int(0),
		MTP_int(0),
		MTP_int(0),
		MTP_int(std::clamp(limit, 1, kPageSize)),
		MTP_int(0),
		MTP_int(0),
		MTP_long(0)
	)).done([=](const MTPmessages_Messages &result) {
		auto page = ParsePage(session, result);
		ranges::sort(page.messages, ranges::less(), &RawMessage::id);
		done(std::move(page.messages));
	}).fail([=](const MTP::Error &error) {
		if (failed) {
			failed(error.type());
		}
	}).send();
}

void LoadScheduled(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		Fn<void(std::vector<RawMessage>)> done,
		Fn<void(QString)> failed) {
	session->api().request(MTPmessages_GetScheduledHistory(
		peer->input(),
		MTP_long(0)
	)).done([=](const MTPmessages_Messages &result) {
		auto page = ParsePage(session, result);
		ranges::sort(page.messages, ranges::less(), &RawMessage::date);
		done(std::move(page.messages));
	}).fail([=](const MTP::Error &error) {
		if (failed) {
			failed(error.type());
		}
	}).send();
}

QString PeerName(not_null<Main::Session*> session, PeerId peerId) {
	if (!peerId) {
		return QString();
	}
	const auto peer = session->data().peerLoaded(peerId);
	if (!peer) {
		return u"#"_q + QString::number(peerId.value);
	}
	if (peer->isSelf()) {
		return AyuAccountGroups::Tr("Me", "من");
	}
	return peer->name();
}

QString MediaLabel(MediaKind kind) {
	using AyuAccountGroups::Tr;
	switch (kind) {
	case MediaKind::Photo: return Tr("Photo", "عکس");
	case MediaKind::Video: return Tr("Video", "ویدیو");
	case MediaKind::Gif: return Tr("GIF", "گیف");
	case MediaKind::Voice: return Tr("Voice message", "ویس");
	case MediaKind::VideoNote: return Tr("Video message", "ویدیو مسیج");
	case MediaKind::Sticker: return Tr("Sticker", "استیکر");
	case MediaKind::Audio: return Tr("Music", "آهنگ");
	case MediaKind::File: return Tr("File", "فایل");
	case MediaKind::Link: return Tr("Link", "لینک");
	case MediaKind::Poll: return Tr("Poll", "نظرسنجی");
	case MediaKind::Location: return Tr("Location", "لوکیشن");
	case MediaKind::Contact: return Tr("Contact", "مخاطب");
	case MediaKind::Other: return Tr("Other", "سایر");
	case MediaKind::None: break;
	}
	return Tr("Text", "متن");
}

QString ShortText(const RawMessage &message, int limit) {
	auto text = message.text.simplified();
	if (text.isEmpty()) {
		text = (message.service)
			? AyuAccountGroups::Tr("Service message", "پیام سیستمی")
			: u"["_q + MediaLabel(message.media) + u"]"_q;
	} else if (message.media != MediaKind::None
		&& message.media != MediaKind::Link) {
		text = u"["_q + MediaLabel(message.media) + u"] "_q + text;
	}
	if (text.size() > limit) {
		text = text.mid(0, limit - 1) + QChar(0x2026);
	}
	return text;
}

QString FormatForPrompt(
		not_null<Main::Session*> session,
		const std::vector<RawMessage> &messages,
		int maxChars) {
	auto lines = QStringList();
	auto total = 0;
	for (auto i = messages.rbegin(); i != messages.rend(); ++i) {
		const auto &message = *i;
		if (message.service) {
			continue;
		}
		auto line = u"[#"_q
			+ QString::number(message.id.bare)
			+ u"] "_q
			+ PeerName(session, message.from);
		if (message.replyTo) {
			line += u" (reply to #"_q
				+ QString::number(message.replyTo.bare)
				+ u")"_q;
		}
		line += u": "_q + ShortText(message, 600);
		total += line.size() + 1;
		if (total > maxChars && !lines.isEmpty()) {
			break;
		}
		lines.push_front(line);
	}
	return lines.join('\n');
}

} // namespace AyuTools
