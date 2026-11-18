// DebugNG page in the SKSE Menu Framework.

#include "PCH.h"

#include "SKSEMenuFramework.h"
#include "debug.h"

namespace
{
	using namespace ImGuiMCP;

	void Toggle(const char* a_label, std::atomic<bool>& a_flag)
	{
		bool v = a_flag.load();
		if (Checkbox(a_label, &v)) {
			a_flag.store(v);
			dbg::SaveSettings();
		}
	}

	void Slider(const char* a_label, std::atomic<int>& a_value, int a_min, int a_max, const char* a_format)
	{
		int v = a_value.load();
		if (SliderInt(a_label, &v, a_min, a_max, a_format)) {
			a_value.store(v);
		}
		if (IsItemDeactivatedAfterEdit()) {
			dbg::SaveSettings();
		}
	}

	void RunOnGameThread(void (*a_fn)())
	{
		if (auto tasks = SKSE::GetTaskInterface()) {
			tasks->AddTask(a_fn);
		}
	}

	// ---- page "Log": live numbers and log actions ------------------------------------
	void __stdcall RenderLogPage()
	{
		using dbg::L;
		dbg::SkipNextFrame();  // the game is paused while this window is open: the next frame is no hitch

		SeparatorText(L("Status", "Status"));
		TextColored(dbg::g_enabled ? ImVec4{ 0.45f, 0.90f, 0.45f, 1.0f } : ImVec4{ 1.0f, 0.55f, 0.15f, 1.0f }, "%s",
			dbg::g_enabled ? L("Aufzeichnung aktiv", "Logging enabled") : L("Aufzeichnung aus", "Logging off"));
		TextDisabled("%s", dbg::LogPath().c_str());
		Text(L("Zeilen geschrieben: %llu", "Lines written: %llu"), static_cast<unsigned long long>(dbg::g_lines.load()));

		SeparatorText(L("Ruckler", "Hitches"));
		Text(L("Bildrate: ~%d fps   Ruckler: %llu   letzter: %d ms   schlimmster: %d ms", "Frame rate: ~%d fps   hitches: %llu   last: %d ms   worst: %d ms"),
			dbg::g_lastFps.load(), static_cast<unsigned long long>(dbg::g_hitches.load()), dbg::g_lastHitchMs.load(), dbg::g_worstHitchMs.load());

		SeparatorText(L("Ins Log schreiben", "Write to the log"));
		if (Button(L("Markierung (\"JETZT\")", "Marker (\"NOW\")"))) {
			RunOnGameThread([] { dbg::LogWarn(dbg::kHitch, "---------- MARK (set by player) ----------"); });
		}
		SameLine();
		if (Button(L("Laufende Papyrus-Skripte", "Running Papyrus scripts"))) {
			RunOnGameThread([] { dbg::DumpStacks("manual", 60); });
		}
		SameLine();
		if (Button(L("Ladereihenfolge + SKSE-Plugins", "Load order + SKSE plugins"))) {
			RunOnGameThread([] {
				dbg::DumpLoadOrder();
				dbg::DumpSksePlugins();
			});
		}
		TextDisabled("%s", L("Tipp: Bei einem Ruckler oder Fehler sofort Markierung setzen - dann findet man die Stelle im Log.",
			"Tip: set a marker right when it stutters or breaks - makes the spot easy to find in the log."));
	}

