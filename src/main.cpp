// DebugNG - logs what happens in the game. See debug.h.

#include "PCH.h"
#include "debug.h"
#include "win.h"

namespace
{
	using Clock = std::chrono::steady_clock;

	std::filesystem::path g_logPath;
	std::filesystem::path g_iniPath;
	std::mutex            g_iniMutex;
	std::atomic<bool>     g_skipFrame{ true };
	Clock::time_point     g_quietUntil{};  // no hitch reports until then (menus, loading)

	// <Documents>\My Games\<game>\SKSE (follows a moved Documents folder), then CommonLib.
	std::filesystem::path FindLogDir()
	{
		std::error_code ec;
		if (const auto docs = dbg::win::DocumentsDir(); !docs.empty()) {
			for (const auto* game : { "Skyrim Special Edition", "Skyrim Special Edition GOG" }) {
				auto dir = std::filesystem::path(docs) / "My Games" / game / "SKSE";
				if (std::filesystem::is_directory(dir, ec)) {
					return dir;
				}
			}
		}
		if (auto dir = SKSE::log::log_directory(); dir && std::filesystem::is_directory(*dir, ec)) {
			return *dir;
		}
		return std::filesystem::current_path(ec) / "Data" / "SKSE" / "Plugins";
	}

	void SetupLog()
	{
		g_logPath = FindLogDir() / "DebugNG.log";
		try {
			auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(g_logPath.string(), true);
			auto log  = std::make_shared<spdlog::logger>("global", std::move(sink));
			log->set_level(spdlog::level::info);
			log->flush_on(spdlog::level::warn);  // hitches are written at once,
			spdlog::set_default_logger(std::move(log));
			spdlog::set_pattern("[%H:%M:%S.%e] %v");
			spdlog::flush_every(std::chrono::seconds(1));  // everything else once per second
		} catch (...) {
			g_logPath.clear();
		}
	}

	void LoadSettings()
	{
		for (int i = 0; i < dbg::kCount; ++i) {
			dbg::g_cat[i] = dbg::kCats[i].def;
		}
		std::ifstream file(g_iniPath);
		std::string   line;
		while (std::getline(file, line)) {
			const auto eq = line.find('=');
			if (line.empty() || line[0] == ';' || line[0] == '[' || eq == std::string::npos) {
				continue;
			}
			const auto key   = line.substr(0, eq);
			const int  value = std::atoi(line.c_str() + eq + 1);
			if (key == "enabled") {
				dbg::g_enabled = value != 0;
			} else if (key == "playerOnly") {
				dbg::g_playerOnly = value != 0;
			} else if (key == "playerStateInactiveEffects") {
				dbg::g_stateInactiveEffects = value != 0;
			} else if (key == "english") {
				dbg::g_english = value != 0;
			} else if (key == "hitchMs") {
				dbg::g_hitchMs = std::clamp(value, 20, 1000);
			} else if (key == "logLevel") {
				dbg::g_level = std::clamp(value, 0, 3);
			} else if (key == "stackLines") {
				dbg::g_stackLines = std::clamp(value, 1, 60);
			} else {
				for (int i = 0; i < dbg::kCount; ++i) {
					if (key == dbg::kCats[i].key) {
						dbg::g_cat[i] = value != 0;
					}
				}
			}
		}
	}

	// ---- frame monitor: hook PlayerCharacter::Update (vfunc 0xAD, called every frame) ----
	struct FrameStats
	{
		Clock::time_point last{};
		Clock::time_point windowStart{};
		double            sumMs   = 0.0;
		double            maxMs   = 0.0;
		int               frames  = 0;
		int               hitches = 0;
	} g_frames;

	double            g_avgMs = 0.0;          // rolling average frame time
	Clock::time_point g_lastStackDump{};      // stack dumps at most every 2 s

	// The enabled-controls flags of RE::ControlMap. CommonLib knows where they are on every runtime
	// (+0x118 SE / AE before 1.6.1130, +0x120 from 1.6.1130 on, +0x138 VR).
	std::uint32_t EnabledControls(const RE::ControlMap* a_map)
	{
		return a_map->GetRuntimeData().enabledControls.underlying();
	}

