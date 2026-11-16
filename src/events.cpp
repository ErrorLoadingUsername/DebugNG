// Game and SKSE events -> DebugNG.log, plus load order and SKSE plugin list.

#include "PCH.h"
#include "debug.h"
#include "win.h"

namespace dbg
{
	std::string FormID(std::uint32_t a_id)
	{
		return fmt::format("{:08X}", a_id);
	}

	std::string Form(const RE::TESForm* a_form)
	{
		if (!a_form) {
			return "None";
		}
		const char* name = a_form->GetName();
		auto        file = a_form->GetFile(0);
		return fmt::format("{} [{:08X} {}]", name && *name ? name : "-", a_form->GetFormID(),
			file ? std::string(file->GetFilename()) : "?"s);
	}

	std::string Ref(const RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) {
			return "None";
		}
		const char* name = a_ref->GetName();
		return fmt::format("{} [{:08X}]", name && *name ? name : "-", a_ref->GetFormID());
	}

	namespace
	{
		std::string FileOf(const RE::TESForm* a_form, std::int32_t a_idx)
		{
			auto file = a_form ? a_form->GetFile(a_idx) : nullptr;
			return file ? std::string(file->GetFilename()) : "?"s;
		}

		// refs attached since the last frame: plugin that placed the ref -> count
		std::mutex                                   g_attachLock;
		std::unordered_map<std::string, std::uint32_t> g_attachByFile;
		std::uint32_t                                g_attachTotal = 0;
	}

	// e.g.  - [2303826E] Static "-" [0001F2A3 Skyrim.esm] placed by Midnight Sun.esm, last edit Patch.esp
	std::string RefDetail(const RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) {
			return "None";
		}
		const char* name = a_ref->GetName();
		auto        base = a_ref->GetBaseObject();
		std::string out  = fmt::format("{} [{:08X}]", name && *name ? name : "-", a_ref->GetFormID());
		if (base) {
			const char* bname = base->GetName();
			out += fmt::format("  {} \"{}\" [{:08X} {}]", RE::FormTypeToString(base->GetFormType()), bname && *bname ? bname : "-",
				base->GetFormID(), FileOf(base, 0));
		}
		const auto placed = FileOf(a_ref, 0);
		const auto last   = FileOf(a_ref, -1);
		out += "  placed by " + placed;
		if (last != placed) {
			out += ", last edit " + last;
		}
		return out;
	}

	void DumpAttachStats(bool a_log)
	{
		std::scoped_lock l(g_attachLock);
		if (a_log && g_attachTotal > 0) {
			std::vector<std::pair<std::string, std::uint32_t>> v(g_attachByFile.begin(), g_attachByFile.end());
			std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
			std::string top;
			for (std::size_t i = 0; i < v.size() && i < 8; ++i) {
				top += fmt::format("{}{} {}", top.empty() ? "" : ", ", v[i].first, v[i].second);
			}
			LogWarn(kHitch, fmt::format("  cell attach during this frame: {} ref(s) - {}", g_attachTotal, top));
		}
		g_attachByFile.clear();
		g_attachTotal = 0;
	}

	bool IsPlayer(const RE::TESObjectREFR* a_ref)
	{
		return a_ref && a_ref == RE::PlayerCharacter::GetSingleton();
	}
}

namespace
{
	using namespace dbg;

	// with "player only": skip actor events that do not involve the player
	bool Wanted(const RE::TESObjectREFR* a, const RE::TESObjectREFR* b = nullptr)
	{
		return !g_playerOnly || IsPlayer(a) || IsPlayer(b);
	}

	template <class E>
	class Sink final : public RE::BSTEventSink<E>
	{
	public:
		using Fn = void (*)(const E&);
		explicit Sink(Fn a_fn) :
			fn(a_fn) {}
		RE::BSEventNotifyControl ProcessEvent(const E* a_event, RE::BSTEventSource<E>*) override
		{
			if (a_event && g_enabled) {
				fn(*a_event);
			}
			return RE::BSEventNotifyControl::kContinue;
		}
		Fn fn;
	};

	template <class E>
	void Listen(RE::BSTEventSource<E>* a_source, void (*a_fn)(const E&))
	{
		static Sink<E> sink(a_fn);  // one sink per event type
		if (a_source) {
			a_source->AddEventSink(&sink);
		}
	}

	template <class E>
	void ListenScript(void (*a_fn)(const E&))
	{
		if (auto holder = RE::ScriptEventSourceHolder::GetSingleton()) {
			Listen<E>(holder->GetEventSource<E>(), a_fn);
		}
	}

