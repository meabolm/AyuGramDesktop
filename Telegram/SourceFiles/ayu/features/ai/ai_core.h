// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
//
// AI services (OpenAI-compatible chat APIs) and AI tasks in chats.
#pragma once

namespace Main {
class Session;
} // namespace Main

namespace AyuAi {

enum class ProviderKind : uchar {
	DeepSeek,
	OpenAI,
	Gemini,
	Claude,
	Grok,
	Groq,
	OpenRouter,
	Mistral,
	Qwen,
	Kimi,
	Ollama,
	Custom,
};

struct Preset {
	ProviderKind kind = ProviderKind::Custom;
	QString name;
	QString baseUrl;
	QString model;
	QStringList models;
	QString keysUrl;
	bool vision = false;
};
[[nodiscard]] const std::vector<Preset> &Presets();
[[nodiscard]] const Preset &PresetFor(ProviderKind kind);

struct Provider {
	uint64 id = 0;
	ProviderKind kind = ProviderKind::DeepSeek;
	QString name;
	QString baseUrl;
	QString apiKey;
	QString model;
	bool vision = false;
};
[[nodiscard]] std::vector<Provider> Providers();
[[nodiscard]] std::optional<Provider> FindProvider(uint64 id); // 0: default.
[[nodiscard]] uint64 DefaultProviderId();
void SetDefaultProvider(uint64 id);
uint64 SaveProvider(Provider provider); // Returns the id.
void RemoveProvider(uint64 id);

struct ChatMessage {
	QString role; // "user" or "assistant".
	QString text;
	QByteArray imageJpeg;
};

struct Request {
	uint64 providerId = 0;
	QString system;
	std::vector<ChatMessage> messages;
	double temperature = 0.7;
	int maxTokens = 1500;
};

// Removes spaces, quotes and invisible characters from a pasted key.
[[nodiscard]] QString CleanApiKey(QString key);

// Returns a function cancelling the request.
Fn<void()> Complete(
	Request request,
	Fn<void(QString)> done,
	Fn<void(QString)> failed);
Fn<void()> CompleteWith(
	Provider provider,
	Request request,
	Fn<void(QString)> done,
	Fn<void(QString)> failed);

enum class TaskKind : uchar {
	AutoReply,
	HotReply,
	CopyReaction,
	Post,
};

struct Task {
	uint64 id = 0;
	bool enabled = true;
	uint64 account = 0;
	PeerId peer = 0;
	MsgId topicRootId = 0;
	TaskKind kind = TaskKind::AutoReply;
	uint64 providerId = 0;
	QString title;
	QString instructions;
	int contextCount = 30;
	int maxPerHour = 6;
	bool onlyWhenAddressed = false; // AutoReply: mentions / replies to me.
	int minReplies = 5; // HotReply.
	int minReactions = 5; // CopyReaction.
	int timeOfDay = 9 * 60; // Post: minutes since midnight.
	bool daily = true; // Post: every day, otherwise once.
	int intervalMinutes = 0; // Post: repeat every N minutes if set.
	TimeId runAt = 0; // Post, once: the time to run.
	bool toSavedMessages = false; // Post: send to my Saved Messages.
	TimeId lastRun = 0;
};
[[nodiscard]] std::vector<Task> Tasks();
[[nodiscard]] std::vector<Task> TasksFor(uint64 account, PeerId peer);
[[nodiscard]] std::optional<Task> FindTask(uint64 id);
uint64 SaveTask(Task task); // Returns the id.
void RemoveTask(uint64 id);
[[nodiscard]] rpl::producer<> Changes();

// Runs the task right away, `report` gets a short status for the user.
void RunTaskNow(uint64 id, Fn<void(QString)> report);
[[nodiscard]] QString LastStatus(uint64 taskId);

[[nodiscard]] QString TaskKindName(TaskKind kind);
[[nodiscard]] QString TaskKindAbout(TaskKind kind);
[[nodiscard]] QString TaskSummary(const Task &task);

// Base instructions for all requests.
[[nodiscard]] QString BaseSystemPrompt();

void Init();

} // namespace AyuAi
