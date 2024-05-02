#pragma once

#include "SKSE/Impl/Stubs.h"
#include "VR/OpenVRUtils.h"
#include "VR/PapyrusVRAPI.h"
#include "VR/VRManagerAPI.h"
#include "menu_checker.h"
#include "mod_event_sink.hpp"
#include "vrinput.h"

#define _DEBUGLOG(...) \
	if (arrownock::g_debug_print) { SKSE::log::trace(__VA_ARGS__); }

namespace arrownock
{
	constexpr const char* g_ini_path = "SKSE/Plugins/SeamlessArrowNocking.ini";

	extern PapyrusVRAPI* g_papyrusvr;
	extern bool          g_left_hand_mode;
	extern bool          g_debug_print;

	void Init();

	void OnGameLoad();

	void OnUpdate();

	void OnMenuOpenClose(RE::MenuOpenCloseEvent const* evn);

	void OnEquipped(const RE::TESEquipEvent* event);

	bool OnButtonEvent(const vrinput::ModInputEvent& e);

	bool IsOverlapping(float a_radius_squared);

	bool IsArrowNocked();

	void GetBowBaseAngle(RE::NiPoint3* out);

	void TryNockArrow(bool a_start_spoof);

	void PlayStaminaInhibitorFX();

	/* true: player has enough stamina */
	bool TestStamina(float a_threshold);

	void UnregisterButtons(bool isLeft);
	void RegisterButtons(bool isLeft);

	void RegisterVRInputCallback();

	/* returns: true if config file changed */
	bool ReadConfig(const char* a_ini_path);
}
