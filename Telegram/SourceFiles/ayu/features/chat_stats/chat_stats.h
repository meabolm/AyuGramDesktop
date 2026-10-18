// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
//
// Statistics of a chat over a period: replies, reactions, activity.
#pragma once

#include "ayu/features/ayu_tools/history_loader.h"

namespace AyuStats {

using AyuTools::MediaKind;
using AyuTools::RawMessage;
using AyuTools::ReactionCount;

struct MessageScore {
	MsgId id = 0;
	PeerId from = 0;
	TimeId date = 0;
	QString text;
	int value = 0;
	std::vector<ReactionCount> reactions;
};

struct PeerScore {
	PeerId peer = 0;
	int value = 0;
};

struct WordScore {
	QString word;
	int value = 0;
};

struct Stats {
	TimeId from = 0;
	TimeId till = 0;
	PeerId filter = 0;
	bool truncated = false;

	int total = 0;
	int service = 0;
	int forwarded = 0;
	int withMedia = 0;
	int senders = 0;
	int totalReplies = 0;
	int totalReactions = 0;
	int reactedMessages = 0;
	int longestStreak = 0; // Most messages in a row by one sender.
	PeerId longestStreakPeer = 0;

	std::vector<std::pair<MediaKind, int>> media;
	std::vector<PeerScore> topSenders;
	std::vector<MessageScore> topReplied;
	std::vector<PeerScore> mostRepliedUsers; // Got the most replies.
	std::vector<PeerScore> mostReplyingUsers; // Replied the most.
	std::vector<MessageScore> topReacted;
	std::vector<ReactionCount> reactionTotals;
	std::vector<PeerScore> mostReactedUsers; // Got the most reactions.
	std::vector<MessageScore> topViewed;
	std::vector<MessageScore> topForwarded;
	std::vector<MessageScore> topComments;
	std::array<int, 24> hours = { { 0 } };
	std::array<int, 7> weekdays = { { 0 } }; // Monday first.
	std::vector<std::pair<QDate, int>> days;
	std::vector<WordScore> hashtags;
	std::vector<WordScore> mentions;
	std::vector<WordScore> words;
	std::vector<WordScore> links;
};

[[nodiscard]] Stats Compute(
	const std::vector<RawMessage> &messages,
	TimeId from,
	TimeId till,
	PeerId filter = 0,
	int top = 10);

// A plain text report (for copying or for the AI).
[[nodiscard]] QString ToText(
	not_null<Main::Session*> session,
	not_null<PeerData*> peer,
	const Stats &stats);

[[nodiscard]] QString ReactionText(const ReactionCount &reaction);
[[nodiscard]] QString ReactionsLine(
	const std::vector<ReactionCount> &reactions,
	int limit = 6);

} // namespace AyuStats
