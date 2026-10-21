// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ui/ayu_tools/ayu_tools_boxes.h"

#include "ayu/features/account_groups/account_groups.h"
#include "ayu/features/ai/ai_conversation.h"
#include "ayu/features/ai/ai_core.h"
#include "ayu/features/ai/ai_multi_reply.h"
#include "base/call_delayed.h"
#include "base/random.h"
#include "ayu/features/ayu_tools/ayu_tools.h"
#include "ayu/features/ayu_tools/history_loader.h"
#include "ayu/features/chat_stats/chat_stats.h"
#include "ayu/features/repeat_schedule/repeat_schedule.h"
#include "base/unixtime.h"
#include "data/data_file_origin.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/boxes/choose_date_time.h"
#include "ui/boxes/confirm_box.h"
#include "ui/image/image.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/text/text_utilities.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_ayu_tools.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QBuffer>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

namespace AyuToolsUi {
namespace {

using AyuAccountGroups::Tr;
using AyuAi::Provider;
using AyuAi::Task;
using AyuAi::TaskKind;

constexpr auto kDay = TimeId(86400);

[[nodiscard]] rpl::producer<QString> P(const QString &text) {
	return rpl::single(text);
}

not_null<Ui::FlatLabel*> AddLabel(
		not_null<Ui::VerticalLayout*> container,
		const QString &text,
		const style::FlatLabel &st = st::ayuToolsHint) {
	const auto result = container->add(
		object_ptr<Ui::FlatLabel>(container, text, st),
		st::boxRowPadding + QMargins(0, st::ayuToolsSkip / 2, 0, 0));
	result->setSelectable(true);
	return result;
}

not_null<Ui::SettingsButton*> AddRowButton(
		not_null<Ui::VerticalLayout*> container,
		const QString &text,
		Fn<void()> click) {
	const auto result = container->add(object_ptr<Ui::SettingsButton>(
		container,
		P(text),
		st::ayuToolsRow));
	result->setClickedCallback(std::move(click));
	return result;
}

not_null<Ui::SettingsButton*> AddRowButton(
		not_null<Ui::VerticalLayout*> container,
		std::shared_ptr<rpl::variable<QString>> text,
		Fn<void()> click) {
	const auto result = container->add(object_ptr<Ui::SettingsButton>(
		container,
		text->value(),
		st::ayuToolsRow));
	result->lifetime().add([text] {});
	result->setClickedCallback(std::move(click));
	return result;
}

void AddTitle(
		not_null<Ui::VerticalLayout*> container,
		const QString &text) {
	Ui::AddSkip(container, st::ayuToolsSkip);
	Ui::AddSubsectionTitle(container, P(text));
}

not_null<Ui::InputField*> AddField(
		not_null<Ui::VerticalLayout*> container,
		const QString &placeholder,
		const QString &value,
		bool multiline = false,
		const style::InputField &st = st::ayuToolsField) {
	return container->add(
		object_ptr<Ui::InputField>(
			container,
			st,
			multiline
				? Ui::InputField::Mode::MultiLine
				: Ui::InputField::Mode::SingleLine,
			P(placeholder),
			value),
		st::boxRowPadding + QMargins(0, st::ayuToolsSkip, 0, 0));
}

not_null<Ui::Checkbox*> AddCheck(
		not_null<Ui::VerticalLayout*> container,
		const QString &text,
		bool checked) {
	return container->add(
		object_ptr<Ui::Checkbox>(
			container,
			text,
			checked,
			st::defaultBoxCheckbox),
		st::boxRowPadding + QMargins(0, st::ayuToolsSkip, 0, 0));
}

[[nodiscard]] int FieldInt(
		Ui::InputField *field,
		int fallback,
		int min,
		int max) {
	if (!field) {
		return fallback;
	}
	auto ok = false;
	const auto value = field->getLastText().trimmed().toInt(&ok);
	return ok ? std::clamp(value, min, max) : fallback;
}

[[nodiscard]] QString ShortLine(QString text, int limit = 60) {
	text = text.simplified();
	if (text.size() > limit) {
		text = text.mid(0, limit - 1) + QString::fromUtf8("…");
	}
	return text;
}

void Copy(std::shared_ptr<Ui::Show> show, const QString &text) {
	QGuiApplication::clipboard()->setText(text);
	show->showToast(Tr("Copied.", "کپی شد."));
}

// A popup menu that lives until hidden.
[[nodiscard]] not_null<Ui::PopupMenu*> MakeMenu(not_null<QWidget*> parent) {
	static auto holder = base::unique_qptr<Ui::PopupMenu>();
	holder = base::make_unique_q<Ui::PopupMenu>(
		parent.get(),
		st::popupMenuWithIcons);
	return holder.get();
}

[[nodiscard]] QString ProviderTitle(const Provider &provider) {
	const auto isDefault = (provider.id == AyuAi::DefaultProviderId());
	return provider.name
		+ u" · "_q
		+ provider.model
		+ (isDefault ? QString::fromUtf8(" ✓") : QString());
}

[[nodiscard]] QString ProviderName(uint64 id) {
	if (!id) {
		const auto def = AyuAi::FindProvider(0);
		return Tr("Default", "پیش‌فرض")
			+ (def ? (u" ("_q + def->name + ')') : QString());
	}
	const auto provider = AyuAi::FindProvider(id);
	return provider ? provider->name : Tr("Default", "پیش‌فرض");
}

// Button "Service: X" that lets to choose a provider (0 is default).
void AddProviderChooser(
		not_null<Ui::VerticalLayout*> container,
		std::shared_ptr<uint64> providerId,
		bool allowDefault) {
	if (AyuAi::Providers().size() < 2 && allowDefault) {
		return;
	}
	const auto label = [=] {
		return Tr("AI service: ", "سرویس هوش مصنوعی: ")
			+ ProviderName(*providerId);
	};
	const auto text = std::make_shared<rpl::variable<QString>>(label());
	const auto button = AddRowButton(container, text, nullptr);
	button->setClickedCallback([=] {
		const auto menu = MakeMenu(button);
		const auto choose = [=](uint64 id) {
			*providerId = id;
			*text = label();
		};
		if (allowDefault) {
			menu->addAction(Tr("Default", "پیش‌فرض"), [=] { choose(0); });
		}
		for (const auto &provider : AyuAi::Providers()) {
			const auto id = provider.id;
			menu->addAction(ProviderTitle(provider), [=] { choose(id); });
		}
		menu->popup(QCursor::pos());
	});
}

[[nodiscard]] QByteArray ToJpeg(QImage image) {
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

//
// AI services.
//

void ProviderEditBox(
		not_null<Ui::GenericBox*> box,
		std::shared_ptr<Ui::Show> show,
		Provider provider) {
	const auto preset = AyuAi::PresetFor(provider.kind);
	box->setTitle(P(provider.id
		? Tr("Edit AI service", "ویرایش سرویس")
		: Tr("New AI service", "سرویس جدید")));
	box->setWidth(st::boxWideWidth);
	const auto content = box->verticalLayout();

	const auto name = AddField(content, Tr("Name", "نام"), provider.name);
	const auto url = AddField(
		content,
		Tr("API address (base URL)", "آدرس API"),
		provider.baseUrl);
	const auto key = AddField(
		content,
		Tr("API key", "کلید API"),
		provider.apiKey);
	const auto model = AddField(
		content,
		Tr("Model", "مدل"),
		provider.model);

	if (!preset.models.isEmpty()) {
		const auto choose = AddRowButton(
			content,
			Tr("Choose a model…", "انتخاب مدل…"),
			nullptr);
		choose->setClickedCallback([=] {
			const auto menu = MakeMenu(choose);
			for (const auto &value : preset.models) {
				menu->addAction(value, [=] { model->setText(value); });
			}
			menu->popup(QCursor::pos());
		});
	}
	if (!preset.keysUrl.isEmpty()) {
		auto text = TextWithEntities{ Tr("Get a key here: ", "گرفتن کلید از اینجا: ") };
		text.append(Ui::Text::Link(preset.keysUrl, preset.keysUrl));
		content->add(
			object_ptr<Ui::FlatLabel>(
				content,
				rpl::single(text),
				st::ayuToolsHint),
			st::boxRowPadding + QMargins(0, st::ayuToolsSkip, 0, 0));
	}
	const auto vision = AddCheck(
		content,
		Tr("Can see images", "می‌تونه عکس ببینه"),
		provider.vision);
	const auto isDefault = AddCheck(
		content,
		Tr("Use by default", "سرویس پیش‌فرض باشه"),
		!provider.id
			|| !AyuAi::DefaultProviderId()
			|| provider.id == AyuAi::DefaultProviderId());
	const auto status = AddLabel(
		content,
		Tr(
			"The key is saved only on this computer.",
			"کلید فقط روی همین کامپیوتر ذخیره میشه."));
	Ui::AddSkip(content, st::ayuToolsSkip);

	const auto collect = [=] {
		auto result = provider;
		result.name = name->getLastText().trimmed();
		result.baseUrl = url->getLastText().trimmed();
		result.apiKey = AyuAi::CleanApiKey(key->getLastText());
		result.model = model->getLastText().trimmed();
		result.vision = vision->checked();
		if (result.name.isEmpty()) {
			result.name = preset.name;
		}
		return result;
	};
	const auto cancelTest = std::make_shared<Fn<void()>>();
	box->lifetime().add([=] {
		if (*cancelTest) {
			(*cancelTest)();
		}
	});

	box->addButton(tr::lng_settings_save(), [=] {
		const auto value = collect();
		if (value.baseUrl.isEmpty()) {
			url->showError();
			return;
		} else if (value.apiKey.contains('*')) {
			key->showError();
			status->setText(Tr(
				"This key is hidden with *** and can't work. Create a new key "
				"on the service site and copy it right away.",
				"این کلید با *** مخفی شده و کار نمی‌کنه. توی سایت سرویس یه "
				"کلید جدید بساز و همون لحظه کپیش کن."));
			return;
		} else if (value.model.isEmpty()) {
			model->showError();
			return;
		}
		const auto id = AyuAi::SaveProvider(value);
		if (isDefault->checked()) {
			AyuAi::SetDefaultProvider(id);
		}
		box->closeBox();
	});
	box->addButton(P(Tr("Test", "تست")), [=] {
		status->setText(Tr("Testing…", "در حال تست…"));
		*cancelTest = AyuAi::CompleteWith(
			collect(),
			AyuAi::Request{
				.messages = { { .role = u"user"_q, .text = u"Reply with one word: OK"_q } },
				.temperature = 0.,
				.maxTokens = 50,
			},
			crl::guard(box, [=](QString answer) {
				status->setText(QString::fromUtf8("✅ ")
					+ Tr("Works! Answer: ", "کار می‌کنه! جواب: ")
					+ ShortLine(answer, 80));
			}),
			crl::guard(box, [=](QString error) {
				const auto sent = collect().apiKey;
				const auto masked = (sent.size() > 10)
					? (sent.left(5) + QString::fromUtf8("…") + sent.right(4))
					: sent;
				status->setText(QString::fromUtf8("❌ ")
					+ error
					+ u"\n\n"_q
					+ Tr("Key that was sent: ", "کلیدی که فرستاده شد: ")
					+ masked
					+ Tr(" (length ", " (طول ")
					+ QString::number(sent.size())
					+ u")"_q);
			}));
	});
	if (provider.id) {
		const auto id = provider.id;
		box->addLeftButton(P(Tr("Delete", "حذف")), [=] {
			AyuAi::RemoveProvider(id);
			box->closeBox();
		});
	}
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

void PresetsBox(
		not_null<Ui::GenericBox*> box,
		std::shared_ptr<Ui::Show> show) {
	box->setTitle(P(Tr("Choose a service", "انتخاب سرویس")));
	const auto content = box->verticalLayout();
	for (const auto &preset : AyuAi::Presets()) {
		const auto kind = preset.kind;
		AddRowButton(content, preset.name, [=] {
			const auto &p = AyuAi::PresetFor(kind);
			box->closeBox();
			show->showBox(Box(ProviderEditBox, show, Provider{
				.kind = kind,
				.name = p.name,
				.baseUrl = p.baseUrl,
				.model = p.model,
				.vision = p.vision,
			}));
		});
	}
	AddLabel(
		content,
		Tr(
			"DeepSeek is cheap and good. Gemini and Groq have free plans. "
			"Ollama runs models on your own computer.",
			"دیپ‌سیک ارزون و خوبه. جمینای و Groq پلن رایگان دارن. "
			"Ollama مدل رو روی کامپیوتر خودت اجرا می‌کنه."));
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

void ProvidersBox(
		not_null<Ui::GenericBox*> box,
		std::shared_ptr<Ui::Show> show) {
	box->setTitle(P(Tr("AI services", "سرویس‌های هوش مصنوعی")));
	box->setWidth(st::boxWideWidth);
	const auto list = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		style::margins());
	const auto rebuild = [=] {
		list->clear();
		const auto providers = AyuAi::Providers();
		if (providers.empty()) {
			AddLabel(
				list,
				Tr(
					"No services yet. Press \"Add\" and paste your API key.",
					"هنوز سرویسی نداری. «افزودن» رو بزن و کلید API رو وارد کن."));
		}
		for (const auto &provider : providers) {
			AddRowButton(list, ProviderTitle(provider), [=] {
				show->showBox(Box(ProviderEditBox, show, provider));
			});
		}
		AddLabel(
			list,
			Tr(
				"✓ marks the default service. Keys are saved only on this "
				"computer. AI tasks and the AI button use these services.",
				"✓ یعنی سرویس پیش‌فرض. کلیدها فقط روی همین کامپیوتر ذخیره "
				"میشن. وظیفه‌های هوش مصنوعی و دکمه کنار ارسال از اینا استفاده می‌کنن."));
		Ui::AddSkip(list, st::ayuToolsSkip);
		list->resizeToWidth(st::boxWideWidth);
	};
	rebuild();
	AyuAi::Changes() | rpl::on_next(rebuild, box->lifetime());

	box->addButton(P(Tr("Add", "افزودن")), [=] {
		show->showBox(Box(PresetsBox, show));
	});
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

//
// AI tasks.
//

struct TaskOptions {
	Ui::Checkbox *addressed = nullptr;
	Ui::InputField *context = nullptr;
	Ui::InputField *perHour = nullptr;
	Ui::InputField *minReplies = nullptr;
	Ui::InputField *minReactions = nullptr;
	Ui::InputField *time = nullptr;
	Ui::Checkbox *daily = nullptr;
	Ui::Checkbox *saved = nullptr;
	Ui::InputField *interval = nullptr;
};

[[nodiscard]] QString TimeText(int minutes) {
	return u"%1:%2"_q
		.arg(minutes / 60, 2, 10, QChar('0'))
		.arg(minutes % 60, 2, 10, QChar('0'));
}

[[nodiscard]] std::optional<int> ParseTime(const QString &text) {
	const auto parts = text.trimmed().split(':');
	if (parts.size() != 2) {
		return std::nullopt;
	}
	auto ok1 = false, ok2 = false;
	const auto h = parts[0].toInt(&ok1);
	const auto m = parts[1].toInt(&ok2);
	if (!ok1 || !ok2 || h < 0 || h > 23 || m < 0 || m > 59) {
		return std::nullopt;
	}
	return h * 60 + m;
}

[[nodiscard]] QString InstructionsPlaceholder(TaskKind kind) {
	switch (kind) {
	case TaskKind::AutoReply: return Tr(
		"How to answer? E.g.: answer questions about programming, friendly and short.",
		"چطور جواب بده؟ مثلا: به سوال‌های برنامه‌نویسی جواب بده، دوستانه و کوتاه.");
	case TaskKind::HotReply: return Tr(
		"How to reply to popular messages? E.g.: make a short joke.",
		"روی پیام‌های پرطرفدار چی بنویسه؟ مثلا: یه شوخی کوتاه بکن.");
	case TaskKind::CopyReaction: return Tr(
		"Optional. E.g.: never react to messages of admins.",
		"اختیاری. مثلا: روی پیام ادمین‌ها ری‌اکشن نزن.");
	case TaskKind::Post: return Tr(
		"What to write? E.g.: summarize today's messages, or say good morning with a joke.",
		"چی بنویسه؟ مثلا: پیام‌های امروز رو خلاصه کن، یا صبح بخیر بگو با یه جوک.");
	}
	return QString();
}

void TaskEditBox(
		not_null<Ui::GenericBox*> box,
		std::shared_ptr<Ui::Show> show,
		not_null<PeerData*> peer,
		Task task) {
	const auto state = std::make_shared<Task>(task);
	if (!state->account) {
		state->account = peer->session().uniqueId();
		state->peer = peer->id;
	}
	box->setTitle(P(task.id
		? Tr("Edit AI task", "ویرایش وظیفه")
		: Tr("New AI task", "وظیفه جدید")));
	box->setWidth(st::boxWideWidth);
	const auto content = box->verticalLayout();

	AddLabel(content, Tr("Chat: ", "چت: ") + peer->name());
	AddTitle(content, Tr("What should it do?", "چیکار کنه؟"));
	const auto group = std::make_shared<Ui::RadioenumGroup<TaskKind>>(
		state->kind);
	for (const auto kind : {
			TaskKind::AutoReply,
			TaskKind::HotReply,
			TaskKind::CopyReaction,
			TaskKind::Post }) {
		content->add(
			object_ptr<Ui::Radioenum<TaskKind>>(
				content,
				group,
				kind,
				AyuAi::TaskKindName(kind),
				st::defaultBoxCheckbox),
			st::boxRowPadding + QMargins(0, st::ayuToolsSkip, 0, 0));
	}
	const auto about = AddLabel(content, AyuAi::TaskKindAbout(state->kind));

	const auto title = AddField(
		content,
		Tr("Title (optional)", "عنوان (اختیاری)"),
		state->title);
	const auto instructions = AddField(
		content,
		InstructionsPlaceholder(state->kind),
		state->instructions,
		true,
		st::ayuToolsBigField);

	const auto providerId = std::make_shared<uint64>(state->providerId);
	AddProviderChooser(content, providerId, true);

	AddTitle(content, Tr("Options", "تنظیمات"));
	const auto options = content->add(
		object_ptr<Ui::VerticalLayout>(content),
		style::margins());
	const auto fields = std::make_shared<TaskOptions>();

	const auto collectOptions = [=] {
		const auto &f = *fields;
		auto &t = *state;
		if (f.addressed) {
			t.onlyWhenAddressed = f.addressed->checked();
		}
		t.contextCount = FieldInt(f.context, t.contextCount, 0, 200);
		t.maxPerHour = FieldInt(f.perHour, t.maxPerHour, 1, 60);
		t.minReplies = FieldInt(f.minReplies, t.minReplies, 1, 1000);
		t.minReactions = FieldInt(f.minReactions, t.minReactions, 1, 1000);
		t.intervalMinutes = FieldInt(f.interval, t.intervalMinutes, 0, 10080);
		if (f.time) {
			if (const auto parsed = ParseTime(f.time->getLastText())) {
				t.timeOfDay = *parsed;
			}
		}
		if (f.daily) {
			t.daily = f.daily->checked();
		}
		if (f.saved) {
			t.toSavedMessages = f.saved->checked();
		}
	};
	const auto rebuild = std::make_shared<Fn<void()>>();
	*rebuild = [=] {
		*fields = TaskOptions();
		options->clear();
		auto &t = *state;
		const auto addContext = [&] {
			fields->context = AddField(
				options,
				Tr(
					"How many recent messages to read (context)",
					"چند پیام آخر رو بخونه (برای فهم موضوع)"),
				QString::number(t.contextCount));
		};
		const auto addPerHour = [&] {
			fields->perHour = AddField(
				options,
				Tr("At most times per hour", "حداکثر چند بار در ساعت"),
				QString::number(t.maxPerHour));
		};
		switch (t.kind) {
		case TaskKind::AutoReply:
			fields->addressed = AddCheck(
				options,
				Tr(
					"Only when someone mentions me or replies to me",
					"فقط وقتی کسی منشنم کنه یا روی پیامم ریپلای بزنه"),
				t.onlyWhenAddressed);
			addContext();
			addPerHour();
			break;
		case TaskKind::HotReply:
			fields->minReplies = AddField(
				options,
				Tr(
					"Reply when a message gets at least this many replies",
					"وقتی یه پیام حداقل این‌قدر ریپلای گرفت"),
				QString::number(t.minReplies));
			addContext();
			addPerHour();
			break;
		case TaskKind::CopyReaction:
			fields->minReactions = AddField(
				options,
				Tr(
					"React when a message gets at least this many reactions",
					"وقتی یه پیام حداقل این‌قدر ری‌اکشن گرفت"),
				QString::number(t.minReactions));
			addPerHour();
			break;
		case TaskKind::Post: {
			fields->interval = AddField(
				options,
				Tr(
					"Repeat every N minutes (0 = use the time below)",
					"هر چند دقیقه یه بار (۰ = طبق ساعت پایین)"),
				QString::number(t.intervalMinutes));
			fields->daily = AddCheck(
				options,
				Tr("Repeat every day", "هر روز تکرار بشه"),
				t.daily);
			if (t.daily) {
				fields->time = AddField(
					options,
					Tr("Time (HH:MM)", "ساعت (مثلا 09:30)"),
					TimeText(t.timeOfDay));
			} else {
				const auto whenText = [=] {
					return Tr("When: ", "زمان: ")
						+ (state->runAt
							? AyuTools::FormatDate(state->runAt)
								+ ' '
								+ AyuTools::FormatTime(state->runAt)
							: Tr("choose…", "انتخاب کن…"));
				};
				const auto whenValue = std::make_shared<rpl::variable<QString>>(
					whenText());
				const auto when = AddRowButton(options, whenValue, nullptr);
				when->setClickedCallback([=] {
					const auto now = base::unixtime::now();
					show->showBox(Box([=](not_null<Ui::GenericBox*> b) {
						Ui::ChooseDateTimeBox(b, {
							.title = P(Tr("Run at", "زمان اجرا")),
							.submit = tr::lng_settings_save(),
							.done = [=](TimeId value) {
								state->runAt = value;
								*whenValue = whenText();
								b->closeBox();
							},
							.min = [] {
								return base::unixtime::now() + 60;
							},
							.time = state->runAt ? state->runAt : (now + 3600),
							.max = [] {
								return base::unixtime::now() + 365 * kDay;
							},
						});
					}));
				});
			}
			fields->saved = AddCheck(
				options,
				Tr(
					"Send the result to my Saved Messages (not to the chat)",
					"نتیجه رو به پیام‌های ذخیره‌شده خودم بفرسته (نه به چت)"),
				t.toSavedMessages);
			addContext();
			fields->daily->checkedChanges(
			) | rpl::on_next([=] {
				collectOptions();
				crl::on_main(box, [=] { (*rebuild)(); });
			}, fields->daily->lifetime());
		} break;
		}
		Ui::AddSkip(options, st::ayuToolsSkip);
		options->resizeToWidth(st::boxWideWidth);
	};
	(*rebuild)();

	group->setChangedCallback([=](TaskKind kind) {
		collectOptions();
		state->kind = kind;
		about->setText(AyuAi::TaskKindAbout(kind));
		instructions->setPlaceholder(P(InstructionsPlaceholder(kind)));
		crl::on_main(box, [=] { (*rebuild)(); });
	});

	const auto enabled = AddCheck(
		content,
		Tr("Enabled", "فعال باشه"),
		state->enabled);
	AddLabel(
		content,
		Tr(
			"AI tasks work while AyuGram is open on this computer.",
			"وظیفه‌های هوش مصنوعی فقط وقتی ایوگرام روی این کامپیوتر بازه کار می‌کنن."));
	if (task.id) {
		const auto status = AyuAi::LastStatus(task.id);
		if (!status.isEmpty()) {
			AddLabel(content, Tr("Last result: ", "آخرین نتیجه: ") + status);
		}
	}
	Ui::AddSkip(content, st::ayuToolsSkip);

	const auto save = [=]() -> uint64 {
		collectOptions();
		auto &t = *state;
		t.title = title->getLastText().trimmed();
		t.instructions = instructions->getLastText().trimmed();
		t.providerId = *providerId;
		t.enabled = enabled->checked();
		if ((t.kind == TaskKind::AutoReply
			|| t.kind == TaskKind::HotReply
			|| t.kind == TaskKind::Post)
			&& t.instructions.isEmpty()) {
			instructions->showError();
			return 0;
		}
		if (t.kind == TaskKind::Post
			&& !t.daily
			&& !t.runAt
			&& t.intervalMinutes <= 0) {
			show->showToast(Tr("Choose the time.", "زمان رو انتخاب کن."));
			return 0;
		}
		if (!AyuAi::FindProvider(t.providerId)
			&& t.kind != TaskKind::CopyReaction) {
			show->showToast(Tr(
				"Add an AI service first (Settings > AyuGram > Other).",
				"اول یه سرویس هوش مصنوعی اضافه کن (تنظیمات > ایوگرام > سایر)."));
			return 0;
		}
		t.id = AyuAi::SaveTask(t);
		return t.id;
	};
	box->addButton(tr::lng_settings_save(), [=] {
		if (save()) {
			box->closeBox();
		}
	});
	box->addButton(P(Tr("Run now", "همین الان اجرا کن")), [=] {
		if (const auto id = save()) {
			show->showToast(Tr("Running…", "در حال اجرا…"));
			AyuAi::RunTaskNow(id, [=](QString report) {
				show->showToast(report);
			});
			box->closeBox();
		}
	});
	if (task.id) {
		const auto id = task.id;
		box->addLeftButton(P(Tr("Delete", "حذف")), [=] {
			AyuAi::RemoveTask(id);
			box->closeBox();
		});
	}
}

[[nodiscard]] QString TaskRowText(const Task &task, bool withChat) {
	auto result = QString::fromUtf8(task.enabled ? "🟢 " : "⚪ ");
	if (withChat) {
		if (const auto session = AyuTools::FindSession(task.account)) {
			result += AyuTools::PeerName(session, task.peer) + u": "_q;
		}
	}
	return result + AyuAi::TaskSummary(task);
}

void ChatTasksBox(
		not_null<Ui::GenericBox*> box,
		std::shared_ptr<Ui::Show> show,
		not_null<PeerData*> peer,
		MsgId topicRootId) {
	box->setTitle(P(Tr("AI tasks", "وظیفه‌های هوش مصنوعی")));
	box->setWidth(st::boxWideWidth);
	const auto list = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		style::margins());
	const auto rebuild = [=] {
		list->clear();
		AddLabel(list, Tr("Chat: ", "چت: ") + peer->name());
		const auto tasks = AyuAi::TasksFor(
			peer->session().uniqueId(),
			peer->id);
		if (tasks.empty()) {
			AddLabel(
				list,
				Tr(
					"No tasks yet. Examples: answer questions in a funny tone, "
					"reply to the message everyone replies to, add the reaction "
					"others use, send a summary every night.",
					"هنوز وظیفه‌ای نداری. مثلا: با لحن شوخ به سوال‌ها جواب بده، "
					"روی پیامی که همه ریپلای می‌کنن ریپلای بزن، همون ری‌اکشنی "
					"که بقیه زدن رو بزن، هر شب خلاصه پیام‌ها رو بفرست."));
		}
		for (const auto &task : tasks) {
			AddRowButton(list, TaskRowText(task, false), [=] {
				show->showBox(Box(TaskEditBox, show, peer, task));
			});
		}
		Ui::AddSkip(list, st::ayuToolsSkip);
		list->resizeToWidth(st::boxWideWidth);
	};
	rebuild();
	AyuAi::Changes() | rpl::on_next(rebuild, box->lifetime());

	box->addButton(P(Tr("New task", "وظیفه جدید")), [=] {
		auto task = Task();
		task.account = peer->session().uniqueId();
		task.peer = peer->id;
		task.topicRootId = topicRootId;
		show->showBox(Box(TaskEditBox, show, peer, task));
	});
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

void AllTasksBox(
		not_null<Ui::GenericBox*> box,
		std::shared_ptr<Ui::Show> show) {
	box->setTitle(P(Tr("All AI tasks", "همه وظیفه‌های هوش مصنوعی")));
	box->setWidth(st::boxWideWidth);
	const auto list = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		style::margins());
	const auto rebuild = [=] {
		list->clear();
		const auto tasks = AyuAi::Tasks();
		if (tasks.empty()) {
			AddLabel(
				list,
				Tr(
					"No tasks yet. Open a chat, press ⋮ > My tools > AI tasks.",
					"هنوز وظیفه‌ای نداری. یه چت رو باز کن، از منوی ⋮ > ابزارهای من > وظیفه‌های هوش مصنوعی."));
		}
		for (const auto &task : tasks) {
			AddRowButton(list, TaskRowText(task, true), [=] {
				const auto session = AyuTools::FindSession(task.account);
				if (!session) {
					show->showToast(Tr(
						"This account is not logged in.",
						"این اکانت وارد نیست."));
					return;
				}
				const auto peer = session->data().peer(task.peer);
				show->showBox(Box(TaskEditBox, show, peer, task));
			});
		}
		Ui::AddSkip(list, st::ayuToolsSkip);
		list->resizeToWidth(st::boxWideWidth);
	};
	rebuild();
	AyuAi::Changes() | rpl::on_next(rebuild, box->lifetime());
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

//
// Repeating messages.
//

[[nodiscard]] std::optional<AyuRepeat::Job> FindRepeatJob(uint64 id) {
	for (const auto &job : AyuRepeat::Jobs()) {
		if (job.id == id) {
			return job;
		}
	}
	return std::nullopt;
}

void RepeatEditBox(
		not_null<Ui::GenericBox*> box,
		std::shared_ptr<Ui::Show> show,
		AyuRepeat::Job job) {
	box->setTitle(P(Tr("Edit repeating message", "ویرایش پیام تکراری")));
	box->setWidth(st::boxWideWidth);
	const auto content = box->verticalLayout();
	const auto field = content->add(
		object_ptr<Ui::InputField>(
			content,
			st::ayuToolsBigField,
			Ui::InputField::Mode::MultiLine,
			P(Tr("Message text", "متن پیام")),
			job.text),
		st::boxRowPadding + QMargins(0, st::ayuToolsSkip, 0, 0));
	const auto period = std::make_shared<TimeId>(job.period);
	const auto periodText = std::make_shared<rpl::variable<QString>>(
		Tr("Repeat: ", "تکرار: ") + AyuRepeat::PeriodText(job.period));
	const auto choose = AddRowButton(content, periodText, nullptr);
	choose->setClickedCallback([=] {
		const auto menu = MakeMenu(choose);
		const auto minute = TimeId(60);
		const auto hour = 60 * minute;
		const auto day = 24 * hour;
		for (const auto value : {
				5 * minute, 10 * minute, 15 * minute, 30 * minute,
				hour, 2 * hour, 3 * hour, 4 * hour, 6 * hour, 8 * hour,
				12 * hour, day, 7 * day, 14 * day, 30 * day }) {
			menu->addAction(AyuRepeat::PeriodText(value), [=] {
				*period = value;
				*periodText = Tr("Repeat: ", "تکرار: ")
					+ AyuRepeat::PeriodText(value);
			});
		}
		menu->popup(QCursor::pos());
	});
	AddLabel(
		content,
		Tr(
			"The already scheduled copies are replaced with the new ones.",
			"نسخه‌هایی که از قبل زمان‌بندی شدن با نسخه جدید عوض میشن."));
	Ui::AddSkip(content, st::ayuToolsSkip);
	box->setFocusCallback([=] { field->setFocusFast(); });
	const auto id = job.id;
	box->addButton(tr::lng_settings_save(), [=] {
		auto text = field->getTextWithTags();
		if (text.text.trimmed().isEmpty()) {
			field->showError();
			return;
		}
		AyuRepeat::Update(id, std::move(text), *period);
		show->showToast(Tr(
			"Saved, the scheduled copies are being replaced…",
			"ذخیره شد، نسخه‌های زمان‌بندی‌شده دارن عوض میشن…"));
		box->closeBox();
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

void RepeatingBox(
		not_null<Ui::GenericBox*> box,
		std::shared_ptr<Ui::Show> show) {
	box->setTitle(P(Tr("Repeating messages", "پیام‌های تکراری")));
	box->setWidth(st::boxWideWidth);
	const auto list = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		style::margins());
	const auto rebuild = [=] {
		list->clear();
		AddLabel(
			list,
			Tr(
				"To create one: write a message, right-click the send button > "
				"Schedule message, pick the time and choose \"Repeat\". "
				"Copies are scheduled on Telegram servers (up to 100 per chat, "
				"up to a year ahead), so they are sent even when your computer is off.",
				"ساختنش: پیام رو بنویس، روی دکمه ارسال راست‌کلیک > ارسال زمان‌بندی‌شده، "
				"ساعت رو انتخاب کن و «تکرار» رو بزن. نسخه‌ها روی سرور تلگرام "
				"زمان‌بندی میشن (تا ۱۰۰ تا در هر چت و تا یک سال جلوتر)، پس حتی با "
				"کامپیوتر خاموش هم ارسال میشن."));
		const auto jobs = AyuRepeat::Jobs();
		if (jobs.empty()) {
			AddLabel(list, Tr("No repeating messages yet.", "هنوز پیام تکراری نداری."));
		}
		for (const auto &job : jobs) {
			const auto session = AyuTools::FindSession(job.account);
			const auto chat = session
				? AyuTools::PeerName(session, job.peer)
				: QString("?");
			const auto text = QString::fromUtf8(job.paused ? "⏸ " : "🔁 ")
				+ chat
				+ u" · "_q
				+ AyuRepeat::PeriodText(job.period)
				+ u" · "_q
				+ AyuTools::FormatTime(job.first)
				+ u" · "_q
				+ ShortLine(job.text.text, 40);
			const auto id = job.id;
			const auto jobText = job.text.text;
			const auto button = AddRowButton(list, text, nullptr);
			const auto paused = job.paused;
			button->setClickedCallback([=] {
				const auto menu = MakeMenu(button);
				menu->addAction(
					Tr("Edit text or period", "ویرایش متن یا فاصله تکرار"),
					[=] {
						if (const auto found = FindRepeatJob(id)) {
							show->showBox(Box(RepeatEditBox, show, *found));
						}
					},
					&st::menuIconEdit);
				menu->addAction(
					paused
						? Tr("Turn on again", "دوباره روشن کن")
						: Tr("Turn off for now", "فعلا خاموش کن"),
					[=] { AyuRepeat::SetPaused(id, !paused); },
					paused ? &st::menuIconRestore : &st::menuIconMute);
				menu->addAction(
					Tr(
						"Delete completely (with scheduled copies)",
						"کامل حذف کن (با نسخه‌های زمان‌بندی‌شده)"),
					[=] { AyuRepeat::Stop(id, true); },
					&st::menuIconDelete);
				menu->addAction(
					Tr(
						"Stop, keep already scheduled copies",
						"متوقف کن ولی نسخه‌های فعلی بمونن"),
					[=] { AyuRepeat::Stop(id, false); },
					&st::menuIconCancel);
				menu->addAction(
					Tr("Copy the text", "کپی متن"),
					[=] { Copy(show, jobText); },
					&st::menuIconCopy);
				menu->popup(QCursor::pos());
			});
		}
		Ui::AddSkip(list, st::ayuToolsSkip);
		list->resizeToWidth(st::boxWideWidth);
	};
	rebuild();
	AyuRepeat::Changes() | rpl::on_next(rebuild, box->lifetime());

	box->addButton(P(Tr("Refill now", "همین الان پر کن")), [=] {
		for (const auto &session : AyuTools::Sessions()) {
			AyuRepeat::TopUp(session);
		}
		show->showToast(Tr(
			"Checking and scheduling more copies…",
			"در حال بررسی و زمان‌بندی نسخه‌های بیشتر…"));
	});
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

//
// Ask AI.
//

//
// AI conversations.
//

[[nodiscard]] QString AccountName(uint64 id) {
	for (const auto &account : AyuMultiReply::Accounts()) {
		if (account.id == id) {
			return account.name;
		}
	}
	return Tr("(not logged in)", "(وارد نیست)");
}

void ConversationEditBox(
		not_null<Ui::GenericBox*> box,
		std::shared_ptr<Ui::Show> show,
		AyuConversation::Conversation conversation,
		QString chatName) {
	using AyuConversation::Participant;
	const auto state = std::make_shared<AyuConversation::Conversation>(
		conversation);
	box->setTitle(P(conversation.id
		? Tr("Edit AI conversation", "ویرایش گفتگوی هوش مصنوعی")
		: Tr("New AI conversation", "گفتگوی هوش مصنوعی جدید")));
	box->setWidth(st::boxWideWidth);
	const auto content = box->verticalLayout();

	AddLabel(content, Tr("Chat: ", "چت: ") + chatName);
	const auto title = AddField(
		content,
		Tr("Title (optional)", "عنوان (اختیاری)"),
		state->title);
	const auto topic = AddField(
		content,
		Tr(
			"Topic and rules, e.g.: discuss which phone is better, keep it friendly",
			"موضوع و قوانین، مثلا: بحث کنن کدوم گوشی بهتره، دوستانه بمونه"),
		state->topic,
		true,
		st::ayuToolsBigField);

	AddTitle(content, Tr("Participants", "شرکت‌کننده‌ها"));
	AddLabel(
		content,
		Tr(
			"Each participant is one of your accounts with its own AI service "
			"and its own role or side. The accounts must be members of the chat.",
			"هر شرکت‌کننده یکی از اکانت‌هاته با سرویس هوش مصنوعی و نقش یا "
			"طرف بحث خودش. اکانت‌ها باید عضو چت باشن."));
	const auto list = content->add(
		object_ptr<Ui::VerticalLayout>(content),
		style::margins());
	const auto personas = std::make_shared<std::vector<Ui::InputField*>>();
	const auto collect = [=] {
		for (auto i = 0; i != int(personas->size()); ++i) {
			if (i < int(state->participants.size())) {
				state->participants[i].persona
					= (*personas)[i]->getLastText().trimmed();
			}
		}
	};
	const auto rebuild = std::make_shared<Fn<void()>>();
	*rebuild = [=] {
		list->clear();
		personas->clear();
		for (auto i = 0; i != int(state->participants.size()); ++i) {
			const auto &p = state->participants[i];
			AddTitle(list, Tr("Participant ", "شرکت‌کننده ")
				+ QString::number(i + 1));
			const auto account = AddRowButton(
				list,
				Tr("Account: ", "اکانت: ") + AccountName(p.account),
				nullptr);
			account->setClickedCallback([=] {
				const auto menu = MakeMenu(account);
				for (const auto &a : AyuMultiReply::Accounts()) {
					const auto id = a.id;
					menu->addAction(
						a.name + (a.group.isEmpty()
							? QString()
							: (u" · "_q + a.group)),
						[=] {
							collect();
							state->participants[i].account = id;
							crl::on_main(box, [=] { (*rebuild)(); });
						});
				}
				menu->popup(QCursor::pos());
			});
			const auto provider = std::make_shared<uint64>(p.providerId);
			const auto service = AddRowButton(
				list,
				Tr("AI service: ", "سرویس هوش مصنوعی: ")
					+ ProviderName(p.providerId),
				nullptr);
			service->setClickedCallback([=] {
				const auto menu = MakeMenu(service);
				const auto choose = [=](uint64 id) {
					collect();
					state->participants[i].providerId = id;
					crl::on_main(box, [=] { (*rebuild)(); });
				};
				menu->addAction(Tr("Default", "پیش‌فرض"), [=] { choose(0); });
				for (const auto &item : AyuAi::Providers()) {
					const auto id = item.id;
					menu->addAction(ProviderTitle(item), [=] { choose(id); });
				}
				menu->popup(QCursor::pos());
			});
			personas->push_back(AddField(
				list,
				Tr(
					"Role, opinion, side and tone, e.g.: loves iPhone, sarcastic",
					"نقش، نظر، طرف بحث و لحن، مثلا: طرفدار آیفون، کنایه‌ای"),
				p.persona,
				true));
			AddRowButton(list, Tr("Remove this participant", "حذف این شرکت‌کننده"), [=] {
				collect();
				if (i < int(state->participants.size())) {
					state->participants.erase(state->participants.begin() + i);
				}
				crl::on_main(box, [=] { (*rebuild)(); });
			});
		}
		AddRowButton(list, QString::fromUtf8("➕ ") + Tr("Add participant", "افزودن شرکت‌کننده"), [=] {
			collect();
			const auto accounts = AyuMultiReply::Accounts();
			auto next = Participant();
			for (const auto &a : accounts) {
				if (!ranges::contains(
						state->participants,
						a.id,
						&Participant::account)) {
					next.account = a.id;
					break;
				}
			}
			if (!next.account && !accounts.empty()) {
				next.account = accounts.front().id;
			}
			state->participants.push_back(next);
			crl::on_main(box, [=] { (*rebuild)(); });
		});
		list->resizeToWidth(st::boxWideWidth);
		content->resizeToWidth(st::boxWideWidth);
	};
	(*rebuild)();

	AddTitle(content, Tr("Timing", "زمان‌بندی"));
	const auto gap = AddField(
		content,
		Tr(
			"Minutes between messages (e.g. 1-3 or 15)",
			"فاصله بین پیام‌ها به دقیقه (مثلا 1-3 یا 15)"),
		(state->minGap == state->maxGap)
			? QString::number(state->minGap / 60.)
			: (QString::number(state->minGap / 60.)
				+ '-'
				+ QString::number(state->maxGap / 60.)));
	const auto limit = AddField(
		content,
		Tr(
			"Stop after this many messages (0 = never)",
			"بعد از چند پیام تموم بشه (۰ = هیچ‌وقت)"),
		QString::number(state->maxMessages));
	const auto showService = AddCheck(
		content,
		Tr(
			"Write the AI service name before each message (to compare them)",
			"اسم سرویس هوش مصنوعی اول هر پیام نوشته بشه (برای مقایسه)"),
		state->showService);
	const auto enabled = AddCheck(
		content,
		Tr("Enabled", "فعال باشه"),
		state->enabled);
	if (conversation.id) {
		const auto status = AyuConversation::Status(conversation.id);
		if (!status.isEmpty()) {
			AddLabel(content, Tr("Status: ", "وضعیت: ") + status);
		}
	}
	AddLabel(
		content,
		Tr(
			"Works while AyuGram is open on this computer.",
			"فقط وقتی ایوگرام روی این کامپیوتر بازه کار می‌کنه."));
	Ui::AddSkip(content, st::ayuToolsSkip);

	const auto save = [=]() -> uint64 {
		collect();
		auto &c = *state;
		c.title = title->getLastText().trimmed();
		c.topic = topic->getLastText().trimmed();
		const auto parts = gap->getLastText().split('-', Qt::SkipEmptyParts);
		auto ok1 = false, ok2 = false;
		const auto from = parts.value(0).trimmed().toDouble(&ok1);
		const auto till = parts.value(1, parts.value(0)).trimmed().toDouble(&ok2);
		if (ok1 && ok2 && from > 0) {
			c.minGap = int(from * 60);
			c.maxGap = std::max(c.minGap, int(till * 60));
		}
		c.maxMessages = FieldInt(limit, c.maxMessages, 0, 100000);
		c.showService = showService->checked();
		c.enabled = enabled->checked();
		if (c.participants.empty()) {
			show->showToast(Tr(
				"Add at least one participant.",
				"حداقل یه شرکت‌کننده اضافه کن."));
			return 0;
		} else if (AyuAi::Providers().empty()) {
			show->showToast(Tr(
				"Add an AI service first (Settings > AyuGram > Other).",
				"اول یه سرویس هوش مصنوعی اضافه کن (تنظیمات > ایوگرام > سایر)."));
			return 0;
		}
		c.id = AyuConversation::Save(c);
		return c.id;
	};
	box->addButton(tr::lng_settings_save(), [=] {
		if (save()) {
			box->closeBox();
		}
	});
	box->addButton(P(Tr("Start now", "همین الان شروع کن")), [=] {
		if (const auto id = save()) {
			AyuConversation::Restart(id);
			show->showToast(Tr("The conversation started.", "گفتگو شروع شد."));
			box->closeBox();
		}
	});
	if (conversation.id) {
		const auto id = conversation.id;
		box->addLeftButton(P(Tr("Delete", "حذف")), [=] {
			AyuConversation::Remove(id);
			box->closeBox();
		});
	}
}

void ConversationsBox(
		not_null<Ui::GenericBox*> box,
		std::shared_ptr<Ui::Show> show,
		PeerData *peer) {
	box->setTitle(P(Tr("AI conversations", "گفتگوهای هوش مصنوعی")));
	box->setWidth(st::boxWideWidth);
	const auto list = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		style::margins());
	const auto chatName = [=](PeerId id) {
		if (peer && peer->id == id) {
			return peer->name();
		}
		for (const auto &session : AyuTools::Sessions()) {
			if (const auto loaded = session->data().peerLoaded(id)) {
				return loaded->name();
			}
		}
		return QString("?");
	};
	const auto rebuild = [=] {
		list->clear();
		AddLabel(
			list,
			Tr(
				"Your accounts talk with each other in a chat, each with its "
				"own AI service and role. Good for comparing AI services.",
				"اکانت‌هات توی یه چت با هم حرف می‌زنن، هر کدوم با سرویس هوش "
				"مصنوعی و نقش خودش. برای مقایسه هوش‌های مصنوعی خوبه."));
		auto any = false;
		for (const auto &c : AyuConversation::List()) {
			if (peer && c.peer != peer->id) {
				continue;
			}
			any = true;
			const auto status = AyuConversation::Status(c.id);
			AddRowButton(
				list,
				QString::fromUtf8(c.enabled ? "🟢 " : "⚪ ")
					+ (peer ? QString() : (chatName(c.peer) + u": "_q))
					+ AyuConversation::Summary(c),
				[=] {
					show->showBox(Box(
						ConversationEditBox,
						show,
						c,
						chatName(c.peer)));
				});
			if (!status.isEmpty()) {
				AddLabel(list, status);
			}
		}
		if (!any) {
			AddLabel(list, Tr("No conversations yet.", "هنوز گفتگویی نداری."));
		}
		Ui::AddSkip(list, st::ayuToolsSkip);
		list->resizeToWidth(st::boxWideWidth);
	};
	rebuild();
	AyuConversation::Changes() | rpl::on_next(rebuild, box->lifetime());
	if (peer) {
		box->addButton(P(Tr("New conversation", "گفتگوی جدید")), [=] {
			auto c = AyuConversation::Conversation();
			c.peer = peer->id;
			show->showBox(Box(ConversationEditBox, show, c, peer->name()));
		});
	}
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

void MultiReplyBox(
	not_null<Ui::GenericBox*> box,
	not_null<Window::SessionController*> controller,
	QString link,
	QString presetInstructions,
	std::vector<uint64> presetAccounts,
	bool autoGenerate);

struct AskState {
	std::vector<AyuAi::ChatMessage> history;
	QString replyFrom;
	QString replyText;
	QByteArray image;
	bool imageLoading = false;
	std::shared_ptr<Data::PhotoMedia> photo;
	QString answer;
	bool busy = false;
	Fn<void()> cancel;
	std::shared_ptr<uint64> providerId = std::make_shared<uint64>(0);
	rpl::lifetime downloadLifetime;
};

void AskBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		FullMsgId replyTo,
		QString question,
		Fn<void(QString)> insert,
		QString extraContext) {
	const auto show = controller->uiShow();
	const auto session = &peer->session();
	const auto state = std::make_shared<AskState>();
	box->lifetime().add([=] {
		if (state->cancel) {
			state->cancel();
		}
	});

	box->setTitle(P(Tr("Ask AI", "از هوش مصنوعی بپرس")));
	box->setWidth(st::boxWideWidth);
	const auto content = box->verticalLayout();

	if (AyuAi::Providers().empty()) {
		AddLabel(
			content,
			Tr(
				"Add an AI service first (for example DeepSeek) and paste its API key.",
				"اول یه سرویس هوش مصنوعی (مثلا دیپ‌سیک) اضافه کن و کلیدش رو وارد کن."),
			st::ayuToolsAnswer);
		box->addButton(P(Tr("Add AI service", "افزودن سرویس")), [=] {
			box->closeBox();
			show->showBox(Box(ProvidersBox, show));
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		return;
	}

	const auto item = replyTo ? session->data().message(replyTo) : nullptr;
	if (item) {
		state->replyFrom = item->from()->name();
		state->replyText = item->originalText().text;
		if (const auto media = item->media()) {
			if (const auto photo = media->photo()) {
				state->photo = photo->createMediaView();
				state->photo->wanted(Data::PhotoSize::Large, item->fullId());
				const auto take = [=] {
					const auto image = state->photo->image(
						Data::PhotoSize::Large);
					if (!image) {
						return false;
					}
					state->image = ToJpeg(image->original());
					state->imageLoading = false;
					return true;
				};
				if (!take()) {
					state->imageLoading = true;
					session->downloaderTaskFinished(
					) | rpl::filter([=] {
						return take();
					}) | rpl::take(1) | rpl::on_next([] {
					}, state->downloadLifetime);
				}
			} else if (state->replyText.isEmpty()) {
				state->replyText = u"["_q
					+ Tr("media without text", "رسانه بدون متن")
					+ u"]"_q;
			}
		}
		auto about = Tr("About the message of ", "درباره پیام ")
			+ state->replyFrom
			+ u": "_q
			+ ShortLine(state->replyText, 120);
		if (state->photo) {
			about += QString::fromUtf8(" 🖼");
		}
		AddLabel(content, about);
	} else if (!extraContext.isEmpty()) {
		AddLabel(
			content,
			Tr(
				"The statistics report will be sent with your question.",
				"گزارش آمار همراه سوالت فرستاده میشه."));
	}

	const auto field = AddField(
		content,
		item
			? Tr("Your question about this message", "سوالت درباره این پیام")
			: Tr("Your question", "سوالت"),
		question,
		true,
		st::ayuToolsBigField);
	const auto withChat = AddCheck(
		content,
		Tr(
			"Also read the last 30 messages of this chat",
			"۳۰ پیام آخر این چت رو هم بخونه"),
		!item && extraContext.isEmpty());
	AddProviderChooser(content, state->providerId, true);

	const auto answer = AddLabel(content, QString(), st::ayuToolsAnswer);
	answer->setBreakEverywhere(false);
	const auto actionsWrap = content->add(
		object_ptr<Ui::VerticalLayout>(content),
		style::margins());
	const auto repliedLink = item
		? AyuMultiReply::MakeLink(item)
		: QString();
	Ui::AddSkip(content, st::ayuToolsSkip);

	const auto setAnswer = [=](const QString &text) {
		answer->setText(text);
		content->resizeToWidth(st::boxWideWidth);
	};

	const auto request = [=](QString firstText) {
		auto system = AyuAi::BaseSystemPrompt();
		system += u"\n\nYou are helping the account owner inside the Telegram chat \""_q
			+ peer->name()
			+ u"\". Answer in the language of the question unless asked otherwise. "
			"If asked to write a message, give only the message text."_q
			+ AyuMultiReply::AgentPrompt(
				session->uniqueId(),
				repliedLink,
				peer->name());
		const auto provider = AyuAi::FindProvider(*state->providerId);
		auto message = AyuAi::ChatMessage{
			.role = u"user"_q,
			.text = firstText,
		};
		if (state->history.empty() && !state->image.isEmpty()) {
			if (provider && provider->vision) {
				message.imageJpeg = state->image;
			} else {
				message.text += u"\n\n(There was an image, but this AI service cannot see images.)"_q;
			}
		}
		state->history.push_back(message);
		state->busy = true;
		setAnswer(Tr("Thinking…", "در حال فکر کردن…"));
		state->cancel = AyuAi::Complete(
			AyuAi::Request{
				.providerId = *state->providerId,
				.system = system,
				.messages = state->history,
			},
			crl::guard(box, [=](QString text) {
				state->busy = false;
				state->answer = text;
				state->history.push_back({
					.role = u"assistant"_q,
					.text = text,
				});
				actionsWrap->clear();
				auto error = QString();
				const auto actions = AyuMultiReply::ParseActions(
					text,
					session->uniqueId(),
					repliedLink,
					&error);
				if (!actions.empty()) {
					state->answer = QString();
					auto lines = QStringList();
					for (const auto &action : actions) {
						lines.push_back(QString::fromUtf8("• ")
							+ AyuMultiReply::ActionText(action));
					}
					setAnswer(Tr("I will do this:\n", "این کارها رو انجام میدم:\n")
						+ lines.join(u"\n\n"_q));
					AddRowButton(
						actionsWrap,
						QString::fromUtf8("✅ ") + Tr("Do it", "انجامش بده"),
						[=] {
							box->closeBox();
							for (const auto &action : actions) {
								if (action.type
									== AyuMultiReply::Action::Type::Conversation) {
									auto c = AyuConversation::Conversation();
									c.peer = peer->id;
									c.title = Tr("By AI request", "با دستور هوش مصنوعی");
									c.topic = action.topic;
									c.participants = action.participants;
									c.minGap = action.minGap;
									c.maxGap = action.maxGap;
									c.maxMessages = action.maxMessages;
									auto providers = base::flat_set<uint64>();
									for (const auto &p : c.participants) {
										providers.emplace(p.providerId);
									}
									c.showService = (providers.size() > 1);
									AyuConversation::Restart(
										AyuConversation::Save(c));
									show->showToast(Tr(
										"The conversation started. See ⋮ > My tools > AI conversations.",
										"گفتگو شروع شد. از ⋮ > ابزارهای من > گفتگوهای هوش مصنوعی می‌بینیش."));
								} else if (action.type
									== AyuMultiReply::Action::Type::React) {
									ShowMultiReact(
										controller,
										action.link,
										action.emoji,
										action.accounts,
										true);
								} else {
									controller->show(Box(
										MultiReplyBox,
										controller,
										action.link,
										action.instructions,
										action.accounts,
										true));
								}
							}
						});
					actionsWrap->resizeToWidth(st::boxWideWidth);
				} else if (!error.isEmpty()) {
					setAnswer(QString::fromUtf8("❌ ") + error);
				} else {
					setAnswer(text);
				}
				field->setText(QString());
				field->setPlaceholder(P(Tr(
					"Ask a follow-up question",
					"سوال بعدی (ادامه گفتگو)")));
				field->setFocusFast();
			}),
			crl::guard(box, [=](QString error) {
				state->busy = false;
				state->history.pop_back();
				setAnswer(QString::fromUtf8("❌ ") + error);
			}));
	};

	const auto ask = [=] {
		if (state->busy) {
			return;
		}
		auto text = field->getLastText().trimmed();
		if (text.isEmpty()) {
			if (!item && extraContext.isEmpty() && !state->history.empty()) {
				field->showError();
				return;
			} else if (state->history.empty() && (item || !extraContext.isEmpty())) {
				text = item
					? Tr("Explain this message.", "این پیام رو توضیح بده.")
					: Tr("Analyze these statistics.", "این آمار رو تحلیل کن.");
			} else {
				field->showError();
				return;
			}
		}
		if (!state->history.empty()) {
			request(text);
			return;
		}
		if (state->imageLoading) {
			setAnswer(Tr(
				"The image is still loading, try again in a moment.",
				"عکس هنوز در حال دانلوده، چند لحظه دیگه دوباره بزن."));
			return;
		}
		auto first = QString();
		if (!extraContext.isEmpty()) {
			first += u"Data:\n"_q + extraContext + u"\n\n"_q;
		}
		if (item) {
			first += u"The message I am asking about (from "_q
				+ state->replyFrom
				+ u"):\n"_q
				+ state->replyText
				+ (state->image.isEmpty()
					? QString()
					: u"\n[The image of this message is attached]"_q)
				+ u"\n\n"_q;
		}
		const auto finish = [=](QString context) {
			auto full = first;
			if (!context.isEmpty()) {
				full = u"Recent messages of the chat:\n"_q
					+ context
					+ u"\n\n"_q
					+ full;
			}
			request(full + u"Question: "_q + text);
		};
		if (withChat->checked()) {
			state->busy = true;
			setAnswer(Tr("Reading the chat…", "در حال خوندن چت…"));
			AyuTools::LoadRecent(
				session,
				peer,
				30,
				crl::guard(box, [=](std::vector<AyuTools::RawMessage> list) {
					state->busy = false;
					finish(AyuTools::FormatForPrompt(session, list, 8000));
				}),
				crl::guard(box, [=](QString error) {
					state->busy = false;
					finish(QString());
				}));
		} else {
			finish(QString());
		}
	};
	field->submits() | rpl::on_next(ask, field->lifetime());

	box->setFocusCallback([=] { field->setFocusFast(); });
	box->addButton(P(Tr("Ask", "بپرس")), ask);
	if (insert) {
		box->addButton(P(Tr("Put in message", "بذار تو پیام")), [=] {
			if (state->answer.isEmpty()) {
				return;
			}
			const auto text = state->answer;
			box->closeBox();
			insert(text);
		});
	}
	box->addButton(P(Tr("Copy", "کپی")), [=] {
		if (!state->answer.isEmpty()) {
			Copy(show, state->answer);
		}
	});
	box->addLeftButton(tr::lng_close(), [=] { box->closeBox(); });
}

//
// Statistics.
//

enum class Period {
	Today,
	Yesterday,
	Week,
	Month,
	Custom,
};

struct StatsState {
	Period period = Period::Today;
	TimeId customFrom = 0;
	TimeId customTill = 0;
	int limit = 5000;
	PeerId filter = 0;

	std::vector<AyuTools::RawMessage> messages;
	TimeId loadedFrom = 0;
	TimeId loadedTill = 0;
	int loadedLimit = 0;
	bool hasResult = false;
	AyuStats::Stats stats;

	Fn<void()> cancel;
};

[[nodiscard]] std::shared_ptr<StatsState> StatsStateFor(
		not_null<PeerData*> peer) {
	static auto map = base::flat_map<
		std::pair<uint64, PeerId>,
		std::shared_ptr<StatsState>>();
	const auto key = std::make_pair(peer->session().uniqueId(), peer->id);
	auto &result = map[key];
	if (!result) {
		result = std::make_shared<StatsState>();
		const auto now = base::unixtime::now();
		result->customFrom = now - 7 * kDay;
		result->customTill = now;
	}
	return result;
}

[[nodiscard]] std::pair<TimeId, TimeId> PeriodRange(
		const StatsState &state) {
	const auto now = base::unixtime::now();
	const auto todayStart = TimeId(
		QDateTime(QDate::currentDate(), QTime(0, 0)).toSecsSinceEpoch());
	switch (state.period) {
	case Period::Today: return { todayStart, now };
	case Period::Yesterday: return { todayStart - kDay, todayStart - 1 };
	case Period::Week: return { now - 7 * kDay, now };
	case Period::Month: return { now - 30 * kDay, now };
	case Period::Custom: return { state.customFrom, state.customTill };
	}
	return { todayStart, now };
}

[[nodiscard]] QString Bar(int value, int max, int width = 16) {
	const auto count = max ? ((value * width + max - 1) / max) : 0;
	return QString(count, QChar(0x2588));
}

[[nodiscard]] QString WeekdayName(int index) {
	static const char *en[] = {
		"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
	static const char *fa[] = {
		"دوشنبه", "سه‌شنبه", "چهارشنبه", "پنجشنبه", "جمعه", "شنبه", "یکشنبه" };
	return Tr(en[index], fa[index]);
}

[[nodiscard]] QString WordsLine(
		const std::vector<AyuStats::WordScore> &words,
		int limit = 15) {
	auto result = QStringList();
	for (const auto &word : words) {
		result.push_back(word.word + u" ("_q + QString::number(word.value) + ')');
		if (result.size() >= limit) {
			break;
		}
	}
	return result.join(u", "_q);
}

void StatsBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	const auto show = controller->uiShow();
	const auto session = &peer->session();
	const auto state = StatsStateFor(peer);
	box->lifetime().add([=] {
		if (state->cancel) {
			state->cancel();
			state->cancel = nullptr;
		}
	});

	box->setTitle(P(Tr("Chat statistics", "آمار چت")));
	box->setWidth(st::boxWideWidth);
	const auto content = box->verticalLayout();
	AddLabel(content, peer->name());

	AddTitle(content, Tr("Period", "بازه زمانی"));
	const auto group = std::make_shared<Ui::RadioenumGroup<Period>>(
		state->period);
	const auto addRadio = [&](Period value, const QString &text) {
		content->add(
			object_ptr<Ui::Radioenum<Period>>(
				content,
				group,
				value,
				text,
				st::defaultBoxCheckbox),
			st::boxRowPadding + QMargins(0, st::ayuToolsSkip, 0, 0));
	};
	addRadio(Period::Today, Tr("Today", "امروز"));
	addRadio(Period::Yesterday, Tr("Yesterday", "دیروز"));
	addRadio(Period::Week, Tr("Last 7 days", "۷ روز اخیر"));
	addRadio(Period::Month, Tr("Last 30 days", "۳۰ روز اخیر"));
	addRadio(Period::Custom, Tr("Custom range", "بازه دلخواه"));

	const auto dateText = [](const QString &prefix, TimeId value) {
		return prefix
			+ AyuTools::FormatDate(value)
			+ ' '
			+ AyuTools::FormatTime(value);
	};
	const auto fromText = std::make_shared<rpl::variable<QString>>(
		dateText(Tr("From: ", "از: "), state->customFrom));
	const auto tillText = std::make_shared<rpl::variable<QString>>(
		dateText(Tr("To: ", "تا: "), state->customTill));
	const auto fromButton = AddRowButton(content, fromText, nullptr);
	const auto tillButton = AddRowButton(content, tillText, nullptr);
	const auto chooseDate = [=](bool from) {
		show->showBox(Box([=](not_null<Ui::GenericBox*> b) {
			Ui::ChooseDateTimeBox(b, {
				.title = P(from ? Tr("From", "از") : Tr("To", "تا")),
				.submit = tr::lng_settings_save(),
				.done = [=](TimeId value) {
					if (from) {
						state->customFrom = value;
						*fromText = dateText(Tr("From: ", "از: "), value);
					} else {
						state->customTill = value;
						*tillText = dateText(Tr("To: ", "تا: "), value);
					}
					group->setValue(Period::Custom);
					state->period = Period::Custom;
					b->closeBox();
				},
				.min = [] {
					return base::unixtime::now() - 15 * 365 * kDay;
				},
				.time = from ? state->customFrom : state->customTill,
				.max = [] {
					return base::unixtime::now() + kDay;
				},
			});
		}));
	};
	fromButton->setClickedCallback([=] { chooseDate(true); });
	tillButton->setClickedCallback([=] { chooseDate(false); });

	const auto limit = AddField(
		content,
		Tr(
			"Read at most this many messages",
			"حداکثر چند پیام خونده بشه"),
		QString::number(state->limit));

	const auto status = AddLabel(content, QString());
	const auto results = content->add(
		object_ptr<Ui::VerticalLayout>(content),
		style::margins());

	const auto name = [=](PeerId id) {
		return AyuTools::PeerName(session, id);
	};
	const auto openMessage = [=](MsgId id) {
		box->closeBox();
		controller->showPeerHistory(
			peer->id,
			Window::SectionShow::Way::ClearStack,
			id);
	};

	const auto render = std::make_shared<Fn<void()>>();
	const auto compute = [=] {
		const auto [from, till] = PeriodRange(*state);
		state->stats = AyuStats::Compute(
			state->messages,
			from,
			till,
			state->filter,
			10);
		state->stats.truncated = state->stats.truncated
			|| (int(state->messages.size()) >= state->loadedLimit
				&& !state->messages.empty()
				&& state->messages.front().date > from);
		state->hasResult = true;
		(*render)();
	};
	*render = [=] {
		results->clear();
		const auto &s = state->stats;
		if (!state->hasResult) {
			results->resizeToWidth(st::boxWideWidth);
			return;
		}
		if (s.filter) {
			AddTitle(results, Tr("Only: ", "فقط: ") + name(s.filter));
			AddRowButton(results, Tr("Show everyone", "نمایش همه"), [=] {
				state->filter = 0;
				compute();
			});
		}
		AddTitle(results, Tr("Summary", "خلاصه"));
		auto summary = QStringList();
		summary.push_back(Tr("Messages: ", "پیام‌ها: ")
			+ QString::number(s.total)
			+ Tr(" · Members who wrote: ", " · افرادی که پیام دادن: ")
			+ QString::number(s.senders));
		summary.push_back(Tr("Replies: ", "ریپلای‌ها: ")
			+ QString::number(s.totalReplies)
			+ Tr(" · Reactions: ", " · ری‌اکشن‌ها: ")
			+ QString::number(s.totalReactions)
			+ Tr(" (on ", " (روی ")
			+ QString::number(s.reactedMessages)
			+ Tr(" messages)", " پیام)"));
		auto media = QStringList();
		for (const auto &[kind, count] : s.media) {
			media.push_back(AyuTools::MediaLabel(kind)
				+ ' '
				+ QString::number(count));
		}
		if (!media.isEmpty()) {
			summary.push_back(Tr("Media: ", "رسانه: ") + media.join(u", "_q));
		}
		if (s.forwarded) {
			summary.push_back(Tr("Forwarded: ", "فوروارد شده: ")
				+ QString::number(s.forwarded));
		}
		if (s.longestStreakPeer && s.longestStreak > 1) {
			summary.push_back(Tr("Longest streak: ", "بیشترین پیام پشت سر هم: ")
				+ name(s.longestStreakPeer)
				+ u" ("_q
				+ QString::number(s.longestStreak)
				+ ')');
		}
		if (s.truncated) {
			summary.push_back(Tr(
				"⚠ Not all messages of the period were read, raise the limit for full stats.",
				"⚠ همه پیام‌های این بازه خونده نشد؛ برای آمار کامل عدد حداکثر رو بیشتر کن."));
		}
		AddLabel(results, summary.join('\n'), st::ayuToolsAnswer);

		const auto messageRows = [&](
				const QString &title,
				const std::vector<AyuStats::MessageScore> &list,
				const QString &unit,
				bool withReactions) {
			if (list.empty()) {
				return;
			}
			AddTitle(results, title);
			for (const auto &m : list) {
				auto text = QString::number(m.value)
					+ ' '
					+ unit
					+ u" · "_q
					+ name(m.from)
					+ u": "_q
					+ ShortLine(m.text, 50);
				if (withReactions && !m.reactions.empty()) {
					text += u"  "_q + AyuStats::ReactionsLine(m.reactions, 4);
				}
				const auto id = m.id;
				AddRowButton(results, text, [=] { openMessage(id); });
			}
		};
		const auto peerRows = [&](
				const QString &title,
				const std::vector<AyuStats::PeerScore> &list,
				const QString &unit) {
			if (list.empty()) {
				return;
			}
			AddTitle(results, title);
			for (const auto &p : list) {
				const auto id = p.peer;
				AddRowButton(
					results,
					name(id) + u" · "_q + QString::number(p.value) + ' ' + unit,
					[=] {
						state->filter = id;
						compute();
					});
			}
		};

		messageRows(
			Tr("Most replied messages", "پر ریپلای‌ترین پیام‌ها"),
			s.topReplied,
			Tr("replies", "ریپلای"),
			false);
		messageRows(
			Tr("Most reacted messages", "پر ری‌اکشن‌ترین پیام‌ها"),
			s.topReacted,
			Tr("reactions", "ری‌اکشن"),
			true);
		if (!s.reactionTotals.empty()) {
			AddTitle(results, Tr("Reactions used", "ری‌اکشن‌های استفاده‌شده"));
			AddLabel(
				results,
				AyuStats::ReactionsLine(s.reactionTotals, 20),
				st::ayuToolsAnswer);
		}
		AddLabel(
			results,
			Tr(
				"Tap a person to see stats only for them, tap a message to open it.",
				"روی هر شخص بزنی آمار فقط برای اون نشون داده میشه، روی پیام بزنی بازش می‌کنه."));
		peerRows(
			Tr("Most active", "فعال‌ترین‌ها"),
			s.topSenders,
			Tr("messages", "پیام"));
		peerRows(
			Tr("Got the most replies", "بیشترین ریپلای گرفته"),
			s.mostRepliedUsers,
			Tr("replies", "ریپلای"));
		peerRows(
			Tr("Replied the most", "بیشترین ریپلای زده"),
			s.mostReplyingUsers,
			Tr("replies", "ریپلای"));
		peerRows(
			Tr("Got the most reactions", "بیشترین ری‌اکشن گرفته"),
			s.mostReactedUsers,
			Tr("reactions", "ری‌اکشن"));
		messageRows(
			Tr("Most viewed", "پربازدیدترین"),
			s.topViewed,
			Tr("views", "بازدید"),
			false);
		messageRows(
			Tr("Most forwarded", "بیشترین فوروارد"),
			s.topForwarded,
			Tr("forwards", "فوروارد"),
			false);
		messageRows(
			Tr("Most comments", "بیشترین کامنت"),
			s.topComments,
			Tr("comments", "کامنت"),
			false);

		if (s.total > 0) {
			const auto maxHour = *ranges::max_element(s.hours);
			auto hours = QStringList();
			for (auto i = 0; i != 24; ++i) {
				if (s.hours[i]) {
					hours.push_back(u"%1  %2 %3"_q
						.arg(i, 2, 10, QChar('0'))
						.arg(Bar(s.hours[i], maxHour))
						.arg(s.hours[i]));
				}
			}
			AddTitle(results, Tr("Activity by hour", "فعالیت بر اساس ساعت"));
			AddLabel(results, hours.join('\n'), st::ayuToolsAnswer);

			const auto maxDay = *ranges::max_element(s.weekdays);
			auto weekdays = QStringList();
			for (auto i = 0; i != 7; ++i) {
				weekdays.push_back(WeekdayName(i)
					+ u"  "_q
					+ Bar(s.weekdays[i], maxDay)
					+ ' '
					+ QString::number(s.weekdays[i]));
			}
			AddTitle(results, Tr("By weekday", "بر اساس روز هفته"));
			AddLabel(results, weekdays.join('\n'), st::ayuToolsAnswer);

			if (s.days.size() > 1) {
				auto max = 0;
				for (const auto &[date, count] : s.days) {
					max = std::max(max, count);
				}
				auto days = QStringList();
				const auto from = std::max(0, int(s.days.size()) - 31);
				for (auto i = from; i != int(s.days.size()); ++i) {
					const auto &[date, count] = s.days[i];
					days.push_back(date.toString(u"MM-dd"_q)
						+ u"  "_q
						+ Bar(count, max)
						+ ' '
						+ QString::number(count));
				}
				AddTitle(results, Tr("By day", "بر اساس روز"));
				AddLabel(results, days.join('\n'), st::ayuToolsAnswer);
			}
		}
		const auto wordsSection = [&](
				const QString &title,
				const std::vector<AyuStats::WordScore> &list) {
			if (!list.empty()) {
				AddTitle(results, title);
				AddLabel(results, WordsLine(list), st::ayuToolsAnswer);
			}
		};
		wordsSection(Tr("Popular words", "کلمات پرتکرار"), s.words);
		wordsSection(Tr("Hashtags", "هشتگ‌ها"), s.hashtags);
		wordsSection(Tr("Mentions", "منشن‌ها"), s.mentions);
		wordsSection(Tr("Links", "لینک‌ها"), s.links);

		Ui::AddSkip(results, st::ayuToolsSkip * 2);
		results->resizeToWidth(st::boxWideWidth);
	};
	(*render)();

	group->setChangedCallback([=](Period value) {
		state->period = value;
	});

	const auto calculate = [=] {
		if (state->cancel) {
			return;
		}
		state->limit = FieldInt(limit, 5000, 100, 100000);
		const auto [from, till] = PeriodRange(*state);
		if (from >= till) {
			status->setText(Tr(
				"The start must be before the end.",
				"شروع بازه باید قبل از پایانش باشه."));
			return;
		}
		const auto cached = !state->messages.empty()
			&& state->loadedFrom <= from
			&& state->loadedTill >= till
			&& state->loadedLimit >= state->limit;
		if (cached) {
			status->setText(QString());
			compute();
			return;
		}
		status->setText(Tr("Reading messages…", "در حال خوندن پیام‌ها…"));
		state->cancel = AyuTools::LoadHistory(
			session,
			peer,
			from,
			till,
			state->limit,
			crl::guard(box, [=](AyuTools::LoadProgress progress) {
				status->setText(Tr("Reading messages… ", "در حال خوندن پیام‌ها… ")
					+ QString::number(progress.loaded)
					+ (progress.oldestDate
						? (u" ("_q + AyuTools::FormatDate(progress.oldestDate) + ')')
						: QString()));
			}),
			crl::guard(box, [=](std::vector<AyuTools::RawMessage> list) {
				state->cancel = nullptr;
				state->messages = std::move(list);
				state->loadedFrom = from;
				state->loadedTill = till;
				state->loadedLimit = state->limit;
				status->setText(Tr("Done. Read ", "تموم شد. خونده شد: ")
					+ QString::number(state->messages.size())
					+ Tr(" messages.", " پیام."));
				compute();
			}),
			crl::guard(box, [=](QString error) {
				state->cancel = nullptr;
				status->setText(QString::fromUtf8("❌ ") + error);
			}));
	};

	box->addButton(P(Tr("Calculate", "محاسبه")), calculate);
	box->addButton(P(Tr("Analyze with AI", "تحلیل با هوش مصنوعی")), [=] {
		if (!state->hasResult) {
			show->showToast(Tr("Calculate first.", "اول «محاسبه» رو بزن."));
			return;
		}
		const auto report = AyuStats::ToText(session, peer, state->stats);
		ShowAiAsk(controller, peer, FullMsgId(), QString(), nullptr, report);
	});
	box->addLeftButton(P(Tr("Copy", "کپی")), [=] {
		if (!state->hasResult) {
			show->showToast(Tr("Calculate first.", "اول «محاسبه» رو بزن."));
			return;
		}
		Copy(show, AyuStats::ToText(session, peer, state->stats));
	});
}

//
// Replies from several accounts.
//

struct MultiAccount {
	uint64 id = 0; // Session unique id.
	QString name;
	AyuAccountGroups::GroupId group = 0;
	Ui::Checkbox *check = nullptr;
};

struct MultiState {
	std::shared_ptr<std::vector<MultiAccount>> accounts;
	std::shared_ptr<uint64> providerId = std::make_shared<uint64>(0);
	bool busy = false;
	bool sending = false;
	QString chatName;
	std::vector<uint64> order; // Accounts of the generated replies.
	std::vector<Ui::InputField*> fields;
	std::vector<QPointer<Ui::FlatLabel>> statuses;
};

[[nodiscard]] std::pair<int, int> ParseDelay(const QString &text) {
	const auto parts = text.split('-', Qt::SkipEmptyParts);
	auto ok1 = false, ok2 = false;
	auto from = parts.value(0).trimmed().toInt(&ok1);
	auto till = parts.value(1, parts.value(0)).trimmed().toInt(&ok2);
	if (!ok1 || !ok2) {
		return { 8, 25 };
	}
	from = std::clamp(from, 0, 3600);
	till = std::clamp(till, from, 3600);
	return { from, till };
}

[[nodiscard]] std::shared_ptr<std::vector<MultiAccount>> AddAccountPicker(
		not_null<Ui::VerticalLayout*> content,
		uint64 current,
		const std::vector<uint64> &preselected) {
	const auto accounts = std::make_shared<std::vector<MultiAccount>>();
	AddTitle(content, Tr("Accounts", "اکانت‌ها"));
	for (const auto &session : AyuTools::Sessions()) {
		const auto key = AyuAccountGroups::AccountKey(session);
		const auto group = AyuAccountGroups::GroupOf(key);
		auto name = AyuAccountGroups::DisplayName(session);
		if (const auto found = AyuAccountGroups::FindGroup(group)) {
			name += u" · "_q + found->name;
		}
		const auto id = session->uniqueId();
		const auto checked = preselected.empty()
			? (id == current)
			: ranges::contains(preselected, id);
		accounts->push_back({
			.id = id,
			.name = AyuAccountGroups::DisplayName(session),
			.group = group,
			.check = AddCheck(content, name, checked),
		});
	}
	const auto selectWhere = [=](Fn<bool(const MultiAccount&)> test) {
		for (const auto &account : *accounts) {
			account.check->setChecked(test(account));
		}
	};
	const auto quick = AddRowButton(
		content,
		Tr("Quick select: all / category / none…", "انتخاب سریع: همه / دسته‌بندی / هیچ‌کدوم…"),
		nullptr);
	quick->setClickedCallback([=] {
		const auto menu = MakeMenu(quick);
		menu->addAction(Tr("All accounts", "همه اکانت‌ها"), [=] {
			selectWhere([](const MultiAccount &) { return true; });
		});
		for (const auto &group : AyuAccountGroups::Groups()) {
			const auto id = group.id;
			menu->addAction(Tr("Category: ", "دسته: ") + group.name, [=] {
				selectWhere([=](const MultiAccount &a) {
					return a.group == id;
				});
			});
		}
		menu->addAction(Tr("Only this account", "فقط همین اکانت"), [=] {
			selectWhere([=](const MultiAccount &a) {
				return a.id == current;
			});
		});
		menu->addAction(Tr("None", "هیچ‌کدوم"), [=] {
			selectWhere([](const MultiAccount &) { return false; });
		});
		menu->popup(QCursor::pos());
	});
	return accounts;
}

[[nodiscard]] std::vector<uint64> SelectedAccounts(
		const std::vector<MultiAccount> &accounts) {
	auto result = std::vector<uint64>();
	for (const auto &account : accounts) {
		if (account.check->checked()) {
			result.push_back(account.id);
		}
	}
	return result;
}

void MultiReplyBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		QString link,
		QString presetInstructions,
		std::vector<uint64> presetAccounts,
		bool autoGenerate) {
	static auto lastInstructions = QString();
	static auto lastAccounts = std::vector<uint64>();
	static auto lastDelay = u"8-25"_q;

	const auto show = std::shared_ptr<Ui::Show>(controller->uiShow());
	const auto state = std::make_shared<MultiState>();

	box->setTitle(P(Tr("Reply with my accounts", "ریپلای با اکانت‌هام")));
	box->setWidth(st::boxWideWidth);
	const auto content = box->verticalLayout();

	if (AyuAi::Providers().empty()) {
		AddLabel(
			content,
			Tr(
				"Add an AI service first (for example DeepSeek) and paste its API key.",
				"اول یه سرویس هوش مصنوعی (مثلا دیپ‌سیک) اضافه کن و کلیدش رو وارد کن."),
			st::ayuToolsAnswer);
		box->addButton(P(Tr("Add AI service", "افزودن سرویس")), [=] {
			box->closeBox();
			show->showBox(Box(ProvidersBox, show));
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		return;
	}

	const auto linkField = AddField(
		content,
		Tr("Message link (t.me/…)", "لینک پیام (t.me/…)"),
		link);
	const auto instructions = AddField(
		content,
		Tr(
			"What and how to reply? E.g.: agree with him, friendly and short",
			"چی و با چه لحنی جواب بدن؟ مثلا: باهاش موافقت کنن، دوستانه و کوتاه"),
		presetInstructions.isEmpty() ? lastInstructions : presetInstructions,
		true,
		st::ayuToolsBigField);

	const auto current = controller->session().uniqueId();
	state->accounts = AddAccountPicker(
		content,
		current,
		presetAccounts.empty() ? lastAccounts : presetAccounts);

	AddProviderChooser(content, state->providerId, true);
	const auto delay = AddField(
		content,
		Tr(
			"Seconds between accounts (e.g. 8-25)",
			"فاصله بین اکانت‌ها به ثانیه (مثلا 8-25)"),
		lastDelay);
	const auto join = AddCheck(
		content,
		Tr(
			"Join the group first if an account is not a member",
			"اگه اکانتی عضو گروه نبود اول عضو بشه"),
		false);

	const auto status = AddLabel(content, QString());
	const auto preview = content->add(
		object_ptr<Ui::VerticalLayout>(content),
		style::margins());

	const auto selected = [=] {
		return SelectedAccounts(*state->accounts);
	};
	const auto nameOf = [=](uint64 id) {
		for (const auto &account : *state->accounts) {
			if (account.id == id) {
				return account.name;
			}
		}
		return QString();
	};
	const auto setStatus = [=](const QString &text) {
		status->setText(text);
		content->resizeToWidth(st::boxWideWidth);
	};

	const auto showPreview = [=](
			const AyuMultiReply::Content &message,
			std::vector<uint64> accounts,
			std::vector<QString> replies) {
		preview->clear();
		state->order = accounts;
		state->fields.clear();
		state->statuses.clear();
		AddTitle(preview, Tr("The message", "پیام"));
		AddLabel(
			preview,
			message.author
				+ u": "_q
				+ ShortLine(message.text, 300)
				+ (message.imageJpeg.isEmpty()
					? QString()
					: QString::fromUtf8(" 🖼")),
			st::ayuToolsAnswer);
		AddLabel(
			preview,
			Tr(
				"You can edit the replies before sending.",
				"قبل از ارسال می‌تونی متن‌ها رو ویرایش کنی."));
		for (auto i = 0; i != int(accounts.size()); ++i) {
			AddTitle(preview, nameOf(accounts[i]));
			state->fields.push_back(AddField(
				preview,
				Tr("Reply", "متن ریپلای"),
				replies[i],
				true));
			state->statuses.push_back(QPointer<Ui::FlatLabel>(AddLabel(preview, QString()).get()));
		}
		Ui::AddSkip(preview, st::ayuToolsSkip);
		preview->resizeToWidth(st::boxWideWidth);
		content->resizeToWidth(st::boxWideWidth);
	};

	const auto generate = [=] {
		if (state->busy || state->sending) {
			return;
		}
		const auto parsed = AyuMultiReply::ParseLink(
			linkField->getLastText());
		if (!parsed.valid()) {
			linkField->showError();
			setStatus(Tr(
				"Paste a message link, for example t.me/group/123 "
				"(right-click a message > Copy link).",
				"لینک پیام رو بذار، مثلا t.me/group/123 "
				"(روی پیام راست‌کلیک > کپی لینک)."));
			return;
		}
		const auto accounts = selected();
		if (accounts.empty()) {
			setStatus(Tr("Choose at least one account.", "حداقل یه اکانت انتخاب کن."));
			return;
		}
		lastInstructions = instructions->getLastText().trimmed();
		lastAccounts = accounts;
		lastDelay = delay->getLastText().trimmed();

		// Read the message with the current account if it is chosen.
		auto reader = ranges::contains(accounts, current)
			? &controller->session()
			: AyuTools::FindSession(accounts.front());
		if (!reader) {
			return;
		}
		const auto session = not_null<Main::Session*>(reader);
		state->busy = true;
		setStatus(Tr("Reading the message…", "در حال خوندن پیام…"));
		AyuMultiReply::Resolve(session, parsed, crl::guard(box, [=](
				AyuMultiReply::Target target) {
			if (!target.peer) {
				state->busy = false;
				setStatus(QString::fromUtf8("❌ ") + target.error);
				return;
			}
			state->chatName = target.peer->name();
			AyuMultiReply::LoadContent(session, target, crl::guard(box, [=](
					AyuMultiReply::Content message) {
				if (!message.error.isEmpty()) {
					state->busy = false;
					setStatus(QString::fromUtf8("❌ ") + message.error);
					return;
				}
				setStatus(Tr("Writing replies…", "در حال نوشتن ریپلای‌ها…"));
				AyuMultiReply::Generate(
					*state->providerId,
					message,
					state->chatName,
					instructions->getLastText(),
					int(accounts.size()),
					crl::guard(box, [=](std::vector<QString> replies) {
						state->busy = false;
						setStatus(Tr(
							"Ready. Check the replies and press Send.",
							"آماده‌ست. ریپلای‌ها رو چک کن و «ارسال» رو بزن."));
						showPreview(message, accounts, replies);
					}),
					crl::guard(box, [=](QString error) {
						state->busy = false;
						setStatus(QString::fromUtf8("❌ ") + error);
					}));
			}));
		}));
	};

	const auto sendAll = [=] {
		if (state->busy || state->sending) {
			return;
		} else if (state->order.empty()) {
			setStatus(Tr(
				"Press \"Write replies\" first.",
				"اول «نوشتن ریپلای‌ها» رو بزن."));
			return;
		}
		const auto parsed = AyuMultiReply::ParseLink(
			linkField->getLastText());
		struct Job {
			uint64 account = 0;
			QString text;
			QPointer<Ui::FlatLabel> status;
		};
		auto jobs = std::vector<Job>();
		for (auto i = 0; i != int(state->order.size()); ++i) {
			const auto text = state->fields[i]->getLastText().trimmed();
			if (!text.isEmpty()) {
				jobs.push_back({ state->order[i], text, state->statuses[i] });
			}
		}
		if (jobs.empty()) {
			return;
		}
		state->sending = true;
		const auto [minDelay, maxDelay] = ParseDelay(delay->getLastText());
		const auto joinChecked = join->checked();
		const auto total = int(jobs.size());
		const auto sent = std::make_shared<int>(0);
		const auto queue = std::make_shared<std::vector<Job>>(std::move(jobs));
		const auto next = std::make_shared<Fn<void(int)>>();
		const auto weakBox = QPointer<Ui::GenericBox>(box.get());
		const auto mark = [=](const Job &job, const QString &text) {
			if (const auto label = job.status.data()) {
				label->setText(text);
			}
		};
		*next = [=](int index) {
			if (index >= int(queue->size())) {
				if (weakBox) {
					state->sending = false;
				}
				show->showToast(Tr("Sent: ", "ارسال شد: ")
					+ QString::number(*sent)
					+ u" / "_q
					+ QString::number(total));
				*next = nullptr;
				return;
			}
			const auto job = (*queue)[index];
			const auto session = AyuTools::FindSession(job.account);
			const auto goOn = [=] {
				if (index + 1 >= int(queue->size())) {
					(*next)(index + 1);
					return;
				}
				const auto seconds = minDelay
					+ (maxDelay > minDelay
						? int(base::RandomValue<uint32>() % uint32(maxDelay - minDelay + 1))
						: 0);
				if (const auto label = (*queue)[index + 1].status.data()) {
					label->setText(Tr("Waiting ", "در انتظار ")
						+ QString::number(seconds)
						+ Tr(" s…", " ثانیه…"));
				}
				base::call_delayed(crl::time(seconds) * 1000, [=] {
					(*next)(index + 1);
				});
			};
			if (!session) {
				mark(job, QString::fromUtf8("❌ ") + Tr(
					"The account is not logged in.",
					"اکانت وارد نیست."));
				goOn();
				return;
			}
			mark(job, Tr("Sending…", "در حال ارسال…"));
			AyuMultiReply::Resolve(session, parsed, [=](
					AyuMultiReply::Target target) {
				AyuMultiReply::Send(
					session,
					target,
					job.text,
					joinChecked,
					[=](QString error) {
						if (error.isEmpty()) {
							++*sent;
							mark(job, QString::fromUtf8("✅ ")
								+ Tr("Sent", "ارسال شد"));
						} else {
							mark(job, QString::fromUtf8("❌ ") + error);
						}
						goOn();
					});
			});
		};
		setStatus(Tr(
			"Sending one by one. You can close this window, it continues.",
			"یکی‌یکی ارسال میشه. می‌تونی پنجره رو ببندی، ادامه پیدا می‌کنه."));
		(*next)(0);
	};

	box->addButton(P(Tr("Write replies", "نوشتن ریپلای‌ها")), generate);
	if (autoGenerate) {
		crl::on_main(box, generate);
	}
	box->addButton(P(Tr("Send", "ارسال")), sendAll);
	box->addLeftButton(tr::lng_close(), [=] { box->closeBox(); });
}

void MultiReactBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		QString link,
		QString emoji,
		std::vector<uint64> presetAccounts,
		bool autoStart) {
	static auto lastAccounts = std::vector<uint64>();
	static auto lastDelay = u"3-10"_q;
	static auto lastEmoji = QString::fromUtf8("😂");

	const auto show = std::shared_ptr<Ui::Show>(controller->uiShow());
	box->setTitle(P(Tr("Reaction with my accounts", "ری‌اکشن با اکانت‌هام")));
	box->setWidth(st::boxWideWidth);
	const auto content = box->verticalLayout();

	const auto linkField = AddField(
		content,
		Tr("Message link (t.me/…)", "لینک پیام (t.me/…)"),
		link);
	const auto emojiField = AddField(
		content,
		Tr("Reaction", "ری‌اکشن"),
		emoji.isEmpty() ? lastEmoji : emoji);
	const auto choose = AddRowButton(
		content,
		Tr("Choose a reaction…", "انتخاب ری‌اکشن…"),
		nullptr);
	choose->setClickedCallback([=] {
		const auto menu = MakeMenu(choose);
		for (const auto &value : {
				"😂", "👍", "❤", "🔥", "😁", "👏", "🤣", "😍", "🎉",
				"🤩", "😢", "😱", "🤯", "🙏", "👌", "💯", "👎", "🤔" }) {
			const auto text = QString::fromUtf8(value);
			menu->addAction(text, [=] { emojiField->setText(text); });
		}
		menu->popup(QCursor::pos());
	});

	const auto current = controller->session().uniqueId();
	const auto accounts = AddAccountPicker(
		content,
		current,
		presetAccounts.empty() ? lastAccounts : presetAccounts);
	const auto delay = AddField(
		content,
		Tr(
			"Seconds between accounts (e.g. 3-10)",
			"فاصله بین اکانت‌ها به ثانیه (مثلا 3-10)"),
		lastDelay);
	const auto join = AddCheck(
		content,
		Tr(
			"Join the group first if an account is not a member",
			"اگه اکانتی عضو گروه نبود اول عضو بشه"),
		false);
	const auto status = AddLabel(content, QString());
	const auto results = content->add(
		object_ptr<Ui::VerticalLayout>(content),
		style::margins());
	const auto running = std::make_shared<bool>(false);

	const auto setStatus = [=](const QString &text) {
		status->setText(text);
		content->resizeToWidth(st::boxWideWidth);
	};
	const auto start = [=] {
		if (*running) {
			return;
		}
		const auto parsed = AyuMultiReply::ParseLink(linkField->getLastText());
		if (!parsed.valid()) {
			linkField->showError();
			setStatus(Tr(
				"Paste a message link, for example t.me/group/123 "
				"(right-click a message > Copy link).",
				"لینک پیام رو بذار، مثلا t.me/group/123 "
				"(روی پیام راست‌کلیک > کپی لینک)."));
			return;
		}
		const auto reaction = emojiField->getLastText().trimmed();
		if (reaction.isEmpty()) {
			emojiField->showError();
			return;
		}
		const auto chosen = SelectedAccounts(*accounts);
		if (chosen.empty()) {
			setStatus(Tr("Choose at least one account.", "حداقل یه اکانت انتخاب کن."));
			return;
		}
		lastAccounts = chosen;
		lastDelay = delay->getLastText().trimmed();
		lastEmoji = reaction;
		*running = true;

		results->clear();
		auto labels = std::vector<QPointer<Ui::FlatLabel>>();
		for (const auto id : chosen) {
			auto name = QString();
			for (const auto &account : *accounts) {
				if (account.id == id) {
					name = account.name;
				}
			}
			labels.push_back(QPointer<Ui::FlatLabel>(AddLabel(
				results,
				name + u": "_q + Tr("waiting", "در صف")).get()));
		}
		results->resizeToWidth(st::boxWideWidth);
		content->resizeToWidth(st::boxWideWidth);

		auto names = std::vector<QString>();
		for (const auto id : chosen) {
			for (const auto &account : *accounts) {
				if (account.id == id) {
					names.push_back(account.name);
				}
			}
		}
		const auto [minDelay, maxDelay] = ParseDelay(delay->getLastText());
		const auto joinChecked = join->checked();
		const auto total = int(chosen.size());
		setStatus(Tr(
			"Working one by one. You can close this window, it continues.",
			"یکی‌یکی انجام میشه. می‌تونی پنجره رو ببندی، ادامه پیدا می‌کنه."));
		AyuMultiReply::RunForAccounts(
			chosen,
			parsed,
			[=](
					not_null<Main::Session*> session,
					const AyuMultiReply::Target &target,
					Fn<void(QString)> done) {
				AyuMultiReply::React(
					session,
					target,
					reaction,
					joinChecked,
					std::move(done));
			},
			minDelay,
			maxDelay,
			[=](int index, QString text) {
				if (index < int(labels.size())) {
					if (const auto label = labels[index].data()) {
						label->setText(names[index] + u": "_q + text);
					}
				}
			},
			[=](int sent) {
				*running = false;
				show->showToast(Tr("Reactions: ", "ری‌اکشن‌ها: ")
					+ QString::number(sent)
					+ u" / "_q
					+ QString::number(total));
			});
	};

	box->addButton(P(Tr("Start", "شروع")), start);
	box->addLeftButton(tr::lng_close(), [=] { box->closeBox(); });
	if (autoStart) {
		crl::on_main(box, start);
	}
}

} // namespace

void ShowAiProviders(std::shared_ptr<Ui::Show> show) {
	show->showBox(Box(ProvidersBox, show));
}

void ShowAllAiTasks(not_null<Window::SessionController*> controller) {
	const auto show = std::shared_ptr<Ui::Show>(controller->uiShow());
	show->showBox(Box(AllTasksBox, show));
}

void ShowRepeatingMessages(not_null<Window::SessionController*> controller) {
	const auto show = std::shared_ptr<Ui::Show>(controller->uiShow());
	show->showBox(Box(RepeatingBox, show));
}

void ShowChatAiTasks(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		MsgId topicRootId) {
	const auto show = std::shared_ptr<Ui::Show>(controller->uiShow());
	show->showBox(Box(ChatTasksBox, show, peer, topicRootId));
}

void ShowAiAsk(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		FullMsgId replyTo,
		QString question,
		Fn<void(QString)> insert,
		QString extraContext) {
	controller->show(Box(
		AskBox,
		controller,
		peer,
		replyTo,
		question,
		insert,
		extraContext));
}

void ShowConversations(
		not_null<Window::SessionController*> controller,
		PeerData *peer) {
	const auto show = std::shared_ptr<Ui::Show>(controller->uiShow());
	show->showBox(Box(ConversationsBox, show, peer));
}

void ShowChatStats(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	controller->show(Box(StatsBox, controller, peer));
}

void ShowMultiReply(
		not_null<Window::SessionController*> controller,
		QString link) {
	controller->show(Box(
		MultiReplyBox,
		controller,
		link,
		QString(),
		std::vector<uint64>(),
		false));
}

void ShowMultiReact(
		not_null<Window::SessionController*> controller,
		QString link,
		QString emoji,
		std::vector<uint64> accounts,
		bool autoStart) {
	controller->show(Box(
		MultiReactBox,
		controller,
		link,
		emoji,
		accounts,
		autoStart));
}

void AddMessageToolsActions(
		not_null<Ui::PopupMenu*> menu,
		HistoryItem *item,
		not_null<Window::SessionController*> controller) {
	if (!item || !item->isRegular()) {
		return;
	}
	const auto weak = base::make_weak(controller);
	const auto peer = item->history()->peer;
	const auto itemId = item->fullId();
	menu->addAction(Tr("Ask AI about this", "پرسیدن از هوش مصنوعی درباره این"), [=] {
		if (const auto strong = weak.get()) {
			ShowAiAsk(strong, peer, itemId, QString(), nullptr);
		}
	}, &st::menuIconBot);
	const auto link = AyuMultiReply::MakeLink(item);
	if (!link.isEmpty()) {
		menu->addAction(Tr("Reply with my accounts (AI)", "ریپلای با اکانت‌هام (AI)"), [=] {
			if (const auto strong = weak.get()) {
				ShowMultiReply(strong, link);
			}
		}, &st::menuIconReply);
		menu->addAction(Tr("Reaction with my accounts", "ری‌اکشن با اکانت‌هام"), [=] {
			if (const auto strong = weak.get()) {
				ShowMultiReact(strong, link);
			}
		}, &st::menuIconReactions);
	}
}

void AddChatToolsMenu(
		not_null<PeerData*> peer,
		Data::Thread *thread,
		not_null<Window::SessionController*> controller,
		const Window::PeerMenuCallback &addAction) {
	const auto topicRootId = thread ? thread->topicRootId() : MsgId();
	const auto weak = base::make_weak(controller);
	addAction({
		.text = Tr("My tools", "ابزارهای من"),
		.handler = nullptr,
		.icon = &st::menuIconBot,
		.fillSubmenu = [=](not_null<Ui::PopupMenu*> menu) {
			menu->addAction(Tr("Chat statistics", "آمار چت"), [=] {
				if (const auto strong = weak.get()) {
					ShowChatStats(strong, peer);
				}
			}, &st::menuIconStats);
			menu->addAction(Tr("Ask AI", "پرسیدن از هوش مصنوعی"), [=] {
				if (const auto strong = weak.get()) {
					ShowAiAsk(strong, peer, FullMsgId(), QString(), nullptr);
				}
			}, &st::menuIconBot);
			menu->addAction(Tr("Reply with my accounts (AI)", "ریپلای با اکانت‌هام (AI)"), [=] {
				if (const auto strong = weak.get()) {
					ShowMultiReply(strong, QString());
				}
			}, &st::menuIconReply);
			menu->addAction(Tr("Reaction with my accounts", "ری‌اکشن با اکانت‌هام"), [=] {
				if (const auto strong = weak.get()) {
					ShowMultiReact(strong, QString());
				}
			}, &st::menuIconReactions);
			menu->addAction(Tr("AI conversations in this chat", "گفتگوهای هوش مصنوعی این چت"), [=] {
				if (const auto strong = weak.get()) {
					ShowConversations(strong, peer);
				}
			}, &st::menuIconDiscussion);
			menu->addAction(Tr("AI tasks for this chat", "وظیفه‌های هوش مصنوعی این چت"), [=] {
				if (const auto strong = weak.get()) {
					ShowChatAiTasks(strong, peer, topicRootId);
				}
			}, &st::menuIconSchedule);
			menu->addAction(Tr("Repeating messages", "پیام‌های تکراری"), [=] {
				if (const auto strong = weak.get()) {
					ShowRepeatingMessages(strong);
				}
			}, &st::menuIconRestore);
			menu->addAction(Tr("AI services", "سرویس‌های هوش مصنوعی"), [=] {
				if (const auto strong = weak.get()) {
					ShowAiProviders(strong->uiShow());
				}
			}, &st::menuIconSettings);
		},
	});
}

} // namespace AyuToolsUi
