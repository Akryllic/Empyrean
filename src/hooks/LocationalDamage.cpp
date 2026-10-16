#include "LocationalDamage.h"
#include "Settings/INI/INISettings.h"

#undef PlaySound

namespace
{
	thread_local RE::BGSImpactData* s_impactOverride = nullptr;

	constexpr std::string_view PLUGIN_FILE = "PathsOfEmpyrean.esp"sv;
	constexpr std::string_view SKYRIM_FILE = "Skyrim.esm"sv;

	bool Contains(std::string_view a_text, std::initializer_list<std::string_view> a_tokens)
	{
		return std::ranges::any_of(a_tokens, [&](std::string_view a_token) { return a_text.find(a_token) != std::string_view::npos; });
	}

	template <class T>
	T* LookupForm(RE::FormID a_localFormID, std::string_view a_file)
	{
		const auto dataHandler = RE::TESDataHandler::GetSingleton();
		auto* form = dataHandler ? dataHandler->LookupForm<T>(a_localFormID, a_file) : nullptr;
		if (!form) {
			logger::error("    > Could not find form {:06X} in {}"sv, a_localFormID, a_file);
		}
		return form;
	}
}

namespace Hooks
{
	bool LocationalDamage::InstallHooks()
	{
		logger::info("  Installing LocationalDamage Hooks..."sv);

		if (!Settings::INI::GetSetting<bool>(Settings::INI::ENABLE_LOCATIONAL_DAMAGE).value_or(true)) {
			logger::info("    > Locational damage is disabled in the INI, skipping"sv);
			return true;
		}

		if (REL::Module::IsVR()) {
			logger::info("    > Locational damage is not available on VR, skipping"sv);
			return true;
		}

		if (ArcheryLocationalDamageProvidesRules()) {
			logger::warn("    > Archery Locational Damage is loaded with the Paths of Empyrean rules in its INI, leaving locational damage to it"sv);
			return true;
		}

		const std::ptrdiff_t impactOffset = REL::Module::IsSE() ? 0x3E3 : REL::Module::get().version() < REL::Version(1, 7, 0, 0) ? 0x3D4 : 0x3EC;
		REL::Relocation<std::uintptr_t> impactSite{ RELOCATION_ID(43013, 44204), impactOffset };
		const bool impactSiteHooked = REL::make_pattern<"FF 15">().match(impactSite.address());
		if (!impactSiteHooked && !REL::make_pattern<"FF 90 C0 05 00 00">().match(impactSite.address())) {
			logger::error("    > Projectile impact hook did not find the expected call at {:#x}, skipping"sv, impactSite.address());
			return true;
		}

		REL::Relocation<std::uintptr_t> hitTaskSite{ RELOCATION_ID(36016, 36991), REL::VariantOffset(0x926, 0xA3D, 0x0) };
		if (!IsExpectedCall(hitTaskSite.address(), RELOCATION_ID(37633, 38586).address())) {
			logger::error("    > Hit task hook did not find the expected call at {:#x}, skipping"sv, hitTaskSite.address());
			return true;
		}

		auto& trampoline = SKSE::GetTrampoline();
		const auto previousImpactHook = trampoline.write_call<6>(impactSite.address(), &KillOnCollision);
		if (impactSiteHooked) {
			_KillOnCollision = *reinterpret_cast<std::uintptr_t*>(previousImpactHook);
			logger::info("    > Chained onto an existing hook at {:#x}"sv, impactSite.address());
		}
		_HandleHitTask = trampoline.write_call<5>(hitTaskSite.address(), &HandleHitTask);
		InstallImpactDataHook();

		hooksInstalled = true;
		logger::info("    > Installed hooks for locational damage"sv);
		return true;
	}

