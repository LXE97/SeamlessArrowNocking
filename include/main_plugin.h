#pragma once

#include "SKSE/Impl/Stubs.h"
#include "VR/OpenVRUtils.h"
#include "VR/PapyrusVRAPI.h"
#include "VR/VRManagerAPI.h"
#include "menu_checker.h"
#include "mod_event_sink.hpp"
#include "mod_input.h"

namespace arrownock
{
	extern PapyrusVRAPI* g_papyrusvr;

	void StartMod();

	void EquippedEventHandler(const RE::TESEquipEvent* event);

	bool OnArrowButton(const vrinput::ModInputEvent& e);

	void OnOverlap(PapyrusVR::VROverlapEvent e, uint32_t id, PapyrusVR::VRDevice device);

	void RegisterVRInputCallback();
}