	// ---- page "Settings" -----------------------------------------------------------------
	void __stdcall RenderSettingsPage()
	{
		using dbg::L;
		dbg::SkipNextFrame();  // the game is paused while this window is open: the next frame is no hitch

		SeparatorText(L("Allgemein", "General"));
		{
			int lang = dbg::g_english ? 1 : 0;
			Text("%s", L("Sprache:", "Language:"));
			SameLine();
			bool changed = RadioButton("Deutsch", &lang, 0);
			SameLine();
			changed |= RadioButton("English", &lang, 1);
			if (changed) {
				dbg::g_english = lang == 1;
				dbg::SaveSettings();
			}
		}
		TextDisabled("%s", L("Die Seitennamen links wechseln erst nach einem Neustart des Spiels.", "The page names on the left change after restarting the game."));
		Toggle(L("Aufzeichnung aktiv", "Logging enabled"), dbg::g_enabled);

		SeparatorText(L("Loglevel", "Log level"));
		{
			int level = dbg::g_level.load();
			bool changed = RadioButton(L("Nur Fehler", "Errors only"), &level, dbg::kErrors);
			SameLine();
			changed |= RadioButton(L("Wichtig", "Important"), &level, dbg::kImportant);
			SameLine();
			changed |= RadioButton("Normal", &level, dbg::kNormal);
			SameLine();
			changed |= RadioButton(L("Ausfuehrlich", "Verbose"), &level, dbg::kVerbose);
			if (changed) {
				dbg::g_level = level;
				dbg::SaveSettings();
			}
			static const char* const kDescDe[] = {
				"Nur Fehler von DebugNG selbst.",
				"Ruckler (mit laufenden Papyrus-Skripten), Steuerungssperren und Markierungen.",
				"Alles, was unten eingeschaltet ist.",
				"Zusaetzlich: 10-s-Bildraten-Zusammenfassung und Freigabe von ModEvents."
			};
			static const char* const kDescEn[] = {
				"Only errors of DebugNG itself.",
				"Hitches (with running Papyrus scripts), control locks and markers.",
				"Everything switched on below.",
				"Also: 10 s frame rate summaries and ModEvent releases."
			};
			TextDisabled("%s", L(kDescDe[level], kDescEn[level]));
		}

		SeparatorText(L("Filter", "Filter"));
		Toggle(L("Nur Ereignisse mit dem Spieler (Kampf, Ausruesten, Treffer ...)", "Only events involving the player (combat, equip, hits ...)"), dbg::g_playerOnly);

		SeparatorText(L("Ruckler", "Hitches"));
		Slider(L("Als Ruckler zaehlen ab", "Count as hitch from"), dbg::g_hitchMs, 20, 500, "%d ms");
		Slider(L("Papyrus-Skripte pro Ruckler", "Papyrus scripts per hitch"), dbg::g_stackLines, 1, 60, "%d");

		SeparatorText(L("Was aufgezeichnet wird", "What is logged"));
		for (int i = 0; i < dbg::kCount; ++i) {
			Toggle(L(dbg::kCats[i].de, dbg::kCats[i].en), dbg::g_cat[i]);
		}
		TextDisabled("%s", L("Magie, Treffer, Gegenstaende und Objekte laden sind sehr gespraechig - nur bei Bedarf.",
			"Magic, hits, items and object loading are very chatty - only when needed."));
		TextDisabled("%s", L("Gespeichert in SKSE\\DebugNG.ini", "Stored in SKSE\\DebugNG.ini"));
	}

	// ---- page "Emergency" ------------------------------------------------------------------
	void __stdcall RenderEmergencyPage()
	{
		using dbg::L;
		dbg::SkipNextFrame();  // the game is paused while this window is open: the next frame is no hitch

		SeparatorText(L("Steuerung", "Controls"));
		if (Button(L("Steuerung zurueckgeben", "Give controls back"))) {
			RunOnGameThread([] { dbg::ResetPlayerControls(); });
		}
		TextWrapped("%s", L("Hebt Steuerungssperren, KI-Steuerung, Charaktererstellungs-Modus, Fesselung und Bewegungssperre auf, "
							"setzt eine haengende Animation zurueck und holt die Kamera in die Third-Person-Ansicht "
							"- fuer den Fall, dass eine Mod (z. B. nach einer Niederlage) die Kontrolle nicht zurueckgibt. "
							"Wirkt nach dem Schliessen des Menues.",
			"Lifts disabled controls, AI-driven mode, char-gen mode, restrained and don't-move states, resets a stuck animation "
			"state and puts the camera back to third person - for when a mod (e.g. after a defeat) does not give control back. "
			"Takes effect after closing the menu."));

		SeparatorText(L("Diagnose", "Diagnostics"));
		if (Button(L("Spielerzustand speichern", "Save player state"))) {
			RunOnGameThread([] { dbg::DumpPlayerState("manual"); });
		}
		Toggle(L("Auch inaktive Effekte auflisten (lang)", "Also list inactive effects (long)"), dbg::g_stateInactiveEffects);
		TextWrapped("%s", L("Schreibt Steuerungssperren, Eingabe-Kontexte, Kamera, Animations-Zustand, alle Effekte und alle laufenden Skripte "
							"in eine eigene Datei DebugNG-Player_<JJJJ-MM-TT_hh-mm-ss>.log neben DebugNG.log (die 20 neuesten bleiben) "
							"- damit sieht man, was blockiert, wenn \"Steuerung zurueckgeben\" nicht hilft.",
			"Writes control locks, input contexts, camera, animation state, all effects and all running scripts to its own file "
			"DebugNG-Player_<yyyy-mm-dd_hh-mm-ss>.log next to DebugNG.log (the 20 newest are kept) "
			"- shows what is blocking when \"Give controls back\" does not help."));
	}
}

namespace dbg
{
	void RegisterMenu()
	{
		if (!GetMenuFrameworkModule()) {
			SKSE::log::info("SKSE Menu Framework not loaded - no menu page");
			return;
		}
		SKSEMenuFramework::SetSection("DebugNG");
		SKSEMenuFramework::AddSectionItem("Log", RenderLogPage);
		SKSEMenuFramework::AddSectionItem(L("Einstellungen", "Settings"), RenderSettingsPage);
		SKSEMenuFramework::AddSectionItem(L("Notfall", "Emergency"), RenderEmergencyPage);
		SKSE::log::info("menu page registered in SKSE Menu Framework");
	}
}
