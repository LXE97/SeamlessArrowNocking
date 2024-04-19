#include "main_plugin.h"

#include <chrono>

namespace arrownock
{
	constexpr float      kArrowDistanceToNockDefault = 18.f;
	constexpr float      kOverlapRadiusDefault = 0.240;
	constexpr std::array kCheckButtons{ vr::k_EButton_SteamVR_Trigger, vr::k_EButton_A,
		vr::k_EButton_Knuckles_B, vr::k_EButton_SteamVR_Touchpad, vr::k_EButton_Grip };

	PapyrusVRAPI* g_papyrusvr;
	bool          g_vrik_disabled = true;

	// settings
	bool               g_left_hand_mode = false;
	float              g_overlap_radius = 0.1f;
	PapyrusVR::Vector3 g_overlap_offset = { 0, 0.1, 0.05 };
	bool               g_debug_print = false;
	vr::EVRButtonId    g_firebutton = vr::EVRButtonId::k_EButton_SteamVR_Trigger;
	int                g_grace_period_ms = 500;

	float g_angle_diff_threshold = 0.005f;
	int   g_frames_between_attempts = 3;

	// state
	bool            g_want_arrow_nocked = false;
	bool            g_arrow_held = false;
	vr::EVRButtonId g_arrow_held_button = vr::EVRButtonId::k_EButton_Max;
	RE::NiPoint3    g_unbent_bow_angle;

	void StartMod()
	{
		g_vrik_disabled = GetModuleHandleA("vrik") == NULL;
		ReadConfig(g_ini_path);
		SKSE::log::info("VRIK {} found", g_vrik_disabled ? "not" : "DLL");

		auto equip_sink = EventSink<RE::TESEquipEvent>::GetSingleton();
		equip_sink->AddCallback(OnEquipped);
		RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink(equip_sink);

		auto menu_sink = EventSink<RE::MenuOpenCloseEvent>::GetSingleton();
		menu_sink->AddCallback(OnMenuOpenClose);
		RE::UI::GetSingleton()->AddEventSink(menu_sink);

		menuchecker::begin();
		RegisterButtons();
		RegisterVRInputCallback();
	}

	void OnGameLoad()
	{
		g_want_arrow_nocked = false;
		g_arrow_held = false;
		g_arrow_held_button = vr::EVRButtonId::k_EButton_Max;
	}

	void OnPoseUpdate()
	{
		if (!menuchecker::isGameStopped() && g_want_arrow_nocked)
		{
			static int frame_count = 0;
			if (frame_count++ % g_frames_between_attempts == 0)
			{
				if (IsArrowNocked()) { g_want_arrow_nocked = false; }

				else { TryNockArrow(); }
			}
		}
	}

	void OnOverlap(bool entered)
	{
		if (entered && g_arrow_held && !menuchecker::isGameStopped())
		{
			g_want_arrow_nocked = true;
		}
		else if (!entered) { g_want_arrow_nocked = false; }
	}

	void OnEquipped(const RE::TESEquipEvent* event)
	{
		if (event->actor.get() == RE::PlayerCharacter::GetSingleton() && event->equipped)
		{
			if (auto form = RE::TESForm::LookupByID(event->baseObject))
			{
				if (!menuchecker::isGameStopped())
				{
					// is equipped object an arrow
					if (form->IsAmmo())
					{
						// does player have bow equipped
						if (auto weap = RE::PlayerCharacter::GetSingleton()->GetEquippedObject(
								!g_left_hand_mode);
							weap && weap->IsWeapon() && weap->As<RE::TESObjectWEAP>()->IsBow())
						{
							// get the bow angle when no arrow is nocked
							GetBowBaseAngle(&g_unbent_bow_angle);
							_DEBUGLOG("Got unbent angle: {} {} {}", VECTOR((g_unbent_bow_angle)));

							g_arrow_held_button = vr::k_EButton_Max;

							// Only one button is chosen, ordered by increasing likelihood of accidental
							// button press
							for (auto b : kCheckButtons)
							{
								if (vrinput::GetButtonState(b, (vrinput::Hand)g_left_hand_mode,
										vrinput::ActionType::kPress) ==
									vrinput::ButtonState::kButtonDown)
								{
									g_arrow_held_button = b;
									g_arrow_held = true;

									_DEBUGLOG("arrow equipped with button press: {}",
										g_arrow_held_button);
									return;
								}
							}
						}
					}
				}
			}
		}
	}