	// The input context stack of RE::ControlMap (gameplay, menu mode, TFC ...). On 1.6.1130+ the
	// game has one more input context, so the array sits 8 bytes later (+0x108 instead of +0x100):
	// BSTArray = data pointer, capacity, size (+0x10). Normal play: only "gameplay" on the stack.
	// A context left on the stack by a mod blocks events like "Ready Weapon" or "Toggle POV".
	std::string InputContexts(const RE::ControlMap* a_map)
	{
		// CommonLib knows where the stack is on every runtime (+0x100 SE, +0x108 from 1.6.1130 on, +0x120 VR)
		const auto& stack = a_map->GetRuntimeData().contextPriorityStack;
		const auto  size  = stack.size();
		if (size > 64) {
			return fmt::format("(unreadable: size {})", size);
		}
		// 1.6.1130+ has one more context (marketplace) before "favor"
		static const bool marketplace = REL::Module::IsAE() && REL::Module::get().version() >= REL::Version{ 1, 6, 1130, 0 };
		static constexpr const char* kNames[] = { "gameplay", "menu mode", "console", "item menu", "inventory",
			"debug text", "favorites", "map", "stats", "cursor", "book", "debug overlay", "journal", "TFC mode",
			"map debug", "lockpicking", "favor" };
		std::string out;
		for (std::uint32_t i = 0; i < size; ++i) {
			const auto  id   = static_cast<std::uint32_t>(stack[i]);
			const char* name = "?";
			if (marketplace && id == 16) {
				name = "marketplace";
			} else if (const auto n = marketplace && id > 16 ? id - 1 : id; n < std::size(kNames)) {
				name = kNames[n];
			}
			out += fmt::format("{}{}({})", out.empty() ? "" : " > ", name, id);
		}
		return fmt::format("{} context(s): {}", size, out.empty() ? "-"s : out);
	}

	// which player controls are disabled right now, e.g. "fighting, POV switch"
	std::string DisabledControls(const RE::ControlMap* a_map)
	{
		const auto  flags = EnabledControls(a_map);
		std::string out;
		auto add = [&](std::uint32_t a_bit, const char* a_name) {
			if ((flags & a_bit) == 0) {
				out += out.empty() ? a_name : ", "s + a_name;
			}
		};
		add(1u << 0, "movement");
		add(1u << 1, "looking");
		add(1u << 5, "POV switch");
		add(1u << 6, "fighting");
		add(1u << 7, "sneaking");
		add(1u << 10, "jumping");
		add(1u << 3, "menu");
		add(1u << 2, "activate");
		add(1u << 8, "journal tabs");
		add(1u << 4, "console");
		return out.empty() ? "none"s : out;
	}

	// logs every change of the disabled controls, with the scripts running at that moment
	void WatchControls()
	{
		static std::string last = "?";
		auto map = RE::ControlMap::GetSingleton();
		if (!map) {
			return;
		}
		// raw words around the enabled-controls field, to verify its offset on this game version
		const auto rawStr = fmt::format("{:08X}", EnabledControls(map));
		static std::string lastRaw;
		auto now = DisabledControls(map);
		if (now == last && rawStr == lastRaw) {
			return;
		}
		lastRaw = rawStr;
		dbg::LogWarn(dbg::kControls, fmt::format("disabled controls: {}  (before: {})  flags {}", now, last, rawStr));
		if (last != "?") {
			dbg::DumpStacks("controls changed", 25);
		}
		last = std::move(now);
	}

	void OnFrame()
	{
		const auto now = Clock::now();
		auto&      f   = g_frames;
		if (g_skipFrame.exchange(false) || f.last == Clock::time_point{}) {
			f.last       = now;  // after menus / loading the frames are not hitches:
			g_quietUntil = now + 750ms;  // HUD widgets and scripts restart for a moment
			return;
		}
		const double ms = std::chrono::duration<double, std::milli>(now - f.last).count();
		f.last          = now;
		if (!dbg::g_enabled) {
			return;
		}

		if (dbg::On(dbg::kModEventStore)) {
			dbg::PollModEvents();
		}
		if (dbg::On(dbg::kControls)) {
			WatchControls();
		}

		if (!dbg::On(dbg::kHitch)) {
			return;
		}
		if (f.windowStart == Clock::time_point{}) {
			f.windowStart = now;
		}
		f.sumMs += ms;
		f.maxMs = (std::max)(f.maxMs, ms);
		++f.frames;

		// A hitch is a frame that is slow in absolute terms AND clearly slower than
		// the frames around it - at a low but steady frame rate, not every frame
		// is a hitch (that would also make the logging itself expensive).
		const double avg   = g_avgMs > 0.0 ? g_avgMs : ms;
		const bool   hitch = ms >= dbg::g_hitchMs && ms >= avg * 1.75 && now >= g_quietUntil;
		g_avgMs            = avg * 0.95 + ms * 0.05;
		if (hitch) {
			++f.hitches;
			dbg::g_hitches++;
			dbg::g_lastHitchMs = static_cast<int>(ms);
			dbg::g_worstHitchMs = (std::max)(dbg::g_worstHitchMs.load(), static_cast<int>(ms));
			dbg::LogWarn(dbg::kHitch, fmt::format("HITCH {:.0f} ms (average {:.0f} ms)", ms, avg));
			dbg::DumpAttachStats(true);  // which plugins' references were attached in this frame
			dbg::DumpActorStats();       // how many actors the AI / pathfinding had to handle
			if (now - g_lastStackDump >= 2s) {
				g_lastStackDump = now;
				dbg::DumpStacks("now", dbg::g_stackLines);
			}
		}

		if (!hitch) {
			dbg::DumpAttachStats(false);  // reset the per-frame counter
		}

		if (now - f.windowStart >= 10s && f.frames > 0) {
			const double windowAvg = f.sumMs / f.frames;
			dbg::g_lastFps          = static_cast<int>(1000.0 / windowAvg + 0.5);
			dbg::LogDebug(dbg::kHitch, fmt::format("10 s: {} frames, avg {:.1f} ms ({:.0f} fps), max {:.0f} ms, {} hitch(es)",
				f.frames, windowAvg, 1000.0 / windowAvg, f.maxMs, f.hitches));
			f = FrameStats{ f.last, now };
		}
	}

