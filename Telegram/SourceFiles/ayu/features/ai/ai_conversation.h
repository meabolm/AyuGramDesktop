// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
//
// AI conversations: several of my accounts talk in a chat, each with its
// own AI service and its own role.
#pragma once

namespace AyuConversation {

struct Participant {
	uint64 account = 0; // Session unique id.
	uint64 providerId = 0; // 0 means the default service.
	QString persona; // Role, opinion, side, tone.
};

struct Conversation {
	uint64 id = 0;
	bool enabled = true;
	QString title;
	PeerId peer = 0;
	MsgId topicRootId = 0;
	QString topic; // What to talk about and the rules.
	std::vector<Participant> participants;
	int minGap = 60; // Seconds between messages.
	int maxGap = 180;
	int maxMessages = 30; // 0 means no limit.
	bool showService = false; // Prefix messages with the service name.
	int sent = 0;
	TimeId nextAt = 0;
	int lastSpeaker = -1;
};

[[nodiscard]] std::vector<Conversation> List();
[[nodiscard]] std::optional<Conversation> Find(uint64 id);
uint64 Save(Conversation conversation); // Returns the id.
void Remove(uint64 id);
void Restart(uint64 id); // Enables, resets the counter, starts now.
void StopAll();
[[nodiscard]] rpl::producer<> Changes();
[[nodiscard]] QString Status(uint64 id);
[[nodiscard]] QString Summary(const Conversation &conversation);

void Init();

} // namespace AyuConversation
