#include "main_plugin.h"

#include <chrono>

namespace arrownock
{
	enum class ArrowState
	{
		// No arrow equipped or no buttons held
		kIdle = 0,
		// An arrow has been equipped and we are waiting for it to overlap with the bow
		kArrowHeld,
		// Arrow button is held down and the arrow was brought to the bow, now we are trying to nock it
		kTryToNock,
		// Arrow was nocked successfully, just waiting for button to be released
		kArrowNocked
	};

	constexpr std::array kCheckButtons{ vr::k_EButton_SteamVR_Trigger, vr::k_EButton_A,
		vr::k_EButton_Knuckles_B, vr::k_EButton_SteamVR_Touchpad, vr::k_EButton_Grip };

	PapyrusVRAPI* g_papyrusvr;

	// user settings, documented in .ini
	bool            g_enable_nocking = true;
	bool            g_stamina_autorecover = true;
	bool            g_debug_print = false;
	vr::EVRButtonId g_firebutton = vr::EVRButtonId::k_EButton_SteamVR_Trigger;
	int             g_grace_period_ms = 500;
	float           g_stamina_threshold = 0.f;
	float           g_stamina_haptic_strength = 1.f;
	int             g_stamina_visual_idx = 2;
	std::string     g_stamina_sound_editorID;

	// settings
	bool              g_left_hand_mode = false;
	float             g_overlap_radius = 18.f;
	float             g_angle_diff_threshold = 0.005f;
	int               g_frames_between_attempts = 4;
	bool              g_vrik_disabled = true;
	RE::BSSoundHandle g_stamina_sound;

	// state
	ArrowState      g_state = ArrowState::kIdle;
	RE::NiPoint3    g_unbent_bow_angle;
	vr::EVRButtonId g_arrow_held_button = vr::EVRButtonId::k_EButton_Max;

	// resources
	constexpr std::array<RE::FormID, 2> kvisuals{ 0xabf02, 0x6b10f };
	std::vector<uint16_t> khaptic_keyframes = { 3875, 3875, 3875, 3875, 3875, 3875, 3875, 3875,
		3875, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3875, 3875, 3875, 3875, 3875, 3875, 3875,
		3875, 3875, 3750, 3625, 3500, 3250, 2875, 2375, 2000, 1625, 1250, 1000, 750, 625, 375, 375,
		250, 125, 125 };

	inline void StateTransition(ArrowState a_next_state)
	{
		if (g_enable_nocking)
		{
			_DEBUGLOG("STATE CHANGE:  {} to {}", (int)g_state, (int)a_next_state);
			g_state = a_next_state;
		}
	}

	void Init()
	{
		g_vrik_disabled = GetModuleHandleA("vrik") == NULL;
		SKSE::log::info("VRIK {} found", g_vrik_disabled ? "not" : "DLL");

		ReadConfig(g_ini_path);

		auto equip_sink = EventSink<RE::TESEquipEvent>::GetSingleton();
		equip_sink->AddCallback(OnEquipped);
		RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink(equip_sink);

		auto menu_sink = EventSink<RE::MenuOpenCloseEvent>::GetSingleton();
		menu_sink->AddCallback(OnMenuOpenClose);
		RE::UI::GetSingleton()->AddEventSink(menu_sink);

		menuchecker::begin();
		RegisterVRInputCallback();
	}

	void OnGameLoad()
	{
		_DEBUGLOG("Load Game: reset state");
		g_state = ArrowState::kIdle;
		g_arrow_held_button = vr::EVRButtonId::k_EButton_Max;
	}

	void OnMenuOpenClose(RE::MenuOpenCloseEvent const* evn)
	{
		if (!evn->opening && std::strcmp(evn->menuName.data(), "Journal Menu") == 0)
		{
			ReadConfig(g_ini_path);
		}
	}

