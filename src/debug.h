#pragma once

// DebugNG - writes what happens in the game to Documents\My Games\<game>\SKSE\DebugNG.log:
// game events, ModEvents, menus, frame hitches with the Papyrus stacks running at
// that moment, load order and loaded SKSE plugins. Everything can be switched in
// the SKSE Menu Framework page "DebugNG" and in DebugNG.ini.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace RE
{
	class TESForm;
	class TESObjectREFR;
}

namespace dbg
{
	enum Cat : int
	{
		kHitch,          // frame time: hitches + 10 s summary
		kModEventStore,  // ModEvent.Create / Send (SKSE object storage)
		kModCallback,    // Form.SendModEvent (SKSE mod callback events)
		kMenus,
		kCells,          // cell loaded, location change
		kCombat,
		kDeath,          // death + bleedout
		kQuests,
		kEquip,
		kActivate,
		kFurniture,
		kSleepWait,      // sleep, wait, fast travel
		kNiNodeUpdate,   // SKSE NiNode update (body/morph updates)
		kStats,          // tracked stats
		kOpenClose,
		kMagic,          // spell cast, magic effect apply, active effects
		kHits,
		kContainer,
		kObjectLoad,     // object loaded, cell attach/detach
		kActions,        // SKSE actions: weapon swing, draw, bow ...
		kScriptInit,
		kControls,       // player controls enabled / disabled (with the script that did it)
		kCount
	};

	struct CatInfo
	{
		const char* key;  // INI key and log tag
		const char* de;
		const char* en;
		bool        def;  // default on/off
	};

	inline constexpr CatInfo kCats[kCount] = {
		{ "HITCH", "Ruckler / Frame-Zeiten (mit laufenden Papyrus-Skripten)", "Hitches / frame times (with running Papyrus scripts)", true },
		{ "MODEVENT", "ModEvent.Create / Send (SKSE)", "ModEvent.Create / Send (SKSE)", true },
		{ "MODCB", "SendModEvent (Form)", "SendModEvent (Form)", true },
		{ "MENU", "Menues", "Menus", true },
		{ "CELL", "Zellen / Orte", "Cells / locations", true },
		{ "COMBAT", "Kampf", "Combat", true },
		{ "DEATH", "Tod / Bleedout", "Death / bleedout", true },
		{ "QUEST", "Quests (Start, Stufe, Ende)", "Quests (start, stage, stop)", true },
		{ "EQUIP", "Ausruesten", "Equip", true },
		{ "ACTIVATE", "Aktivieren", "Activate", true },
		{ "FURNITURE", "Moebel", "Furniture", true },
		{ "SLEEPWAIT", "Schlafen / Warten / Schnellreise", "Sleep / wait / fast travel", true },
		{ "NINODE", "Modell-Updates (NiNode, z. B. Morphs)", "Model updates (NiNode, e.g. morphs)", true },
		{ "STATS", "Statistiken", "Tracked stats", true },
		{ "OPENCLOSE", "Oeffnen / Schliessen", "Open / close", false },
		{ "MAGIC", "Magie (Zauber, Effekte)", "Magic (spells, effects)", false },
		{ "HIT", "Treffer", "Hits", false },
		{ "CONTAINER", "Gegenstaende verschieben", "Item transfers", false },
		{ "OBJLOAD", "Objekte laden / Zellen anhaengen", "Object load / cell attach", false },
		{ "ACTION", "Aktionen (Schwingen, Ziehen, Bogen)", "Actions (swing, draw, bow)", false },
		{ "SCRIPTINIT", "Skript-Initialisierung", "Script init", false },
		{ "CONTROLS", "Steuerungssperren (wer sperrt was)", "Player controls (who disables what)", true },
	};

	inline std::atomic<bool> g_cat[kCount]{};
	inline std::atomic<bool> g_enabled{ true };
	inline std::atomic<bool> g_playerOnly{ true };  // actor events: only those involving the player
	inline std::atomic<bool> g_english{ true };
	inline std::atomic<bool> g_stateInactiveEffects{ false };  // player state file: also list inactive effects
	inline std::atomic<int>  g_hitchMs{ 40 };       // frames slower than this are logged
	inline std::atomic<int>  g_stackLines{ 15 };    // Papyrus stacks listed per hitch

	// log level: 0 = errors only, 1 = important (hitches, control locks, markers),
	//            2 = normal (all enabled modules), 3 = verbose (+ 10 s frame summaries, ModEvent releases)
	enum Level : int { kErrors = 0, kImportant = 1, kNormal = 2, kVerbose = 3 };
	inline std::atomic<int>  g_level{ kNormal };
	inline constexpr const char* kLevelKeys[] = { "errors", "important", "normal", "verbose" };

	// counters for the menu page
	inline std::atomic<std::uint64_t> g_lines{ 0 };
	inline std::atomic<std::uint64_t> g_hitches{ 0 };
	inline std::atomic<int>           g_lastHitchMs{ 0 };
	inline std::atomic<int>           g_worstHitchMs{ 0 };
	inline std::atomic<int>           g_lastFps{ 0 };

	// a module only runs if its lines can reach the log at the current level
	// (hitches and control locks are "important", everything else needs "normal")
	inline bool On(Cat a_cat)
	{
		if (!g_enabled || !g_cat[a_cat]) {
			return false;
		}
		const int need = (a_cat == kHitch || a_cat == kControls) ? kImportant : kNormal;
		return g_level >= need;
	}
	inline const char* L(const char* a_de, const char* a_en) { return g_english ? a_en : a_de; }

	// main.cpp
	void        Log(Cat a_cat, const std::string& a_text);
	void        LogWarn(Cat a_cat, const std::string& a_text);   // level "important"
	void        LogDebug(Cat a_cat, const std::string& a_text);  // level "verbose"
	void        SaveSettings();
	void        SkipNextFrame();  // menus / loading: do not count the next frame as a hitch
	std::string LogPath();
	void        ResetPlayerControls();  // "give me my controls back" - run on the game thread
	void        DumpPlayerState(const char* a_reason);  // everything that can lock the player - game thread

	// names (events.cpp)
	std::string Form(const RE::TESForm* a_form);
	std::string FormID(std::uint32_t a_id);
	std::string Ref(const RE::TESObjectREFR* a_ref);
	bool        IsPlayer(const RE::TESObjectREFR* a_ref);
	std::string RefDetail(const RE::TESObjectREFR* a_ref);  // + base object type/name and plugins
	void        DumpAttachStats(bool a_log);                // refs attached since the last frame, by plugin
	void        DumpActorStats();                           // loaded actors by AI process level / activity, at a hitch
	void        RegisterEvents();
	void        DumpLoadOrder();
	void        DumpSksePlugins();

	// papyrus.cpp
	bool        InitStorage();
	void        PollModEvents();
	std::size_t RunningStackCount();
	void        DumpStacks(const char* a_reason, int a_max);
	std::vector<std::string> StackLines(const char* a_reason, int a_max);  // 0 = all

	// ui.cpp
	void RegisterMenu();
}
