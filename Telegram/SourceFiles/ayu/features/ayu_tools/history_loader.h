// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
//
// Loads raw chat history from the server, independently of what is shown.
#pragma once

namespace Main {
class Session;
} // namespace Main

namespace AyuTools {

enum class MediaKind : uchar {
	None,
	Photo,
	Video,
	Gif,
	Voice,
	VideoNote,
	Sticker,
	Audio,
	File,
	Link,
	Poll,
	Location,
	Contact,
	Other,
};

struct ReactionCount {
	QString emoji; // Empty for custom emoji.
	uint64 customId = 0;
	int count = 0;
	bool chosen = false;
};

struct RawMessage {
	MsgId id = 0;
	TimeId date = 0;
	PeerId from = 0;
	QString text;
	MediaKind media = MediaKind::None;
	MsgId replyTo = 0;
	bool out = false;
	bool service = false;
	bool forwarded = false;
	int repliesCount = -1; // Channel comments / thread replies, if known.
	int views = -1;
	int forwards = -1;
	std::vector<ReactionCount> reactions;

	[[nodiscard]] int reactionsTotal() const;
};

struct LoadProgress {
	int loaded = 0;
	TimeId oldestDate = 0;
};

// Loads messages with date in [from, till] (newest first), at most `limit`.
// `done` gets the messages sorted from old to new; `failed` gets an error.
// Returns a function that cancels the loading.
Fn<void()> LoadHistory(
	not_null<Main::Session*> session,
	not_null<PeerData*> peer,
	TimeId from,
	TimeId till,
	int limit,
	Fn<void(LoadProgress)> progress,
	Fn<void(std::vector<RawMessage>)> done,
	Fn<void(QString)> failed);

// Loads the last `limit` messages (sorted from old to new).
void LoadRecent(
	not_null<Main::Session*> session,
	not_null<PeerData*> peer,
	int limit,
	Fn<void(std::vector<RawMessage>)> done,
	Fn<void(QString)> failed = nullptr);

// Loads the scheduled messages of a chat (sorted by date).
void LoadScheduled(
	not_null<Main::Session*> session,
	not_null<PeerData*> peer,
	Fn<void(std::vector<RawMessage>)> done,
	Fn<void(QString)> failed = nullptr);

[[nodiscard]] QString PeerName(
	not_null<Main::Session*> session,
	PeerId peerId);
[[nodiscard]] QString MediaLabel(MediaKind kind);
[[nodiscard]] QString ShortText(const RawMessage &message, int limit = 80);

// "Name: text" lines for prompts.
[[nodiscard]] QString FormatForPrompt(
	not_null<Main::Session*> session,
	const std::vector<RawMessage> &messages,
	int maxChars = 12000);

} // namespace AyuTools