	// a pointer that really is a loaded form (readable, and the form table agrees)
	const RE::TESForm* ValidForm(const RE::TESForm* a_form)
	{
		if (!a_form || !win::ReadableRange(a_form, 0x20)) {
			return nullptr;
		}
		const auto id = a_form->GetFormID();
		return id && RE::TESForm::LookupByID(id) == a_form ? a_form : nullptr;
	}

	const char* CombatState(std::uint32_t a_state)
	{
		return a_state == 1 ? "combat" : a_state == 2 ? "searching" : "none";
	}
}

namespace dbg
{
	void RegisterEvents()
	{
		// ---- menus (also tell the frame monitor to skip the next frame) ----
		if (auto ui = RE::UI::GetSingleton()) {
			Listen<RE::MenuOpenCloseEvent>(ui->GetEventSource<RE::MenuOpenCloseEvent>(), [](const RE::MenuOpenCloseEvent& e) {
				SkipNextFrame();
				if (On(kMenus)) {
					Log(kMenus, fmt::format("{} {}", e.opening ? "open " : "close", e.menuName.c_str()));
				}
			});
		}

		// ---- script events ----
		ListenScript<RE::TESActivateEvent>([](const RE::TESActivateEvent& e) {
			if (On(kActivate) && Wanted(e.actionRef.get())) {
				Log(kActivate, fmt::format("{} activates {}", Ref(e.actionRef.get()), Ref(e.objectActivated.get())));
			}
		});
		ListenScript<RE::TESCombatEvent>([](const RE::TESCombatEvent& e) {
			if (On(kCombat) && Wanted(e.actor.get(), e.targetActor.get())) {
				Log(kCombat, fmt::format("{} -> {}: {}", Ref(e.actor.get()), Ref(e.targetActor.get()), CombatState(static_cast<std::uint32_t>(e.newState.get()))));
			}
		});
		ListenScript<RE::TESDeathEvent>([](const RE::TESDeathEvent& e) {
			if (On(kDeath) && Wanted(e.actorDying.get(), e.actorKiller.get())) {
				Log(kDeath, fmt::format("{} {} (killer {})", Ref(e.actorDying.get()), e.dead ? "died" : "dying", Ref(e.actorKiller.get())));
			}
		});
		ListenScript<RE::TESEnterBleedoutEvent>([](const RE::TESEnterBleedoutEvent& e) {
			if (On(kDeath) && Wanted(e.actor.get())) {
				Log(kDeath, fmt::format("{} enters bleedout", Ref(e.actor.get())));
			}
		});
		ListenScript<RE::TESEquipEvent>([](const RE::TESEquipEvent& e) {
			if (On(kEquip) && Wanted(e.actor.get())) {
				Log(kEquip, fmt::format("{} {} {}", Ref(e.actor.get()), e.equipped ? "equips  " : "unequips", Form(RE::TESForm::LookupByID(e.baseObject))));
			}
		});
		ListenScript<RE::TESHitEvent>([](const RE::TESHitEvent& e) {
			if (On(kHits) && Wanted(e.cause.get(), e.target.get())) {
				Log(kHits, fmt::format("{} hits {} with {}", Ref(e.cause.get()), Ref(e.target.get()), Form(RE::TESForm::LookupByID(e.source))));
			}
		});
		ListenScript<RE::TESMagicEffectApplyEvent>([](const RE::TESMagicEffectApplyEvent& e) {
			if (On(kMagic) && Wanted(e.caster.get(), e.target.get())) {
				Log(kMagic, fmt::format("effect {} on {} from {}", Form(RE::TESForm::LookupByID(e.magicEffect)), Ref(e.target.get()), Ref(e.caster.get())));
			}
		});
		ListenScript<RE::TESSpellCastEvent>([](const RE::TESSpellCastEvent& e) {
			if (On(kMagic) && Wanted(e.object)) {
				Log(kMagic, fmt::format("{} casts {}", Ref(e.object), Form(RE::TESForm::LookupByID(e.spell))));
			}
		});
		ListenScript<RE::TESActiveEffectApplyRemoveEvent>([](const RE::TESActiveEffectApplyRemoveEvent& e) {
			if (On(kMagic) && Wanted(e.caster.get(), e.target.get())) {
				Log(kMagic, fmt::format("active effect #{} {} on {}", e.activeEffectUniqueID, e.isApplied ? "added" : "removed", Ref(e.target.get())));
			}
		});
		ListenScript<RE::TESQuestStageEvent>([](const RE::TESQuestStageEvent& e) {
			if (On(kQuests)) {
				Log(kQuests, fmt::format("{} stage {}", Form(RE::TESForm::LookupByID(e.formID)), e.stage));
			}
		});
		ListenScript<RE::TESQuestStartStopEvent>([](const RE::TESQuestStartStopEvent& e) {
			if (On(kQuests)) {
				Log(kQuests, fmt::format("{} {}", Form(RE::TESForm::LookupByID(e.formID)), e.started ? "started" : "stopped"));
			}
		});
		ListenScript<RE::TESSleepStopEvent>([](const RE::TESSleepStopEvent& e) {
			if (On(kSleepWait)) {
				Log(kSleepWait, fmt::format("sleep ended{}", e.interrupted ? " (interrupted)" : ""));
			}
		});
		ListenScript<RE::TESWaitStopEvent>([](const RE::TESWaitStopEvent& e) {
			if (On(kSleepWait)) {
				Log(kSleepWait, fmt::format("wait ended{}", e.interrupted ? " (interrupted)" : ""));
			}
		});
		ListenScript<RE::TESFastTravelEndEvent>([](const RE::TESFastTravelEndEvent& e) {
			if (On(kSleepWait)) {
				Log(kSleepWait, fmt::format("fast travel ended ({:.1f} h)", e.fastTravelEndHours));
			}
		});
		ListenScript<RE::TESFurnitureEvent>([](const RE::TESFurnitureEvent& e) {
			if (On(kFurniture) && Wanted(e.actor.get())) {
				Log(kFurniture, fmt::format("{} {} {}", Ref(e.actor.get()), e.type.get() == RE::TESFurnitureEvent::FurnitureEventType::kEnter ? "enters" : "leaves", Ref(e.targetFurniture.get())));
			}
		});
		ListenScript<RE::TESCellFullyLoadedEvent>([](const RE::TESCellFullyLoadedEvent& e) {
			if (On(kCells) && e.cell) {
				const char* edid = e.cell->GetFormEditorID();
				Log(kCells, fmt::format("cell loaded {} ({})", Form(e.cell), edid && *edid ? edid : "-"));
			}
		});
		ListenScript<RE::TESActorLocationChangeEvent>([](const RE::TESActorLocationChangeEvent& e) {
			if (On(kCells) && IsPlayer(e.actor.get())) {
				Log(kCells, fmt::format("location {} -> {}", Form(e.oldLoc), Form(e.newLoc)));
			}
		});
		ListenScript<RE::TESContainerChangedEvent>([](const RE::TESContainerChangedEvent& e) {
			if (On(kContainer)) {
				const auto player = RE::PlayerCharacter::GetSingleton();
				const auto pid    = player ? player->GetFormID() : 0x14u;
				if (!g_playerOnly || e.oldContainer == pid || e.newContainer == pid) {
					Log(kContainer, fmt::format("{}x {} : {} -> {}", e.itemCount, Form(RE::TESForm::LookupByID(e.baseObj)), FormID(e.oldContainer), FormID(e.newContainer)));
				}
			}
		});
		ListenScript<RE::TESObjectLoadedEvent>([](const RE::TESObjectLoadedEvent& e) {
			if (On(kObjectLoad)) {
				Log(kObjectLoad, fmt::format("{} {}", e.loaded ? "loaded  " : "unloaded", Form(RE::TESForm::LookupByID(e.formID))));
			}
		});
		ListenScript<RE::TESCellAttachDetachEvent>([](const RE::TESCellAttachDetachEvent& e) {
			if (On(kObjectLoad)) {
				Log(kObjectLoad, fmt::format("{} {}", e.attached ? "attached" : "detached", RefDetail(e.reference)));
				if (e.attached && e.reference) {
					std::scoped_lock l(g_attachLock);
					g_attachByFile[FileOf(e.reference, 0)]++;
					g_attachTotal++;
				}
			}
		});
		ListenScript<RE::TESOpenCloseEvent>([](const RE::TESOpenCloseEvent& e) {
			if (On(kOpenClose) && Wanted(e.activeRef.get())) {
				Log(kOpenClose, fmt::format("{} {} {}", Ref(e.activeRef.get()), e.opened ? "opens" : "closes", Ref(e.ref.get())));
			}
		});
		ListenScript<RE::TESTrackedStatsEvent>([](const RE::TESTrackedStatsEvent& e) {
			if (On(kStats)) {
				Log(kStats, fmt::format("{} = {}", e.stat.c_str(), e.value));
			}
		});
		ListenScript<RE::TESInitScriptEvent>([](const RE::TESInitScriptEvent& e) {
			if (On(kScriptInit)) {
				Log(kScriptInit, fmt::format("init {}", Ref(e.objectInitialized.get())));
			}
		});
		ListenScript<RE::TESSwitchRaceCompleteEvent>([](const RE::TESSwitchRaceCompleteEvent& e) {
			if (On(kEquip) && Wanted(e.subject.get())) {
				Log(kEquip, fmt::format("{} switched race", Ref(e.subject.get())));
			}
		});
		ListenScript<RE::TESLoadGameEvent>([](const RE::TESLoadGameEvent&) {
			Log(kCells, "game loaded");
		});

		// ---- SKSE events ----
		Listen<SKSE::ModCallbackEvent>(SKSE::GetModCallbackEventSource(), [](const SKSE::ModCallbackEvent& e) {
			if (!On(kModCallback)) {
				return;
			}
			// Other plugins send these too and not all of them fill every field
			// correctly (dmenu put text into "sender") - read everything guarded.
			char name[128]{}, str[256]{};
			win::SafeStringAt(&e, offsetof(SKSE::ModCallbackEvent, eventName), name, sizeof(name));
			win::SafeStringAt(&e, offsetof(SKSE::ModCallbackEvent, strArg), str, sizeof(str));
			const RE::TESForm* sender = ValidForm(e.sender);
			Log(kModCallback, fmt::format("{}  str=\"{}\" num={} sender={}", name[0] ? name : "?", str, e.numArg,
				sender ? Form(sender) : (e.sender ? "<invalid>"s : "None"s)));
		});
		Listen<SKSE::NiNodeUpdateEvent>(SKSE::GetNiNodeUpdateEventSource(), [](const SKSE::NiNodeUpdateEvent& e) {
			auto ref = static_cast<const RE::TESObjectREFR*>(ValidForm(e.reference));
			if (On(kNiNodeUpdate) && ref && Wanted(ref)) {
				Log(kNiNodeUpdate, fmt::format("model update {}", Ref(ref)));
			}
		});
		Listen<SKSE::ActionEvent>(SKSE::GetActionEventSource(), [](const SKSE::ActionEvent& e) {
			auto actor = static_cast<const RE::TESObjectREFR*>(ValidForm(e.actor));
			if (On(kActions) && actor && Wanted(actor)) {
				static constexpr const char* names[] = { "weapon swing", "spell cast", "spell fire", "voice cast", "voice fire",
					"bow draw", "bow release", "begin draw", "end draw", "begin sheathe", "end sheathe" };
				const auto t = static_cast<std::uint32_t>(e.type.get());
				Log(kActions, fmt::format("{} {} {}", Ref(actor), t < std::size(names) ? names[t] : "?", Form(ValidForm(e.sourceForm))));
			}
		});
	}