	void LocationalDamage::LoadData()
	{
		if (!hooksInstalled) return;

		showMessages = Settings::INI::GetSetting<bool>(Settings::INI::SHOW_LOCATIONAL_DAMAGE_MESSAGES).value_or(true);
		headshotMultRank1 = Settings::INI::GetSetting<float>(Settings::INI::LOCATIONAL_DAMAGE_HEADSHOT_MULT_RANK1).value_or(1.30f);
		headshotMultRank2 = Settings::INI::GetSetting<float>(Settings::INI::LOCATIONAL_DAMAGE_HEADSHOT_MULT_RANK2).value_or(1.15f);

		keywordDeflectArrowsActive = LookupForm<RE::BGSKeyword>(0x5805F3, PLUGIN_FILE);
		keywordPhalanxActive = LookupForm<RE::BGSKeyword>(0x1FE445, PLUGIN_FILE);
		keywordWeapTypeCrossbow = LookupForm<RE::BGSKeyword>(0x02E13C, PLUGIN_FILE);
		keywordVendorItemArrow = LookupForm<RE::BGSKeyword>(0x0917E7, SKYRIM_FILE);
		keywordsLowTierMaterial = {
			LookupForm<RE::BGSKeyword>(0x01E717, SKYRIM_FILE),
			LookupForm<RE::BGSKeyword>(0x01E718, SKYRIM_FILE),
			LookupForm<RE::BGSKeyword>(0x0C5C03, SKYRIM_FILE),
			LookupForm<RE::BGSKeyword>(0x00AA12, PLUGIN_FILE)
		};
		conditionPerks = {
			LookupForm<RE::BGSPerk>(0x193CFE, PLUGIN_FILE),
			LookupForm<RE::BGSPerk>(0x193CFF, PLUGIN_FILE)
		};

		const auto impactArrowVsMetal = LookupForm<RE::BGSImpactData>(0x04BB54, SKYRIM_FILE);
		const auto impactArrowBounce = LookupForm<RE::BGSImpactData>(0x0193B3, SKYRIM_FILE);

		const std::array<std::array<RE::SpellItem*, kRankCount>, kZoneCount> spells{ {
			{ LookupForm<RE::SpellItem>(0x1849DB, PLUGIN_FILE), LookupForm<RE::SpellItem>(0x193CFC, PLUGIN_FILE) },
			{ LookupForm<RE::SpellItem>(0x1849E4, PLUGIN_FILE), LookupForm<RE::SpellItem>(0x193CFA, PLUGIN_FILE) },
			{ LookupForm<RE::SpellItem>(0x1849DE, PLUGIN_FILE), LookupForm<RE::SpellItem>(0x193CF8, PLUGIN_FILE) }
		} };

		const bool foundAll = keywordDeflectArrowsActive && keywordPhalanxActive && keywordWeapTypeCrossbow && keywordVendorItemArrow &&
		                      std::ranges::all_of(keywordsLowTierMaterial, [](auto* a_form) { return a_form != nullptr; }) &&
		                      std::ranges::all_of(conditionPerks, [](auto* a_form) { return a_form != nullptr; }) &&
		                      impactArrowVsMetal && impactArrowBounce &&
		                      std::ranges::all_of(spells, [](const auto& a_ranks) { return std::ranges::all_of(a_ranks, [](auto* a_form) { return a_form != nullptr; }); });
		if (!foundAll) {
			logger::error("  >Locational damage is disabled because some of its forms are missing"sv);
			return;
		}

		deflectRule = { .damageMult = 0.0f, .deflect = true, .impactData = impactArrowVsMetal, .sound = "WPNImpactArrowVsShieldHeavySD" };
		phalanxRule = { .damageMult = 0.0f, .deflect = true, .impactData = impactArrowBounce, .sound = "WPNImpactArrowBounceSD" };

		const std::array<std::pair<const char*, const char*>, kZoneCount> messages{ {
			{ "$PoEHeadshot", "Headshot!" },
			{ "$PoEArmInjury", "Arm injury inflicted!" },
			{ "$PoELegInjury", "Leg injury inflicted!" }
		} };

		for (std::size_t zone = 0; zone < kZoneCount; ++zone) {
			for (std::size_t rank = 0; rank < kRankCount; ++rank) {
				auto& rule = crossbowRules[zone][rank];
				rule.damageMult = zone != kHead ? 1.0f : rank == kRank1 ? headshotMultRank1 : headshotMultRank2;
				rule.sound = zone == kHead ? "UISneakAttack" : nullptr;
				rule.messageKey = messages[zone].first;
				rule.messageFallback = messages[zone].second;
				rule.spell = spells[zone][rank];
				rule.castFromTarget = CastsFromTarget(rule.spell);
			}
		}

		active = true;
	}

