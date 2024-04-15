
#include "main_plugin.h"

#include <chrono>

namespace arrownock
{
	PapyrusVRAPI*       g_papyrusvr;
	bool                g_left_hand_mode = false;
	uint32_t            g_overlap_handle = 0;
	PapyrusVR::Matrix34 overlap_transform;

	bool            g_nocked = false;
	vr::EVRButtonId g_button = vr::EVRButtonId::k_EButton_Max;
	vr::EVRButtonId g_firebutton = vr::EVRButtonId::k_EButton_SteamVR_Trigger;
	float           g_overlap_radius = 10.f;

	void StartMod() 
	{
		auto          config_path = helper::GetGamePath() / "SKSE/Plugins/SeamlessArrowNocking.ini";
		std::ifstream config(config_path);
		if (config.is_open())
		{
			g_left_hand_mode = helper::ReadIntFromIni(config, "iLeftHandMode");
			g_firebutton = (vr::EVRButtonId)helper::ReadIntFromIni(config, "iFireButtonID");
            g_overlap_radius = helper::ReadFloatFromIni(config, "fOverlapRadius");
            
			config.close();
		}
		else { SKSE::log::error("ini not found, using defaults"); }

		auto equip_sink = EventSink<RE::TESEquipEvent>::GetSingleton();
		RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink(equip_sink);
		equip_sink->AddCallback(EquippedEventHandler);
	}

	void EquippedEventHandler(const RE::TESEquipEvent* event)
	{
		if (event->actor.get() == RE::PlayerCharacter::GetSingleton())
		{
			if (!menuchecker::isGameStopped())
			{
				// is equipped object an arrow
				if (auto form = RE::TESForm::LookupByID(event->baseObject); form && form->IsAmmo())
				{
					// does player have bow equipped
					if (auto weap = RE::PlayerCharacter::GetSingleton()->GetEquippedObject(
							!g_left_hand_mode);
						weap && weap->IsWeapon() && weap->As<RE::TESObjectWEAP>()->IsBow())
					{
						// get pressed button
						g_button =
							(bool)vrinput::GetButtonState(vr::k_EButton_SteamVR_Trigger,
								(vrinput::Hand)g_left_hand_mode, vrinput::ActionType::kPress) ?
							vr::k_EButton_SteamVR_Trigger :
							(bool)vrinput::GetButtonState(vr::k_EButton_A,
								(vrinput::Hand)g_left_hand_mode, vrinput::ActionType::kPress) ?
							vr::k_EButton_A :
							(bool)vrinput::GetButtonState(vr::k_EButton_Knuckles_B,
								(vrinput::Hand)g_left_hand_mode, vrinput::ActionType::kPress) ?
							vr::k_EButton_Knuckles_B :
							(bool)vrinput::GetButtonState(vr::k_EButton_SteamVR_Touchpad,
								(vrinput::Hand)g_left_hand_mode, vrinput::ActionType::kPress) ?
							vr::k_EButton_SteamVR_Touchpad :
							(bool)vrinput::GetButtonState(vr::k_EButton_Grip,
								(vrinput::Hand)g_left_hand_mode, vrinput::ActionType::kPress) ?
							vr::k_EButton_Grip :
							vr::k_EButton_Max;

						if (g_button != vr::k_EButton_Max)
						{
							SKSE::log::trace("arrow equipped with button press: {}", g_button);

							// register release listener
							vrinput::AddCallback(OnArrowButton, g_button,
								(vrinput::Hand)g_left_hand_mode, vrinput::ActionType::kPress);

							// register overlap sphere
							g_overlap_handle =
								g_papyrusvr->GetVRManager()->CreateLocalOverlapSphere(
									g_overlap_radius, &overlap_transform,
									g_left_hand_mode ?
										PapyrusVR::VRDevice::VRDevice_RightController :
										PapyrusVR::VRDevice::VRDevice_LeftController);
						}
					}
				}
			}
		}
	}

	bool OnArrowButton(const vrinput::ModInputEvent& e)
	{
		SKSE::log::trace("2nd arrow button event");
		if (e.button_state == vrinput::ButtonState::kButtonUp)
		{
			g_nocked = false;

			// stop spoofing
			vrinput::ClearFakeButtonState({
				.device = g_left_hand_mode ? vrinput::Hand::kLeft : vrinput::Hand::kRight,
				.touch_or_press = vrinput::ActionType::kPress,
				.button_state = vrinput::ButtonState::kButtonDown,
				.button_ID = vr::k_EButton_SteamVR_Trigger,
			});

			g_papyrusvr->GetVRManager()->DestroyLocalOverlapObject(g_overlap_handle);

			vrinput::RemoveCallback(OnArrowButton, g_button, (vrinput::Hand)g_left_hand_mode,
				vrinput::ActionType::kPress);

			g_button = vr::k_EButton_Max;
		}
		return false;
	}

	void OnOverlap(PapyrusVR::VROverlapEvent e, uint32_t id, PapyrusVR::VRDevice device)
	{
		SKSE::log::trace("Overlap Event : {}",
			e == PapyrusVR::VROverlapEvent::VROverlapEvent_OnEnter ? "enter" : "exit");

		if (id == g_overlap_handle &&
				(g_left_hand_mode && device == PapyrusVR::VRDevice::VRDevice_LeftController) ||
			(!g_left_hand_mode && device == PapyrusVR::VRDevice::VRDevice_RightController))
		{
			if (e == PapyrusVR::VROverlapEvent::VROverlapEvent_OnEnter)
			{
				g_nocked = true;

				// spoof arrow nock input
				if (g_button == vr::k_EButton_SteamVR_Trigger)
				{
					// no hold, just send momentary release
					vrinput::SendFakeInputEvent({
						.device = g_left_hand_mode ? vrinput::Hand::kLeft : vrinput::Hand::kRight,
						.touch_or_press = vrinput::ActionType::kPress,
						.button_state = vrinput::ButtonState::kButtonUp,
						.button_ID = g_button,
					});
				}
				else
				{
					vrinput::SetFakeButtonState({
						.device = g_left_hand_mode ? vrinput::Hand::kLeft : vrinput::Hand::kRight,
						.touch_or_press = vrinput::ActionType::kPress,
						.button_state = vrinput::ButtonState::kButtonDown,
						.button_ID = vr::k_EButton_SteamVR_Trigger,
					});
				}
			}
		}
	}

	// Register SkyrimVRTools callback
	void RegisterVRInputCallback()
	{
		auto OVRHookManager = g_papyrusvr->GetOpenVRHook();
		if (OVRHookManager && OVRHookManager->IsInitialized())
		{
			OVRHookManager = RequestOpenVRHookManagerObject();
			if (OVRHookManager)
			{
				SKSE::log::info("Successfully requested OpenVRHookManagerAPI.");

				vrinput::g_leftcontroller =
					OVRHookManager->GetVRSystem()->GetTrackedDeviceIndexForControllerRole(
						vr::TrackedControllerRole_LeftHand);
				vrinput::g_rightcontroller =
					OVRHookManager->GetVRSystem()->GetTrackedDeviceIndexForControllerRole(
						vr::TrackedControllerRole_RightHand);
				OVRHookManager->RegisterControllerStateCB(vrinput::ControllerInputCallback);
			}
		}
		else { SKSE::log::error("Failed to initialize OVRHookManager"); }
	}
}
