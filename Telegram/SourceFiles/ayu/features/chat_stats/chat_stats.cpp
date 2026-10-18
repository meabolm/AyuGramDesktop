// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/chat_stats/chat_stats.h"

#include "ayu/features/account_groups/account_groups.h"
#include "ayu/features/ayu_tools/ayu_tools.h"
#include "base/unixtime.h"
#include "data/data_peer.h"
#include "main/main_session.h"

namespace AyuStats {
namespace {

using AyuAccountGroups::Tr;

template <typename Map>
[[nodiscard]] std::vector<PeerScore> TopPeers(const Map &map, int top) {
	auto result = std::vector<PeerScore>();
	for (const auto &[peer, value] : map) {
		if (peer && value > 0) {
			result.push_back({ peer, value });
		}
	}
	ranges::sort(result, ranges::greater(), &PeerScore::value);
	if (int(result.size()) > top) {
		result.resize(top);
	}
	return result;
}

[[nodiscard]] std::vector<WordScore> TopWords(
		const base::flat_map<QString, int> &map,
		int top,
		int minCount = 1) {
	auto result = std::vector<WordScore>();
	for (const auto &[word, value] : map) {
		if (value >= minCount) {
			result.push_back({ word, value });
		}
	}
	ranges::sort(result, ranges::greater(), &WordScore::value);
	if (int(result.size()) > top) {
		result.resize(top);
	}
	return result;
}

[[nodiscard]] MessageScore Score(const RawMessage &message, int value) {
	return {
		.id = message.id,
		.from = message.from,
		.date = message.date,
		.text = AyuTools::ShortText(message, 120),
		.value = value,
		.reactions = message.reactions,
	};
}

void KeepTop(std::vector<MessageScore> &list, int top) {
	ranges::sort(list, ranges::greater(), &MessageScore::value);
	if (int(list.size()) > top) {
		list.resize(top);
	}
}

[[nodiscard]] const base::flat_set<QString> &StopWords() {
	static const auto result = [] {
		auto list = base::flat_set<QString>();
		const auto words = QString::fromUtf8(
			"که از به در با را این اون آن هم رو یه یک برای تا ولی اما "
			"هست است بود شد شده می نمی دیگه خیلی چی چه کی کجا چرا "
			"من تو او ما شما اونا ایشون خودم خودت همه هر نه آره اره "
			"باشه بشه کنم کنی کنه کن داره دارم داری ندارم هستم هستی "
			"بعد قبل الان فقط حتی پس اگه اگر چون چطور مثل توی تو "
			"the and for you are was this that with have not but "
			"what all can just your from they will about there");
		for (const auto &word : words.split(' ', Qt::SkipEmptyParts)) {
			list.emplace(word);
		}
		return list;
	}();
	return result;
}

[[nodiscard]] QString ReactionKey(const ReactionCount &reaction) {
	return reaction.customId
		? (u"custom:"_q + QString::number(reaction.customId))
		: reaction.emoji;
}

} // namespace

QString ReactionText(const ReactionCount &reaction) {
	return reaction.customId
		? Tr("custom emoji", "ایموجی سفارشی")
		: reaction.emoji;
}

QString ReactionsLine(
		const std::vector<ReactionCount> &reactions,
		int limit) {
	auto sorted = reactions;
	ranges::sort(sorted, ranges::greater(), &ReactionCount::count);
	auto parts = QStringList();
	for (const auto &reaction : sorted) {
		if (int(parts.size()) >= limit) {
			break;
		}
		parts.push_back(ReactionText(reaction)
			+ ' '
			+ QString::number(reaction.count));
	}
	return parts.join(u"  "_q);
}

Stats Compute(
		const std::vector<RawMessage> &messages,
		TimeId from,
		TimeId till,
		PeerId filter,
		int top) {
	auto result = Stats{ .from = from, .till = till, .filter = filter };

	auto byId = base::flat_map<MsgId, const RawMessage*>();
	for (const auto &message : messages) {
		byId.emplace(message.id, &message);
	}

	auto senders = base::flat_map<PeerId, int>();
	auto repliesTo = base::flat_map<MsgId, int>();
	auto repliedUsers = base::flat_map<PeerId, int>();
	auto replyingUsers = base::flat_map<PeerId, int>();
	auto reactedUsers = base::flat_map<PeerId, int>();
	auto reactionTotals = base::flat_map<QString, ReactionCount>();
	auto media = base::flat_map<MediaKind, int>();
	auto days = base::flat_map<QDate, int>();
	auto hashtags = base::flat_map<QString, int>();
	auto mentions = base::flat_map<QString, int>();
	auto words = base::flat_map<QString, int>();
	auto links = base::flat_map<QString, int>();

	static const auto hashtagRegex = QRegularExpression(
		u"#[\\w\\x{200C}]+"_q,
		QRegularExpression::UseUnicodePropertiesOption);
	static const auto mentionRegex = QRegularExpression(
		u"@[A-Za-z0-9_]{4,}"_q);
	static const auto linkRegex = QRegularExpression(
		u"(?:https?://)?(?:t\\.me|telegram\\.me|[a-z0-9-]+\\.[a-z]{2,})(?:/[^\\s]*)?"_q,
		QRegularExpression::CaseInsensitiveOption);
	static const auto wordRegex = QRegularExpression(
		u"[\\w\\x{200C}]{3,}"_q,
		QRegularExpression::UseUnicodePropertiesOption);

	auto streakPeer = PeerId();
	auto streak = 0;
	for (const auto &message : messages) {
		// Replies are counted for the whole chat, the filter applies to
		// the author of the message that got them.
		if (message.replyTo) {
			const auto i = byId.find(message.replyTo);
			const auto target = (i != end(byId)) ? i->second : nullptr;
			if (!filter || (target && target->from == filter)) {
				++repliesTo[message.replyTo];
				++result.totalReplies;
				if (target) {
					++repliedUsers[target->from];
				}
			}
			if (!filter || message.from == filter) {
				++replyingUsers[message.from];
			}
		}
		if (filter && message.from != filter) {
			continue;
		}
		if (message.service) {
			++result.service;
			continue;
		}
		++result.total;
		++senders[message.from];
		if (message.forwarded) {
			++result.forwarded;
		}
		if (message.media != MediaKind::None) {
			++result.withMedia;
		}
		++media[message.media];

		const auto local = base::unixtime::parse(message.date);
		++result.hours[std::clamp(local.time().hour(), 0, 23)];
		++result.weekdays[std::clamp(local.date().dayOfWeek() - 1, 0, 6)];
		++days[local.date()];

		if (message.from == streakPeer) {
			++streak;
		} else {
			streakPeer = message.from;
			streak = 1;
		}
		if (streak > result.longestStreak) {
			result.longestStreak = streak;
			result.longestStreakPeer = streakPeer;
		}

		const auto reactions = message.reactionsTotal();
		if (reactions > 0) {
			++result.reactedMessages;
			result.totalReactions += reactions;
			reactedUsers[message.from] += reactions;
			result.topReacted.push_back(Score(message, reactions));
			for (const auto &reaction : message.reactions) {
				auto &total = reactionTotals[ReactionKey(reaction)];
				total.emoji = reaction.emoji;
				total.customId = reaction.customId;
				total.count += reaction.count;
			}
		}
		if (message.views > 0) {
			result.topViewed.push_back(Score(message, message.views));
		}
		if (message.forwards > 0) {
			result.topForwarded.push_back(Score(message, message.forwards));
		}
		if (message.repliesCount > 0) {
			result.topComments.push_back(
				Score(message, message.repliesCount));
		}

		if (!message.text.isEmpty()) {
			auto it = hashtagRegex.globalMatch(message.text);
			while (it.hasNext()) {
				++hashtags[it.next().captured().toLower()];
			}
			it = mentionRegex.globalMatch(message.text);
			while (it.hasNext()) {
				++mentions[it.next().captured().toLower()];
			}
			it = linkRegex.globalMatch(message.text);
			while (it.hasNext()) {
				auto link = it.next().captured().toLower();
				link.remove(QRegularExpression(u"^https?://"_q));
				const auto slash = link.indexOf('/');
				const auto host = (slash > 0) ? link.left(slash) : link;
				if (host.contains('.') && !host.endsWith('.')) {
					++links[host.startsWith(u"t.me"_q)
						? link.left(64)
						: host];
				}
			}
			auto seen = base::flat_set<QString>();
			it = wordRegex.globalMatch(message.text);
			while (it.hasNext()) {
				const auto word = it.next().captured().toLower();
				if (!StopWords().contains(word)
					&& !word.front().isDigit()
					&& seen.emplace(word).second) {
					++words[word];
				}
			}
		}
	}

	for (const auto &[id, count] : repliesTo) {
		const auto i = byId.find(id);
		if (i != end(byId)) {
			result.topReplied.push_back(Score(*i->second, count));
		} else {
			// The original message is older than the period.
			result.topReplied.push_back({
				.id = id,
				.text = Tr("(earlier message)", "(پیام قدیمی‌تر از بازه)"),
				.value = count,
			});
		}
	}
	KeepTop(result.topReplied, top);
	KeepTop(result.topReacted, top);
	KeepTop(result.topViewed, top);
	KeepTop(result.topForwarded, top);
	KeepTop(result.topComments, top);

	result.senders = int(senders.size());
	result.topSenders = TopPeers(senders, top);
	result.mostRepliedUsers = TopPeers(repliedUsers, top);
	result.mostReplyingUsers = TopPeers(replyingUsers, top);
	result.mostReactedUsers = TopPeers(reactedUsers, top);

	for (const auto &[key, reaction] : reactionTotals) {
		result.reactionTotals.push_back(reaction);
	}
	ranges::sort(
		result.reactionTotals,
		ranges::greater(),
		&ReactionCount::count);

	for (const auto &[kind, count] : media) {
		result.media.emplace_back(kind, count);
	}
	ranges::sort(result.media, [](const auto &a, const auto &b) {
		return a.second > b.second;
	});
	for (const auto &[date, count] : days) {
		result.days.emplace_back(date, count);
	}
	result.hashtags = TopWords(hashtags, top);
	result.mentions = TopWords(mentions, top);
	result.links = TopWords(links, top);
	result.words = TopWords(words, top * 2, 2);
	return result;
}

QString ToText(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		const Stats &stats) {
	const auto name = [&](PeerId id) {
		return AyuTools::PeerName(session, id);
	};
	auto lines = QStringList();
	lines.push_back(Tr("Statistics of ", "آمار ") + peer->name());
	lines.push_back(Tr("Period: ", "بازه: ")
		+ AyuTools::FormatDate(stats.from)
		+ u" — "_q
		+ AyuTools::FormatDate(stats.till));
	if (stats.filter) {
		lines.push_back(Tr("Only messages of: ", "فقط پیام‌های: ")
			+ name(stats.filter));
	}
	lines.push_back(Tr("Messages: %1, members who wrote: %2, replies: %3, "
		"reactions: %4",
		"پیام‌ها: %1، افراد فعال: %2، ریپلای‌ها: %3، ری‌اکشن‌ها: %4"
	).arg(stats.total).arg(stats.senders).arg(stats.totalReplies).arg(
		stats.totalReactions));

	const auto addPeers = [&](
			const QString &title,
			const std::vector<PeerScore> &list) {
		if (list.empty()) {
			return;
		}
		lines.push_back(QString());
		lines.push_back(title);
		auto index = 0;
		for (const auto &entry : list) {
			lines.push_back(QString::number(++index)
				+ u". "_q
				+ name(entry.peer)
				+ u" — "_q
				+ QString::number(entry.value));
		}
	};
	const auto addMessages = [&](
			const QString &title,
			const std::vector<MessageScore> &list,
			bool withReactions = false) {
		if (list.empty()) {
			return;
		}
		lines.push_back(QString());
		lines.push_back(title);
		auto index = 0;
		for (const auto &entry : list) {
			auto line = QString::number(++index)
				+ u". ("_q
				+ QString::number(entry.value)
				+ u") "_q
				+ (entry.from ? (name(entry.from) + u": "_q) : QString())
				+ entry.text;
			if (withReactions && !entry.reactions.empty()) {
				line += u" ["_q + ReactionsLine(entry.reactions) + u"]"_q;
			}
			lines.push_back(line);
		}
	};
	addPeers(Tr("Most active members:", "فعال‌ترین افراد:"), stats.topSenders);
	addMessages(Tr("Most replied messages:", "پرریپلای‌ترین پیام‌ها:"), stats.topReplied);
	addMessages(Tr("Most reacted messages:", "پرری‌اکشن‌ترین پیام‌ها:"), stats.topReacted, true);
	if (!stats.reactionTotals.empty()) {
		lines.push_back(QString());
		lines.push_back(Tr("Reactions used:", "ری‌اکشن‌های استفاده‌شده:"));
		lines.push_back(ReactionsLine(stats.reactionTotals, 15));
	}
	addPeers(Tr("Got the most replies:", "بیشترین ریپلای گرفته‌ها:"), stats.mostRepliedUsers);
	addPeers(Tr("Replied the most:", "بیشترین ریپلای زده‌ها:"), stats.mostReplyingUsers);
	addPeers(Tr("Got the most reactions:", "بیشترین ری‌اکشن گرفته‌ها:"), stats.mostReactedUsers);
	addMessages(Tr("Most viewed posts:", "پربازدیدترین پست‌ها:"), stats.topViewed);
	addMessages(Tr("Most forwarded posts:", "پرفورواردترین پست‌ها:"), stats.topForwarded);
	addMessages(Tr("Most commented posts:", "پرکامنت‌ترین پست‌ها:"), stats.topComments);

	auto busiest = 0;
	for (auto hour = 0; hour != 24; ++hour) {
		if (stats.hours[hour] > stats.hours[busiest]) {
			busiest = hour;
		}
	}
	if (stats.total > 0) {
		lines.push_back(QString());
		lines.push_back(Tr("Busiest hour: %1:00 (%2 messages)",
			"شلوغ‌ترین ساعت: %1:00 (%2 پیام)"
		).arg(busiest).arg(stats.hours[busiest]));
	}
	const auto addWords = [&](
			const QString &title,
			const std::vector<WordScore> &list) {
		if (list.empty()) {
			return;
		}
		auto parts = QStringList();
		for (const auto &entry : list) {
			parts.push_back(entry.word
				+ u" ("_q
				+ QString::number(entry.value)
				+ u")"_q);
		}
		lines.push_back(QString());
		lines.push_back(title + ' ' + parts.join(u", "_q));
	};
	addWords(Tr("Hashtags:", "هشتگ‌ها:"), stats.hashtags);
	addWords(Tr("Mentions:", "منشن‌ها:"), stats.mentions);
	addWords(Tr("Links:", "لینک‌ها:"), stats.links);
	addWords(Tr("Frequent words:", "کلمات پرتکرار:"), stats.words);
	return lines.join('\n');
}

} // namespace AyuStats