	bool LocationalDamage::ArcheryLocationalDamageProvidesRules()
	{
		if (!REX::W32::GetModuleHandleW(L"ArcheryLocationalDamage.dll")) return false;

		std::ifstream file(R"(Data\SKSE\Plugins\ArcheryLocationalDamage.ini)");
		const std::string text{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
		if (text.find("PoE_") != std::string::npos) return true;

		logger::warn("    > Archery Locational Damage is loaded, its own rules will apply on top of these"sv);
		return false;
	}

	bool LocationalDamage::IsExpectedCall(std::uintptr_t a_site, std::uintptr_t a_callee)
	{
		if (!util::is_call_site(a_site)) return false;

		const auto target = a_site + 5 + *reinterpret_cast<const std::int32_t*>(a_site + 1);
		if (target == a_callee) return true;

		const auto text = REL::Module::get().segment(REL::Segment::textx);
		if (target >= text.address() && target < text.address() + text.size()) return false;

		logger::info("    > Chained onto an existing hook at {:#x}"sv, a_site);
		return true;
	}

	void LocationalDamage::InstallImpactDataHook()
	{
		REL::Relocation<std::uintptr_t> site{ RELOCATION_ID(36682, 37690), REL::VariantOffset(0x354, 0x3AF, 0x0) };
		if (!IsExpectedCall(site.address(), RELOCATION_ID(20408, 20860).address())) {
			logger::warn("    > Impact effect hook did not find the expected call at {:#x}, deflected arrows will keep their normal impact effects"sv, site.address());
			return;
		}
		_GetImpactData = SKSE::GetTrampoline().write_call<5>(site.address(), &GetImpactData);
	}

	bool LocationalDamage::KillOnCollision(RE::Projectile* a_projectile)
	{
		if (active) ProcessImpact(a_projectile);

		if (_KillOnCollision.address()) return _KillOnCollision(a_projectile);
		return a_projectile->GetKillOnCollision();
	}

	void LocationalDamage::HandleHitTask(RE::Actor* a_target, RE::HitData* a_hitData)
	{
		const auto hit = active && a_hitData ? TakePendingHit(*a_hitData) : std::nullopt;
		if (!hit) {
			_HandleHitTask(a_target, a_hitData);
			return;
		}

		a_hitData->totalDamage *= hit->rule->damageMult;
		s_impactOverride = hit->rule->impactData;
		_HandleHitTask(a_target, a_hitData);
		s_impactOverride = nullptr;

		ApplyEffects(*hit);
	}

	RE::BGSImpactData* LocationalDamage::GetImpactData(RE::BGSImpactDataSet* a_impactDataSet, RE::BGSMaterialType* a_material)
	{
		if (s_impactOverride) return s_impactOverride;
		return _GetImpactData(a_impactDataSet, a_material);
	}

	void LocationalDamage::ProcessImpact(RE::Projectile* a_projectile)
	{
		if (!a_projectile || a_projectile->GetFormType() != RE::FormType::ProjectileArrow) return;

		auto& runtime = a_projectile->GetProjectileRuntimeData();
		if (runtime.impacts.empty() || runtime.flags.any(RE::Projectile::Flags::kNoDamageOutsideCombat)) return;

		auto* impact = runtime.impacts.front();
		if (!impact) return;

		const auto targetRef = impact->collidee.get();
		if (!targetRef || targetRef->GetFormType() != RE::FormType::ActorCharacter || targetRef->IsDead()) return;

		auto* target = targetRef->As<RE::Actor>();
		const auto shooterRef = runtime.shooter.get();
		auto* shooter = shooterRef ? shooterRef->As<RE::Actor>() : nullptr;
		if (!target || (shooter && !shooter->CheckValidTarget(*target))) return;

		auto* node = impact->damageRootNode;
		if (!node) {
			auto* root = target->Get3D();
			auto* rootNode = root ? root->AsNode() : nullptr;
			float distance = 0.0f;
			node = rootNode ? FindClosestHitNode(rootNode, impact->desiredTargetLoc, target->IsPlayerRef(), distance) : nullptr;
		}
		if (!node) return;

		if (node->parent && node->parent->name == "SHIELD"sv) node = node->parent;

		const auto* rule = FindRule(a_projectile, target, shooter, node->name.c_str());
		if (!rule) return;

		if (rule->deflect) {
			if (auto* missile = a_projectile->As<RE::MissileProjectile>()) {
				auto& impactResult = missile->GetMissileRuntimeData().impactResult;
				if (impactResult == RE::ImpactResult::kStick) {
					impactResult = RE::ImpactResult::kBounce;
					impact->impactResult = RE::ImpactResult::kBounce;
				}
			}
		}

		PendingHit hit{ a_projectile->GetHandle(), target->GetHandle(), shooter ? shooter->GetHandle() : RE::ActorHandle{}, rule, std::chrono::steady_clock::now() };
		if (!RE::TaskQueueInterface::ShouldUseTaskQueue()) {
			ApplyEffects(hit);
			return;
		}

		if (shooter && rule->damageMult != 1.0f) {
			hit.amplified = AmplifyNewEffects(target, hit.aggressor, rule->damageMult, {});
		}

		if (runtime.weaponDamage > 0.0f) {
			std::lock_guard lock(pendingLock);
			std::erase_if(pendingHits, [&](const PendingHit& a_pending) { return hit.queued - a_pending.queued > PENDING_HIT_LIFETIME; });
			pendingHits.push_back(std::move(hit));
		}
		else {
			SKSE::GetTaskInterface()->AddTask([hit = std::move(hit)]() { ApplyEffects(hit); });
		}
	}

	const LocationalDamage::Rule* LocationalDamage::FindRule(RE::Projectile* a_projectile, RE::Actor* a_target, RE::Actor* a_shooter, std::string_view a_nodeName)
	{
		if (HasActiveEffectWithKeyword(a_target, keywordDeflectArrowsActive)) return &deflectRule;
		if (HasActiveEffectWithKeyword(a_target, keywordPhalanxActive) && IsLowTierArrow(a_projectile)) return &phalanxRule;

		if (!a_shooter || !ShotHasKeywords(a_projectile, { keywordWeapTypeCrossbow })) return nullptr;

		const auto zone = GetZone(a_nodeName);
		if (!zone) return nullptr;

		if (conditionPerks[kRank2]->perkConditions.IsTrue(a_shooter, a_target)) return &crossbowRules[*zone][kRank2];
		if (conditionPerks[kRank1]->perkConditions.IsTrue(a_shooter, a_target)) return &crossbowRules[*zone][kRank1];
		return nullptr;
	}

	std::optional<LocationalDamage::Zone> LocationalDamage::GetZone(std::string_view a_nodeName)
	{
		if (Contains(a_nodeName, { "Head"sv, "Eye"sv, "Nose"sv, "Jaw"sv, "HEAD"sv })) return kHead;
		if (Contains(a_nodeName, { "Arm"sv, "Forearm"sv, "Hand"sv })) return kArm;
		if (Contains(a_nodeName, { "Calf"sv, "Thigh"sv, "Leg"sv, "Upperleg"sv, "Lowerleg"sv })) return kLeg;
		return std::nullopt;
	}

	RE::NiNode* LocationalDamage::FindClosestHitNode(RE::NiNode* a_root, const RE::NiPoint3& a_position, bool a_isPlayer, float& a_distance)
	{
		RE::NiNode* closest = nullptr;
		a_distance = std::numeric_limits<float>::max();

		for (const auto& child : a_root->GetChildren()) {
			auto* childNode = child ? child->AsNode() : nullptr;
			if (!childNode) continue;

			float childDistance = 0.0f;
			if (auto* hitNode = FindClosestHitNode(childNode, a_position, a_isPlayer, childDistance); hitNode && childDistance < a_distance) {
				closest = hitNode;
				a_distance = childDistance;
			}
		}

		const std::string_view name{ a_root->name.c_str() };
		const bool isHitNode = a_isPlayer ? Contains(name, { "Head"sv, "Neck"sv, "Arm"sv, "Forearm"sv, "Calf"sv, "Thigh"sv }) : a_root->collisionObject && !name.empty();
		if (isHitNode) {
			const float distance = a_root->world.translate.GetSquaredDistance(a_position);
			if (distance <= a_distance) {
				closest = a_root;
				a_distance = distance;
			}
		}

		return closest;
	}

	bool LocationalDamage::HasActiveEffectWithKeyword(RE::Actor* a_actor, const RE::BGSKeyword* a_keyword)
	{
		const auto activeEffects = a_actor->AsMagicTarget()->GetActiveEffectList();
		if (!activeEffects) return false;

		for (const auto* effect : *activeEffects) {
			const auto* baseEffect = effect ? effect->GetBaseObject() : nullptr;
			if (baseEffect && effect->flags.none(RE::ActiveEffect::Flag::kInactive) && baseEffect->HasKeyword(a_keyword)) return true;
		}
		return false;
	}

	bool LocationalDamage::ShotHasKeywords(RE::Projectile* a_projectile, std::initializer_list<const RE::BGSKeyword*> a_keywords)
	{
		const auto& runtime = a_projectile->GetProjectileRuntimeData();
		const RE::BGSKeywordForm* weapon = runtime.weaponSource;
		const RE::BGSKeywordForm* ammo = runtime.ammoSource ? runtime.ammoSource->AsKeywordForm() : nullptr;
		if (!weapon || !ammo) return false;

		const auto hasAll = [&](const RE::BGSKeywordForm* a_form) {
			return std::ranges::all_of(a_keywords, [&](const RE::BGSKeyword* a_keyword) { return a_form->HasKeyword(a_keyword); });
		};
		return hasAll(weapon) || hasAll(ammo);
	}

	bool LocationalDamage::IsLowTierArrow(RE::Projectile* a_projectile)
	{
		return std::ranges::any_of(keywordsLowTierMaterial, [&](const RE::BGSKeyword* a_material) {
			return ShotHasKeywords(a_projectile, { keywordVendorItemArrow, a_material });
		});
	}

	std::optional<LocationalDamage::PendingHit> LocationalDamage::TakePendingHit(const RE::HitData& a_hitData)
	{
		std::lock_guard lock(pendingLock);
		if (pendingHits.empty()) return std::nullopt;

		const auto now = std::chrono::steady_clock::now();
		std::erase_if(pendingHits, [&](const PendingHit& a_pending) { return now - a_pending.queued > PENDING_HIT_LIFETIME; });

		const auto it = std::ranges::find_if(pendingHits, [&](const PendingHit& a_pending) {
			return a_pending.projectile == a_hitData.sourceRef && a_pending.target == a_hitData.target;
		});
		if (it == pendingHits.end()) return std::nullopt;

		const auto hit = *it;
		pendingHits.erase(it);
		return hit;
	}

	void LocationalDamage::ApplyEffects(const PendingHit& a_hit)
	{
		const auto target = a_hit.target.get();
		if (!target) return;

		const auto aggressor = a_hit.aggressor.get();
		const auto& rule = *a_hit.rule;

		if (aggressor) {
			if (rule.damageMult != 1.0f) AmplifyNewEffects(target.get(), a_hit.aggressor, rule.damageMult, a_hit.amplified);

			const bool shooterIsPlayer = aggressor->IsPlayerRef();
			if (rule.sound && (shooterIsPlayer || target->IsPlayerRef())) RE::PlaySound(rule.sound);
			if (rule.messageKey && shooterIsPlayer && showMessages) RE::SendHUDMessage::ShowHUDMessage(HitMessage(rule).c_str(), nullptr, false);
		}

		if (rule.spell) {
			auto* caster = rule.castFromTarget || !aggressor ? target.get() : aggressor.get();
			if (auto* magicCaster = caster->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant)) {
				magicCaster->CastSpellImmediate(rule.spell, false, target.get(), 1.0f, false, 0.0f, nullptr);
			}
		}
	}