	void OnEquipped(const RE::TESEquipEvent* event)
	{
		if (event && event->actor && event->actor.get() == RE::PlayerCharacter::GetSingleton() &&
			!menuchecker::isGameStopped())
		{
			switch (g_state)
			{
			case ArrowState::kIdle:
			case ArrowState::kArrowHeld:
				// is object an arrow
				if (auto form = RE::TESForm::LookupByID(event->baseObject);
					form && form->IsAmmo() && !form->As<RE::TESAmmo>()->IsBolt())
				{
					if (event->equipped)
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

									_DEBUGLOG("arrow equipped with button press: {}",
										g_arrow_held_button);

									StateTransition(ArrowState::kArrowHeld);
									return;
								}
							}
						}
					}
					else
					{  // arrow was unequipped, go to idle state
						StateTransition(ArrowState::kIdle);
					}
				}
				break;
			default:
				break;
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
				switch (g_state)
				{
				case ArrowState::kIdle:
					break;
				case ArrowState::kArrowHeld:
					last_arrow_hold = std::chrono::steady_clock::now();
				case ArrowState::kTryToNock:
				case ArrowState::kArrowNocked:
				default:
					vrinput::ClearAllFake();
					StateTransition(ArrowState::kIdle);
					break;
				}
			}
			else  // button down
			{
				switch (g_state)
				{
				case ArrowState::kIdle:
					{
						// Check if we're still in the grace period
						auto ms_since_release =
							std::chrono::duration_cast<std::chrono::milliseconds>(
								std::chrono::steady_clock::now() - last_arrow_hold)
								.count();

						if (ms_since_release < g_grace_period_ms)
						{
							_DEBUGLOG("Arrow button holding resumed after {} ms", ms_since_release);
							StateTransition(ArrowState::kArrowHeld);
						}
						else
						{
							// too late, stop listening to this button
							g_arrow_held_button = vr::EVRButtonId::k_EButton_Max;
						}
					}
				default:
					break;
				}
			}
		}

		// Stamina Inhibitor Feature - manual nocking
		if (e.button_ID == g_firebutton && e.button_state == vrinput::ButtonState::kButtonDown &&
			g_stamina_threshold > 0.f)
		{
			if (!TestStamina(g_stamina_threshold))
			{
				if (auto weap =
						RE::PlayerCharacter::GetSingleton()->GetEquippedObject(!g_left_hand_mode);
					weap && weap->IsWeapon() && weap->As<RE::TESObjectWEAP>()->IsBow())
				{
					if (auto ammo = RE::PlayerCharacter::GetSingleton()->GetCurrentAmmo();
						ammo && !ammo->IsBolt())
					{
						if (IsOverlapping(arrownock::g_overlap_radius * 1.05))
						{
							// Player is attemping to fire a bow with not enough stamina, block the trigger press
							PlayStaminaInhibitorFX();
							return true;
						}
					}
				}
			}
		}

		return false;
	}

	void OnUpdate()
	{
		static bool fake_button_down = false;
		static int  frame_count = 0;

		switch (g_state)
		{
		case ArrowState::kIdle:
			break;
		case ArrowState::kArrowHeld:
			{
				static bool stamina_blocked = false;
				if (IsOverlapping(g_overlap_radius * 0.95))
				{
					if (!stamina_blocked)
					{
						// Stamina Inhibitor Feature: block auto nocking
						if (g_stamina_threshold > 0.f && !TestStamina(g_stamina_threshold))
						{
							// Player is attemping to fire a bow with not enough stamina
							PlayStaminaInhibitorFX();

							// Set the flag that indicates player must move out of overlap zone to reset the stamina block
							if (!g_stamina_autorecover) { stamina_blocked = true; }
						}
						else
						{
							fake_button_down = true;
							frame_count = 0;
							TryNockArrow(true);
							StateTransition(ArrowState::kTryToNock);
						}
					}
				}
				// stamina inhibitor: unblock autonocking when player moves out of overlap zone,
				// even if stamina has not recovered we'll check it again and repeat the FX next
				// time they try
				else if (stamina_blocked) { stamina_blocked = false; }

				break;
			}
		case ArrowState::kTryToNock:
			if (IsArrowNocked()) { StateTransition(ArrowState::kArrowNocked); }
			else if (!IsOverlapping(g_overlap_radius * 0.95))
			{
				StateTransition(ArrowState::kArrowHeld);
			}
			else if (++frame_count % g_frames_between_attempts == 0)
			{
				fake_button_down ^= 1;
				TryNockArrow(fake_button_down);
			}

			break;
		default:
			break;
		}
	}

	bool IsOverlapping(float a_radius_squared)
	{
		if (auto pcvr = RE::PlayerCharacter::GetSingleton()->GetVRNodeData())
		{
			// compute overlap
			auto bow_node = pcvr->ArrowSnapNode;
			auto arrow_node = g_left_hand_mode ? pcvr->LeftWandNode : pcvr->RightWandNode;

			return (arrow_node->world.translate - bow_node->world.translate).SqrLength() <
				a_radius_squared;
		}
		return false;
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
			_DEBUGLOG("IsArrowNocked: {}", norm);
			return norm > g_angle_diff_threshold;
		}
		return false;
	}

	void TryNockArrow(bool a_start_spoof)
	{
		if (a_start_spoof)
		{
			if (g_arrow_held_button == g_firebutton)
			{
				auto isfiredown = vrinput::GetButtonState(
					g_arrow_held_button, vrinput::Hand::kRight, vrinput::ActionType::kPress);
				_DEBUGLOG("sending fake input: momentary fire button release. current state: {}",
					(bool)isfiredown)
				vrinput::SendFakeInputEvent(
					{ .device = g_left_hand_mode ? vrinput::Hand::kLeft : vrinput::Hand::kRight,
						.touch_or_press = vrinput::ActionType::kPress,
						.button_state = vrinput::ButtonState::kButtonUp,
						.button_ID = g_firebutton });
			}
			else
			{
				_DEBUGLOG("sending fake input: fire button down")
				vrinput::SetFakeButtonState({
					.device = g_left_hand_mode ? vrinput::Hand::kLeft : vrinput::Hand::kRight,
					.touch_or_press = vrinput::ActionType::kPress,
					.button_state = vrinput::ButtonState::kButtonDown,
					.button_ID = g_firebutton,
				});
				vrinput::SetFakeButtonState({
					.device = g_left_hand_mode ? vrinput::Hand::kLeft : vrinput::Hand::kRight,
					.touch_or_press = vrinput::ActionType::kTouch,
					.button_state = vrinput::ButtonState::kButtonDown,
					.button_ID = g_firebutton,
				});
			}
		}
		else
		{  // reset button so we can try again in a few frames
			if (g_arrow_held_button != g_firebutton)
			{
				_DEBUGLOG("clearing fake button states")
				vrinput::ClearAllFake();
			}
		}
	}

	void PlayStaminaInhibitorFX()
	{
		constexpr int kMinFXInterval = 1400;

		static std::chrono::steady_clock::time_point last_played = {};

		auto now = std::chrono::steady_clock::now();

		if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_played).count() >
			kMinFXInterval)
		{
			last_played = now;

			_DEBUGLOG("Bow draw blocked, stamina: {} ({}%) ",
				RE::PlayerCharacter::GetSingleton()->AsActorValueOwner()->GetActorValue(
					RE::ActorValue::kStamina),
				helper::GetAVPercent(
					RE::PlayerCharacter::GetSingleton(), RE::ActorValue::kStamina));

			auto pc = RE::PlayerCharacter::GetSingleton();
			auto node = pc->Get3D(g_vrik_disabled)->GetObjectByName("NPC L Finger10 [LF10]");

			// Controller vibration
			if (g_stamina_haptic_strength > 0.f)
			{
				_DEBUGLOG("Activating haptics");
				vrinput::Vibrate(!g_left_hand_mode, &khaptic_keyframes, g_stamina_haptic_strength);
			}
			// Sound Effect
			if (!g_stamina_sound_editorID.empty() &&
				std::strcmp(g_stamina_sound_editorID.c_str(), "none") &&
				std::strcmp(g_stamina_sound_editorID.c_str(), ""))
			{
				if (node)
				{
					if (!g_stamina_sound_editorID.empty() &&
						std::strcmp(g_stamina_sound_editorID.c_str(), "none"))
					{
						if (auto sound_success =
								helper::InitializeSound(g_stamina_sound, g_stamina_sound_editorID))
						{
							_DEBUGLOG("Playing sound '{}' : ", g_stamina_sound_editorID,
								sound_success ? "success" : "failed");
							helper::PlaySound(g_stamina_sound, 1.f,
								pc->Get3D(g_vrik_disabled)->world.translate,
								pc->Get3D(g_vrik_disabled));
						}
						else
						{
							SKSE::log::error("invalid editor ID : {}", g_stamina_sound_editorID);
						}
					}
				}
			}
			// Visual Effect
			if (g_stamina_visual_idx)
			{
				if (node)
				{
					if (auto artform = RE::TESForm::LookupByID(kvisuals[g_stamina_visual_idx - 1]);
						artform && artform->GetFormType() == RE::FormType::ArtObject)
					{
						pc->ApplyArtObject(
							artform->As<RE::BGSArtObject>(), 1, nullptr, false, false, node);
						_DEBUGLOG("Applying art object with formid: {}",
							kvisuals[g_stamina_visual_idx - 1]);
					}
				}
			}
		}
		else { _DEBUGLOG("FX rate limit"); }
	}

	/* true: player has enough stamina */
	bool TestStamina(float a_threshold)
	{
		if (a_threshold > 0.f)
		{
			float stam = 0;
			if (a_threshold < 1.f)
			{
				// compare percentages
				stam = helper::GetAVPercent(
					RE::PlayerCharacter::GetSingleton(), RE::ActorValue::kStamina);
			}
			else
			{
				// compare flat values
				stam = RE::PlayerCharacter::GetSingleton()->AsActorValueOwner()->GetActorValue(
					RE::ActorValue::kStamina);
			}
			return stam > a_threshold;
		}
		return true;
	}

	void RegisterButtons(bool isLeft)
	{
		for (auto b : kCheckButtons)
		{
			vrinput::AddCallback(
				OnButtonEvent, b, (vrinput::Hand)isLeft, vrinput::ActionType::kPress);
		}
	}

	void UnregisterButtons(bool isLeft)
	{
		for (auto b : kCheckButtons)
		{
			vrinput::RemoveCallback(
				OnButtonEvent, b, (vrinput::Hand)isLeft, vrinput::ActionType::kPress);
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

				vrinput::g_IVRSystem = OVRHookManager->GetVRSystem();

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

		UnregisterButtons(g_left_hand_mode);

		if (auto setting = RE::GetINISetting("bLeftHandedMode:VRInput"))
		{
			g_left_hand_mode = setting->GetBool();
		}

		if (auto setting = RE::GetINISetting("fArrowDistanceToNock:VRWand"))
		{
			g_overlap_radius = setting->GetFloat();
		}

		SKSE::log::info(
			"bLeftHandedMode: {}\n fArrowDistanceToNock: {}", g_left_hand_mode, g_overlap_radius);
		g_overlap_radius *= g_overlap_radius;

		RegisterButtons(g_left_hand_mode);

		try
		{
			auto last_write = last_write_time(config_path);

			if (last_write > last_read)
			{
				std::ifstream config(config_path);
				if (config.is_open())
				{
					g_enable_nocking = helper::ReadIntFromIni(config, "iEnableAutonocking");
					g_firebutton = (vr::EVRButtonId)helper::ReadIntFromIni(config, "FireButtonID");
					g_debug_print = helper::ReadIntFromIni(config, "Debug");
					g_grace_period_ms = helper::ReadIntFromIni(config, "iGracePeriod");
					g_stamina_threshold = helper::ReadFloatFromIni(config, "fStaminaThreshold");
					if (g_stamina_threshold > 0.f)
					{
						g_stamina_autorecover =
							helper::ReadIntFromIni(config, "iAutonockAfterBlocking");
						g_stamina_haptic_strength =
							helper::ReadFloatFromIni(config, "fHapticStrength");
						g_stamina_visual_idx = helper::ReadIntFromIni(config, "iVisualEffect");
						if (g_stamina_visual_idx < 0 || g_stamina_visual_idx > 2)
						{
							SKSE::log::trace("Invalid visual index, disabling");
							g_stamina_visual_idx = 0;
						}
						g_stamina_sound_editorID =
							helper::ReadStringFromIni(config, "sBlockedSound");
					}
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