	void DumpLoadOrder()
	{
		auto data = RE::TESDataHandler::GetSingleton();
		if (!data || REL::Module::IsVR()) {
			return;
		}
		// TESFileCollection at +0xD70 (SE/AE); CommonLib's light-mod count is only 8 bit
		const auto& collection = REL::RelocateMember<const RE::TESFileCollection>(data, 0xD70, 0);
		const auto& files      = collection.files;
		const auto& light      = collection.smallFiles;
		Log(kCells, fmt::format("load order: {} full plugin(s), {} light plugin(s)", files.size(), light.size()));
		for (auto f : files) {
			if (f) {
				Log(kCells, fmt::format("  [{:02X}]     {}", f->GetCompileIndex(), f->GetFilename()));
			}
		}
		for (auto f : light) {
			if (f) {
				Log(kCells, fmt::format("  [FE:{:03X}] {}", f->GetSmallFileCompileIndex(), f->GetFilename()));
			}
		}
	}

	void DumpSksePlugins()
	{
		std::vector<std::string> plugins;
		for (const auto& w : win::LoadedModules()) {
			std::string path(w.size(), '?');
			std::transform(w.begin(), w.end(), path.begin(), [](wchar_t c) { return c < 128 ? static_cast<char>(c) : '?'; });
			std::string lower = path;
			std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
			if (lower.find("\\skse\\plugins\\") != std::string::npos) {
				plugins.push_back(path.substr(path.find_last_of('\\') + 1));
			}
		}
		std::sort(plugins.begin(), plugins.end());
		Log(kCells, fmt::format("SKSE plugins loaded: {}", plugins.size()));
		for (const auto& p : plugins) {
			Log(kCells, "  " + p);
		}
	}
}