	std::vector<RE::ActiveEffect*> LocationalDamage::AmplifyNewEffects(RE::Actor* a_target, RE::ActorHandle a_caster, float a_mult, const std::vector<RE::ActiveEffect*>& a_skip)
	{
		std::vector<RE::ActiveEffect*> amplified;
		const auto activeEffects = a_target->AsMagicTarget()->GetActiveEffectList();
		if (!activeEffects) return amplified;

		for (auto* effect : *activeEffects) {
			if (!effect || effect->elapsedSeconds != 0.0f || effect->caster != a_caster || std::ranges::contains(a_skip, effect)) continue;

			const auto* baseEffect = effect->GetBaseObject();
			if (!baseEffect) continue;

			if (baseEffect->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kPowerAffectsMagnitude) && effect->magnitude != 0.0f) {
				effect->magnitude *= a_mult;
			}
			else {
				effect->duration *= a_mult;
			}
			amplified.push_back(effect);
		}
		return amplified;
	}

	std::string LocationalDamage::HitMessage(const Rule& a_rule)
	{
		std::string text;
		if (!SKSE::Translation::Translate(a_rule.messageKey, text) || text.empty() || text.starts_with('$')) {
			text = a_rule.messageFallback;
		}
		return text;
	}

	bool LocationalDamage::CastsFromTarget(const RE::SpellItem* a_spell)
	{
		return std::ranges::all_of(a_spell->effects, [](const RE::Effect* a_effect) {
			return a_effect && a_effect->baseEffect && a_effect->baseEffect->GetArchetype() == RE::EffectSetting::Archetype::kPeakValueModifier;
		});
	}
}