	struct PlayerUpdate
	{
		static void thunk(RE::PlayerCharacter* a_this, float a_delta)
		{
			func(a_this, a_delta);
			OnFrame();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	void InstallFrameHook()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCharacter[0] };
		PlayerUpdate::func = vtbl.write_vfunc(0xAD, PlayerUpdate::thunk);
		dbg::Log(dbg::kHitch, "frame monitor installed (PlayerCharacter::Update)");
	}
}

namespace dbg
{
	void Log(Cat a_cat, const std::string& a_text)
	{
		if (g_level < kNormal) {
			return;
		}
		g_lines++;
		SKSE::log::info("[{:<10}] {}", kCats[a_cat].key, a_text);
	}

	void LogWarn(Cat a_cat, const std::string& a_text)
	{
		if (g_level < kImportant) {
			return;
		}
		g_lines++;
		SKSE::log::warn("[{:<10}] {}", kCats[a_cat].key, a_text);
	}

	void LogDebug(Cat a_cat, const std::string& a_text)
	{
		if (g_level < kVerbose) {
			return;
		}
		g_lines++;
		SKSE::log::info("[{:<10}] {}", kCats[a_cat].key, a_text);
	}

	void SkipNextFrame() { g_skipFrame = true; }

	// Writes every state that can take control away from the player, so the log shows
	// what is blocking when the ControlMap itself says "nothing disabled".
	void DumpPlayerState(const char* a_reason)
	{
		auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
		}
		// everything goes into its own file DebugNG-Player_<yyyy-mm-dd_hh-mm-ss>.log next to DebugNG.log,
		// complete (all effects, all scripts); DebugNG.log only gets one line pointing to it
		std::vector<std::string> lines;
		auto put = [&lines](const std::string& a_text) { lines.push_back(a_text); };
		const auto now = std::time(nullptr);
		std::tm    tm{};
		localtime_s(&tm, &now);
		char stamp[32]{};
		std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &tm);
		char when[32]{};
		std::strftime(when, sizeof(when), "%d.%m.%Y %H:%M:%S", &tm);
		put(fmt::format("DebugNG player state ({}) - {} - Skyrim {}", a_reason, when, REL::Module::get().version().string(".")));
		put("");

		if (auto map = RE::ControlMap::GetSingleton()) {
			put(fmt::format("  controls disabled: {}  (flags {:08X})", DisabledControls(map), EnabledControls(map)));
			put("  input " + InputContexts(map));
		}

		static constexpr const char* kCamNames[] = { "first person", "auto vanity", "VATS", "free", "iron sights",
			"furniture", "transition", "tween", "animated", "third person", "mount", "bleedout", "dragon" };
		if (auto cam = RE::PlayerCamera::GetSingleton(); cam && cam->currentState) {
			const auto id = static_cast<std::uint32_t>(cam->currentState->id);
			put(fmt::format("  camera: {} ({})", id < std::size(kCamNames) ? kCamNames[id] : "?", id));
		}

		const auto state = player->AsActorState();
		put(fmt::format("  AI enabled: {}   kill move: {}   bleeding out: {}   weapon drawn: {}   sit/sleep state: {}   knock state: {}",
			player->IsAIEnabled(), player->IsInKillMove(), state->IsBleedingOut(), state->IsWeaponDrawn(),
			static_cast<int>(state->GetSitSleepState()), static_cast<int>(state->GetKnockState())));

		// animation graph: a graph stuck in a scene / paired / furniture state blocks drawing and POV
		static constexpr const char* kGraphBools[] = { "bAnimationDriven", "bIsSynced", "IsEquipping", "IsUnequipping",
			"IsStaggering", "IsRecoiling", "IsAttacking", "IsBlocking", "bInJumpState", "IsSneaking", "bAllowRotation",
			"bHeadTrackSpine", "IsInFurniture", "bIsRiding", "IsBleedingOut" };
		std::string graph;
		for (const auto* name : kGraphBools) {
			bool value = false;
			if (player->GetGraphVariableBool(name, value)) {
				graph += fmt::format("{}{}={}", graph.empty() ? "" : "  ", name, value ? 1 : 0);
			}
		}
		put("  graph: " + (graph.empty() ? "(no animation graph)"s : graph));

		// active effects (Defeat / DA / bondage mods lock the player through effects too)
		if (auto target = player->AsMagicTarget()) {
			if (auto list = target->GetActiveEffectList()) {
				std::vector<std::string> active, inactive;
				for (auto* effect : *list) {
					if (!effect) {
						continue;
					}
					const auto text = fmt::format("    {}  from {}", Form(effect->GetBaseObject()), Form(effect->spell));
					(effect->flags.any(RE::ActiveEffect::Flag::kInactive) ? inactive : active).push_back(text);
				}
				put(fmt::format("  active effects: {}", active.size()));
				for (const auto& e : active) {
					put(e);
				}
				if (g_stateInactiveEffects) {
					put(fmt::format("  inactive effects: {}", inactive.size()));
					for (const auto& e : inactive) {
						put(e);
					}
				} else {
					put(fmt::format("  inactive effects: {} (not listed - see playerStateInactiveEffects)", inactive.size()));
				}
			}
		}
		put("");
		for (const auto& l : StackLines("player state", 0)) {
			put(l);
		}

		const auto name = fmt::format("DebugNG-Player_{}.log", stamp);
		const auto dir  = g_logPath.parent_path();
		{
			std::ofstream file(dir / name, std::ios::trunc);
			for (const auto& l : lines) {
				file << l << "\n";
			}
		}
		LogWarn(kControls, fmt::format("player state ({}) written to {}", a_reason, name));

		// keep the 20 newest player state files
		std::error_code ec;
		std::vector<std::filesystem::directory_entry> files;
		for (const auto& f : std::filesystem::directory_iterator(dir, ec)) {
			const auto fn = f.path().filename().string();
			if (fn.rfind("DebugNG-Player_", 0) == 0 && f.path().extension() == ".log") {
				files.push_back(f);
			}
		}
		if (files.size() > 20) {
			std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
				std::error_code e1, e2;
				return a.last_write_time(e1) > b.last_write_time(e2);
			});
			for (std::size_t i = 20; i < files.size(); ++i) {
				std::filesystem::remove(files[i].path(), ec);
			}
		}
	}

	// Undo everything mods typically use to take control away from the player
	// (Death Alternative, Defeat, scenes ...). Same calls as the console / DA's own reset.
	void ResetPlayerControls()
	{
		auto player = RE::PlayerCharacter::GetSingleton();
		auto vm     = RE::BSScript::Internal::VirtualMachine::GetSingleton();
		if (!player || !vm) {
			return;
		}
		const auto before = RE::ControlMap::GetSingleton() ? DisabledControls(RE::ControlMap::GetSingleton()) : "?"s;
		LogWarn(kControls, fmt::format("---------- RESET CONTROLS (set by player) - disabled before: {} ----------", before));
		DumpPlayerState("before reset");

		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> none;
		auto staticCall = [&](const char* a_class, const char* a_fn, RE::BSScript::IFunctionArguments* a_args) {
			vm->DispatchStaticCall(a_class, a_fn, a_args, none);
		};
		// Game.EnablePlayerControls(all), SetInCharGen(false...), SetPlayerAIDriven(false)
		staticCall("Game", "EnablePlayerControls", RE::MakeFunctionArguments(true, true, true, true, true, true, true, true, 0));
		staticCall("Game", "SetInCharGen", RE::MakeFunctionArguments(false, false, false));
		staticCall("Game", "SetPlayerAIDriven", RE::MakeFunctionArguments(false));
		staticCall("Debug", "SetGodMode", RE::MakeFunctionArguments(false));

		// player.EnableAI(true), SetRestrained(false), SetDontMove(false)
		if (auto policy = vm->GetObjectHandlePolicy()) {
			const auto handle = policy->GetHandleForObject(RE::Actor::FORMTYPE, player);
			if (handle != policy->EmptyHandle()) {
				vm->DispatchMethodCall(handle, "Actor", "EnableAI", RE::MakeFunctionArguments(true), none);
				vm->DispatchMethodCall(handle, "Actor", "SetRestrained", RE::MakeFunctionArguments(false), none);
				vm->DispatchMethodCall(handle, "Actor", "SetDontMove", RE::MakeFunctionArguments(false), none);
			}
		}

		// (no ControlMap::ToggleControls here: on 1.6.1170 CommonLib's enabledControls
		//  offset does not match - it would write into the wrong field)
		player->SetAIDriven(false);
		player->EnableAI(true);

		// animation graph stuck in a scene / paired animation: back to the default idle state
		player->NotifyAnimationGraph("IdleForceDefaultState");
		// camera stuck in a scene / furniture / free state: back to third person
		staticCall("Game", "ForceThirdPerson", RE::MakeFunctionArguments());

		RE::SendHUDMessage::ShowHUDMessage(L("DebugNG: Steuerung zurueckgegeben", "DebugNG: controls restored"));
	}

	std::string LogPath() { return g_logPath.string(); }

	void SaveSettings()
	{
		std::scoped_lock l(g_iniMutex);
		if (g_iniPath.empty()) {
			return;
		}
		std::ofstream file(g_iniPath, std::ios::trunc);
		file << "; DebugNG settings - 1 = on, 0 = off\n"
			 << "; Can also be changed in game: SKSE Menu Framework -> DebugNG -> Log (written back here).\n"
			 << "; Changes made in this file take effect at the next game start.\n"
			 << "\n[General]\n"
			 << "; master switch: 0 = DebugNG logs nothing at all\n"
			 << "enabled=" << int(g_enabled.load()) << "\n"
			 << "; actor events (combat, equip, hits ...) only when the player is involved\n"
			 << "playerOnly=" << int(g_playerOnly.load()) << "\n"
			 << "; \"Save player state\": also list inactive effects (long); 0 = only count them\n"
			 << "playerStateInactiveEffects=" << int(g_stateInactiveEffects.load()) << "\n"
			 << "; language of the menu page: 0 = German, 1 = English\n"
			 << "english=" << int(g_english.load()) << "\n"
			 << "; how much is written: 0 = errors only, 1 = important (hitches with Papyrus stacks,\n"
			 << ";   control locks, markers), 2 = normal (all enabled modules), 3 = verbose\n"
			 << ";   (also 10 s frame summaries and ModEvent releases)\n"
			 << "logLevel=" << g_level.load() << "\n"
			 << "; frames slower than this (ms, 20-1000) are logged as hitch\n"
			 << "hitchMs=" << g_hitchMs.load() << "\n"
			 << "; number of running Papyrus scripts listed per hitch (1-60)\n"
			 << "stackLines=" << g_stackLines.load() << "\n"
			 << "\n[Modules]\n";
		for (int i = 0; i < kCount; ++i) {
			file << "; " << kCats[i].en << (kCats[i].def ? "" : "  (default off, very chatty)") << "\n"
				 << kCats[i].key << "=" << int(g_cat[i].load()) << "\n";
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	SetupLog();
	g_iniPath = FindLogDir() / "DebugNG.ini";
	LoadSettings();
	dbg::SaveSettings();  // create / rewrite with all keys and comments (keeps the loaded values)
	SKSE::log::info("DebugNG 1.1.3 (beta) - {}", REL::Module::get().version().string("."));
	SKSE::log::info("log: {}   settings: {}", g_logPath.string(), g_iniPath.string());
	if (REL::Module::IsVR()) {
		SKSE::log::warn("Skyrim VR: not tested yet - please report whether DebugNG works");
	}

	SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* a_msg) {
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			dbg::RegisterMenu();
			dbg::DumpLoadOrder();
			dbg::DumpSksePlugins();
			dbg::InitStorage();
			dbg::RegisterEvents();
			InstallFrameHook();
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			dbg::SkipNextFrame();
			SKSE::log::info("[{:<10}] ---- loading a save ----", "GAME");
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			dbg::SkipNextFrame();
			SKSE::log::info("[{:<10}] ---- save loaded ----", "GAME");
			break;
		case SKSE::MessagingInterface::kNewGame:
			dbg::SkipNextFrame();
			SKSE::log::info("[{:<10}] ---- new game ----", "GAME");
			break;
		case SKSE::MessagingInterface::kSaveGame:
			SKSE::log::info("[{:<10}] ---- saving ----", "GAME");
			break;
		default:
			break;
		}
	});
	return true;
}