	bool OnButtonEvent(const vrinput::ModInputEvent& e)
	{
		static std::chrono::steady_clock::time_point last_arrow_hold = {};

		if (e.button_ID == g_arrow_held_button)
		{
			_DEBUGLOG("arrow button {} event {}",
				e.button_state == vrinput::ButtonState::kButtonUp ? "release" : "press",
				e.button_ID);

			if (e.button_state == vrinput::ButtonState::kButtonUp)
			{
				vrinput::ClearAllFake();

				g_arrow_held = false;

				if (IsArrowNocked())
				{
					// Arrow will be shot, stop watching the buttons
					g_arrow_held_button = vr::EVRButtonId::k_EButton_Max;
				}
				else
				{  // Arrow hasn't been nocked yet, but we were holding an arrow and a button,
					// so start the grace period

					last_arrow_hold = std::chrono::steady_clock::now();
				}
			}
			else if (g_arrow_held == false)
			{  // Check if we're still in the grace period
				auto ms_since_release = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - last_arrow_hold)
											.count();

				if (ms_since_release < g_grace_period_ms)
				{
					_DEBUGLOG("Arrow button holding resumed after {} ms", ms_since_release);
					g_arrow_held = true;
				}
			}
		}
		return false;
	}

	void OnMenuOpenClose(RE::MenuOpenCloseEvent const* evn)
	{
		if (!evn->opening && std::strcmp(evn->menuName.data(), "Journal Menu") == 0)
		{
			if (ReadConfig(g_ini_path))
			{
				// re-register buttons in case left handed mode changed
				UnregisterButtons();
				RegisterButtons();
				g_unbent_bow_angle = RE::NiPoint3();
			}
		}
	}

	/* Get the angle between the bow and the hand, normally fixed but any change indicates arrow is in place */
	void GetBowBaseAngle(RE::NiPoint3* out)
	{
		if (auto pc = RE::PlayerCharacter::GetSingleton(); pc && pc->Get3D(g_vrik_disabled))
		{
			auto bow = pc->Get3D(g_vrik_disabled)->GetObjectByName("SHIELD")->world;
			auto hand =
				vrinput::GetHandNode((vrinput::Hand)!g_left_hand_mode, g_vrik_disabled)->world;

			auto rotdiff = bow.rotate.Transpose() * hand.rotate;
			rotdiff.ToEulerAnglesXYZ(*out);
		}
	}

	/* Checks if the current hand-bow angle is different from the base */
	bool IsArrowNocked()
	{
		if (auto pc = RE::PlayerCharacter::GetSingleton(); pc && pc->Get3D(g_vrik_disabled))
		{
			RE::NiPoint3 ang;
			GetBowBaseAngle(&ang);

			ang = g_unbent_bow_angle - ang;

			auto norm = std::sqrt(ang.x * ang.x + ang.y * ang.y + ang.z * ang.z);
			_DEBUGLOG("{}", norm);
			return norm > g_angle_diff_threshold;
		}
		return false;
	}

	void TryNockArrow()
	{
		static bool fake_button_down = false;

		if (fake_button_down)
		{  // reset button so we can try again in a few frames
			if (g_arrow_held_button != vr::k_EButton_SteamVR_Trigger)
			{
				vrinput::ClearFakeButtonState({
					.device = g_left_hand_mode ? vrinput::Hand::kLeft : vrinput::Hand::kRight,
					.touch_or_press = vrinput::ActionType::kPress,
					.button_state = vrinput::ButtonState::kButtonDown,
					.button_ID = vr::k_EButton_SteamVR_Trigger,
				});
				vrinput::ClearFakeButtonState({
					.device = g_left_hand_mode ? vrinput::Hand::kLeft : vrinput::Hand::kRight,
					.touch_or_press = vrinput::ActionType::kTouch,
					.button_state = vrinput::ButtonState::kButtonDown,
					.button_ID = vr::k_EButton_SteamVR_Trigger,
				});
			}
			fake_button_down = false;
		}
		else
		{  // start spoofing
			if (g_arrow_held_button == vr::k_EButton_SteamVR_Trigger)
			{
				vrinput::SendFakeInputEvent(
					{ .device = g_left_hand_mode ? vrinput::Hand::kLeft : vrinput::Hand::kRight,
						.touch_or_press = vrinput::ActionType::kPress,
						.button_state = vrinput::ButtonState::kButtonUp,
						.button_ID = vr::k_EButton_SteamVR_Trigger });
			}
			else
			{
				vrinput::SetFakeButtonState({
					.device = g_left_hand_mode ? vrinput::Hand::kLeft : vrinput::Hand::kRight,
					.touch_or_press = vrinput::ActionType::kPress,
					.button_state = vrinput::ButtonState::kButtonDown,
					.button_ID = vr::k_EButton_SteamVR_Trigger,
				});
				vrinput::SetFakeButtonState({
					.device = g_left_hand_mode ? vrinput::Hand::kLeft : vrinput::Hand::kRight,
					.touch_or_press = vrinput::ActionType::kTouch,
					.button_state = vrinput::ButtonState::kButtonDown,
					.button_ID = vr::k_EButton_SteamVR_Trigger,
				});
			}
			fake_button_down = true;
		}
	}

	void RegisterButtons()
	{
		for (auto b : kCheckButtons)
		{
			vrinput::AddCallback(
				OnButtonEvent, b, (vrinput::Hand)g_left_hand_mode, vrinput::ActionType::kPress);
		}
	}

	void UnregisterButtons()
	{
		for (auto b : kCheckButtons)
		{
			vrinput::RemoveCallback(
				OnButtonEvent, b, (vrinput::Hand)g_left_hand_mode, vrinput::ActionType::kPress);
		}
	}

	void RegisterVRInputCallback()
	{
		auto OVRHookManager = g_papyrusvr->GetOpenVRHook();
		if (OVRHookManager && OVRHookManager->IsInitialized())
		{
			OVRHookManager = RequestOpenVRHookManagerObject();
			if (OVRHookManager)
			{
				SKSE::log::info("Successfully requested OpenVRHookManagerAPI.");

				vrinput::InitControllerHooks();

				vrinput::g_leftcontroller =
					OVRHookManager->GetVRSystem()->GetTrackedDeviceIndexForControllerRole(
						vr::TrackedControllerRole_LeftHand);
				vrinput::g_rightcontroller =
					OVRHookManager->GetVRSystem()->GetTrackedDeviceIndexForControllerRole(
						vr::TrackedControllerRole_RightHand);

				OVRHookManager->RegisterControllerStateCB(vrinput::ControllerInputCallback);
				OVRHookManager->RegisterGetPosesCB(vrinput::ControllerPoseCallback);
			}
		}
		else { SKSE::log::trace("Failed to initialize OVRHookManager"); }
	}

	bool ReadConfig(const char* a_ini_path)
	{
		using namespace std::filesystem;
		static std::filesystem::file_time_type last_read = {};

		auto config_path = helper::GetGamePath() / a_ini_path;

		try
		{
			auto last_write = last_write_time(config_path);

			if (last_write > last_read)
			{
				std::ifstream config(config_path);
				if (config.is_open())
				{
					g_firebutton = (vr::EVRButtonId)helper::ReadIntFromIni(config, "FireButtonID");

					auto ArrowDistanceToNock =
						helper::ReadFloatFromIni(config, "fArrowDistanceToNock");
					g_overlap_radius = std::powf(
						(ArrowDistanceToNock / kArrowDistanceToNockDefault) * kOverlapRadiusDefault,
						2);

					g_debug_print = helper::ReadIntFromIni(config, "Debug");
					g_grace_period_ms = helper::ReadIntFromIni(config, "iGracePeriod");
					g_left_hand_mode = helper::ReadIntFromIni(config, "iLeftHandedMode");
					g_frames_between_attempts =
						helper::ReadIntFromIni(config, "iFramesBetweenNockAttempts");

					_DEBUGLOG("g_overlap_radius : {}", g_overlap_radius);

					config.close();
					last_read = last_write_time(config_path);
					return true;
				}
				else
				{
					SKSE::log::error("error opening ini");
					last_read = file_time_type{};
				}
			}
			else { _DEBUGLOG("ini not read (no changes)"); }
		} catch (const filesystem_error&)
		{
			SKSE::log::error("ini not found, using defaults");
			last_read = file_time_type{};
		}
		return false;
	}
}
