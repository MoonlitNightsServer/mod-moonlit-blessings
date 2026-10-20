// Moonlit Blessings: C++ spell scripts for the Warden Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/warden.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_cairn_guard           unlimited absorb: 8% less damage from enemies you are not targeting or attacking
//   spell_blessings_earthen_reprisal      block proc: Nature damage to the attacker, 15% of the block value
//   spell_blessings_avalanche             every 3 sec at 10+ Bedrock stacks: Nature damage and threat pulse around you
//   spell_blessings_grounding_bulwark     unlimited absorb: 15% of direct-damage magic hits are absorbed completely
//   spell_blessings_living_rock           every second at 15 Bedrock stacks: heal 0.5% of maximum health
//   spell_blessings_ancestral_fortitude   bound to the stock Ancestral Fortitude ranks: 50% stronger and 5 sec longer on a Warden with Ancestors' Mantle
//   spell_blessings_spirit_of_granite     unlimited absorb: a hit that leaves you below 30% health starts a 6 sec damage reduction (once per 90 sec)
//   spell_blessings_granite_skin          armour equal to 15% of the equipped shield's armour (the Stoneskin part is a spell modifier in the DBC)
//   spell_blessings_tectonic_pull         keeps bosses out of the pull
//   spell_blessings_bedrock_keystone      Heart of the Mountain and Mountain's Wrath: a buff that is on while Bedrock is at 15 stacks
//   spell_blessings_unbroken              once per 5 min a killing blow leaves you at 20% health, plus an 8 sec shield
//   spell_blessings_bedrock_slam          bound to Bedrock Slam: Favor of Thunderous Bulwark turns its damage into Nature damage
//   spell_blessings_bedrock_barrier       bound to Bedrock Barrier: Favor of Thunderous Bulwark reflects 20% of the absorbed damage as lightning
//
// A passive's helper spell id (the buff or bolt a script casts) is read from the TriggerSpell column of one of its dummy effects, never hard-coded.
// ~Moonlit Nights Team

#include "BlessingsHelpers.h"
#include "Creature.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"

namespace
{
    // Kit spells and markers (tools/talents/blessing_spells.py and the Warden recipe). Cross-check these against runtime/blessing_recipes.json when the recipe changes.
    enum WardenSpells : uint32
    {
        SPELL_BEDROCK                = 1840061,   // the stacking buff of Warden's Stance
        SPELL_ANCESTORS_MANTLE       = 1840180,   // Boon marker
        SPELL_FAVOR_THUNDEROUS       = 1840280    // Favor marker: effect 0 names the Thunderous Slam bolt, effect 1 the Thunderous Reflection bolt
    };

    constexpr uint32 BEDROCK_FULL = 15;
    constexpr uint32 AVALANCHE_STACKS = 10;
    constexpr int32 CAIRN_GUARD_PCT = 8;
    constexpr int32 EARTHEN_REPRISAL_PCT_OF_BLOCK = 15;
    constexpr float AVALANCHE_PCT_OF_ATTACK_POWER = 0.15f;
    constexpr int32 GROUNDING_BULWARK_CHANCE = 15;
    constexpr int32 SPIRIT_OF_GRANITE_HEALTH_PCT = 30;
    constexpr int32 SPIRIT_OF_GRANITE_REDUCTION_PCT = 25;
    constexpr uint32 SPIRIT_OF_GRANITE_COOLDOWN_MS = 90 * IN_MILLISECONDS;
    constexpr int32 ANCESTRAL_FORTITUDE_BONUS_PCT = 50;
    constexpr int32 ANCESTRAL_FORTITUDE_EXTRA_MS = 5 * IN_MILLISECONDS;
    constexpr int32 GRANITE_SKIN_PCT_OF_SHIELD = 15;
    constexpr int32 UNBROKEN_HEALTH_PCT = 20;
    constexpr int32 UNBROKEN_SHIELD_PCT_OF_MAX_HEALTH = 20;
    constexpr uint32 UNBROKEN_COOLDOWN_MS = 5 * MINUTE * IN_MILLISECONDS;
    constexpr int32 THUNDEROUS_REFLECT_PCT = 20;

    uint32 BedrockStacks(Unit const* unit)
    {
        Aura const* bedrock = unit->GetAura(SPELL_BEDROCK);
        return bedrock ? bedrock->GetStackAmount() : 0;
    }

    // The enemy the Warden is targeting or fighting.
    bool IsCurrentFocus(Unit* warden, Unit const* enemy)
    {
        if (warden->GetVictim() == enemy)
            return true;

        Player const* player = warden->ToPlayer();
        return player && player->GetSelectedUnit() == enemy;
    }

    // The armour value of the shield in the off hand (0 when none, or when it is broken).
    int32 ShieldArmour(Player const* player)
    {
        Item const* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        if (!item || item->IsBroken())
            return 0;

        ItemTemplate const* proto = item->GetTemplate();
        if (proto->Class != ITEM_CLASS_ARMOR || proto->SubClass != ITEM_SUBCLASS_ARMOR_SHIELD)
            return 0;

        return int32(proto->Armor);
    }

    // An unlimited absorb aura only exists to run a script: the amount is -1 (the core hands the decision to the script's absorbAmount).
    void MakeUnlimited(int32& amount, bool& canBeRecalculated)
    {
        amount = -1;
        canBeRecalculated = false;
    }
}

// Cairn Guard: effect 0 is an unlimited absorb.
class spell_blessings_cairn_guard : public AuraScript
{
    PrepareAuraScript(spell_blessings_cairn_guard);

    void CalculateAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        MakeUnlimited(amount, canBeRecalculated);
    }

    void Absorb(AuraEffect* /*aurEff*/, DamageInfo& dmgInfo, uint32& absorbAmount)
    {
        Unit* warden = GetTarget();
        Unit* attacker = dmgInfo.GetAttacker();
        if (!attacker || attacker == warden || IsCurrentFocus(warden, attacker))
            return;

        absorbAmount = CalculatePct(dmgInfo.GetDamage(), CAIRN_GUARD_PCT);
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_blessings_cairn_guard::CalculateAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        OnEffectAbsorb += AuraEffectAbsorbFn(spell_blessings_cairn_guard::Absorb, EFFECT_0);
    }
};

// Earthen Reprisal: effect 0 is a dummy that names the Nature bolt; the proc row (block hit mask) decides when this runs.
class spell_blessings_earthen_reprisal : public AuraScript
{
    PrepareAuraScript(spell_blessings_earthen_reprisal);

    void HandleProc(ProcEventInfo& eventInfo)
    {
        Unit* warden = GetTarget();
        Unit* attacker = eventInfo.GetActor();
        if (!attacker || attacker == warden || !attacker->IsAlive())
            return;

        int32 damage = int32(CalculatePct(warden->GetShieldBlockValue(), EARTHEN_REPRISAL_PCT_OF_BLOCK));
        if (damage <= 0)
            return;

        warden->CastCustomSpell(attacker, GetSpellInfo()->Effects[EFFECT_0].TriggerSpell, &damage, nullptr, nullptr, true);
    }

    void Register() override
    {
        OnProc += AuraProcFn(spell_blessings_earthen_reprisal::HandleProc);
    }
};

// Avalanche: effect 0 ticks every 3 sec, effect 1 is a dummy that names the pulse spell (damage in effect 0 of it, extra threat in effect 1).
class spell_blessings_avalanche : public AuraScript
{
    PrepareAuraScript(spell_blessings_avalanche);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* warden = GetTarget();
        if (!warden->IsInCombat() || BedrockStacks(warden) < AVALANCHE_STACKS)
            return;

        int32 damage = std::max<int32>(1, int32(warden->GetTotalAttackPowerValue(BASE_ATTACK) * AVALANCHE_PCT_OF_ATTACK_POWER));
        int32 threat = damage;
        warden->CastCustomSpell(warden, GetSpellInfo()->Effects[EFFECT_1].TriggerSpell, &damage, &threat, nullptr, true);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_avalanche::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Grounding Bulwark: effect 0 is an unlimited absorb. Only direct damage from magic-class spells counts (damage over time ticks and melee are left alone).
class spell_blessings_grounding_bulwark : public AuraScript
{
    PrepareAuraScript(spell_blessings_grounding_bulwark);

    void CalculateAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        MakeUnlimited(amount, canBeRecalculated);
    }

    void Absorb(AuraEffect* /*aurEff*/, DamageInfo& dmgInfo, uint32& absorbAmount)
    {
        SpellInfo const* spell = dmgInfo.GetSpellInfo();
        if (!spell || spell->DmgClass != SPELL_DAMAGE_CLASS_MAGIC || dmgInfo.GetDamageType() != SPELL_DIRECT_DAMAGE)
            return;

        if (roll_chance_i(GROUNDING_BULWARK_CHANCE))
            absorbAmount = dmgInfo.GetDamage();
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_blessings_grounding_bulwark::CalculateAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        OnEffectAbsorb += AuraEffectAbsorbFn(spell_blessings_grounding_bulwark::Absorb, EFFECT_0);
    }
};

// Living Rock: effect 0 ticks every second.
class spell_blessings_living_rock : public AuraScript
{
    PrepareAuraScript(spell_blessings_living_rock);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* warden = GetTarget();
        if (!warden->IsAlive() || BedrockStacks(warden) < BEDROCK_FULL)
            return;

        // 0.5% of maximum health (CountPctFromMaxHealth only takes whole percents)
        BlessingsHelpers::HealUnit(warden, warden, warden->GetMaxHealth() / 200, GetSpellInfo()->Id);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_living_rock::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Ancestors' Mantle: this script is bound to the three stock ranks of Ancestral Fortitude (16177, 16236, 16237), which another Shaman's critical heal puts on the Warden.
class spell_blessings_ancestral_fortitude : public AuraScript
{
    PrepareAuraScript(spell_blessings_ancestral_fortitude);

    void CalculateAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        Unit* owner = GetUnitOwner();
        if (owner && owner->HasAura(SPELL_ANCESTORS_MANTLE))
            AddPct(amount, ANCESTRAL_FORTITUDE_BONUS_PCT);
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* owner = GetUnitOwner();
        Aura* aura = GetAura();
        // the guard keeps a re-applied effect from adding the extra time twice
        if (!owner || !owner->HasAura(SPELL_ANCESTORS_MANTLE) || aura->GetMaxDuration() <= 0 || aura->GetMaxDuration() > GetSpellInfo()->GetMaxDuration())
            return;

        aura->SetMaxDuration(aura->GetMaxDuration() + ANCESTRAL_FORTITUDE_EXTRA_MS);
        aura->SetDuration(aura->GetMaxDuration());
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_blessings_ancestral_fortitude::CalculateAmount, EFFECT_0, SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN);
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_ancestral_fortitude::HandleApply, EFFECT_0, SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN, AURA_EFFECT_HANDLE_REAL);
    }
};

// Spirit of Granite: effect 0 is an unlimited absorb, effect 1 a dummy that names the 6 sec damage-reduction buff. The cooldown is kept on the passive's own spell id.
class spell_blessings_spirit_of_granite : public AuraScript
{
    PrepareAuraScript(spell_blessings_spirit_of_granite);

    bool Load() override
    {
        return GetUnitOwner()->IsPlayer();
    }

    void CalculateAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        MakeUnlimited(amount, canBeRecalculated);
    }

    void Absorb(AuraEffect* /*aurEff*/, DamageInfo& dmgInfo, uint32& absorbAmount)
    {
        Player* warden = GetTarget()->ToPlayer();
        uint32 const damage = dmgInfo.GetDamage();
        uint32 const buff = GetSpellInfo()->Effects[EFFECT_1].TriggerSpell;
        if (!warden || !damage || warden->HasAura(buff) || warden->HasSpellCooldown(GetSpellInfo()->Id))
            return;

        // does this hit leave the Warden below the health line?
        if (!warden->HealthBelowPctDamaged(SPIRIT_OF_GRANITE_HEALTH_PCT, damage))
            return;

        warden->AddSpellCooldown(GetSpellInfo()->Id, 0, SPIRIT_OF_GRANITE_COOLDOWN_MS);
        warden->CastSpell(warden, buff, true);
        absorbAmount = CalculatePct(damage, SPIRIT_OF_GRANITE_REDUCTION_PCT);   // the buff is not in force yet for this hit, so take the share off by hand
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_blessings_spirit_of_granite::CalculateAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        OnEffectAbsorb += AuraEffectAbsorbFn(spell_blessings_spirit_of_granite::Absorb, EFFECT_0);
    }
};

// Granite Skin: effect 1 is flat armour whose amount is 15% of the shield's armour, effect 2 ticks every 2 sec to refresh it after a gear change.
class spell_blessings_granite_skin : public AuraScript
{
    PrepareAuraScript(spell_blessings_granite_skin);

    void CalculateArmour(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        canBeRecalculated = true;
        Unit* owner = GetUnitOwner();
        Player const* player = owner ? owner->ToPlayer() : nullptr;
        amount = player ? int32(CalculatePct(ShieldArmour(player), GRANITE_SKIN_PCT_OF_SHIELD)) : 0;
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        GetAura()->RecalculateAmountOfEffects();
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_blessings_granite_skin::CalculateArmour, EFFECT_1, SPELL_AURA_MOD_RESISTANCE);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_granite_skin::HandlePeriodic, EFFECT_2, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Tectonic Pull: effect 0 pulls every enemy in 15 yards; bosses are taken out of the target list.
class spell_blessings_tectonic_pull : public SpellScript
{
    PrepareSpellScript(spell_blessings_tectonic_pull);

    void FilterTargets(std::list<WorldObject*>& targets)
    {
        targets.remove_if([](WorldObject* object)
        {
            Creature const* creature = object->ToCreature();
            return creature && (creature->IsDungeonBoss() || creature->isWorldBoss() || creature->GetCreatureTemplate()->rank == CREATURE_ELITE_WORLDBOSS);
        });
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_blessings_tectonic_pull::FilterTargets, EFFECT_0, TARGET_UNIT_SRC_AREA_ENEMY);
    }
};

// Heart of the Mountain and Mountain's Wrath: effect 0 ticks twice a second, effect 1 is a dummy that names the buff that is on while Bedrock is at 15 stacks.
class spell_blessings_bedrock_keystone : public AuraScript
{
    PrepareAuraScript(spell_blessings_bedrock_keystone);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* warden = GetTarget();
        uint32 const buff = GetSpellInfo()->Effects[EFFECT_1].TriggerSpell;
        if (warden->IsAlive() && BedrockStacks(warden) >= BEDROCK_FULL)
        {
            if (!warden->HasAura(buff))
                warden->CastSpell(warden, buff, true);
        }
        else
            warden->RemoveAurasDueToSpell(buff);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(GetSpellInfo()->Effects[EFFECT_1].TriggerSpell);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_bedrock_keystone::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_bedrock_keystone::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Unbroken: effect 0 is an unlimited absorb, effect 1 a dummy that names the 8 sec shield. A killing blow is absorbed down to 20% health, as the Rogue's Cheat Death does.
class spell_blessings_unbroken : public AuraScript
{
    PrepareAuraScript(spell_blessings_unbroken);

    bool Load() override
    {
        return GetUnitOwner()->IsPlayer();
    }

    void CalculateAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        MakeUnlimited(amount, canBeRecalculated);
    }

    void Absorb(AuraEffect* /*aurEff*/, DamageInfo& dmgInfo, uint32& absorbAmount)
    {
        Player* warden = GetTarget()->ToPlayer();
        uint32 const damage = dmgInfo.GetDamage();
        if (!warden || damage < warden->GetHealth() || warden->HasSpellCooldown(GetSpellInfo()->Id))
            return;

        warden->AddSpellCooldown(GetSpellInfo()->Id, 0, UNBROKEN_COOLDOWN_MS);

        uint32 const floorHealth = warden->CountPctFromMaxHealth(UNBROKEN_HEALTH_PCT);
        if (warden->GetHealth() > floorHealth)
            absorbAmount = damage - warden->GetHealth() + floorHealth;   // the blow takes everything above the floor and no more
        else
        {
            absorbAmount = damage;                                       // already below the floor: the blow is nullified and health is lifted to it
            warden->SetHealth(floorHealth);
        }

        int32 shield = int32(warden->CountPctFromMaxHealth(UNBROKEN_SHIELD_PCT_OF_MAX_HEALTH));
        warden->CastCustomSpell(warden, GetSpellInfo()->Effects[EFFECT_1].TriggerSpell, &shield, nullptr, nullptr, true);
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_blessings_unbroken::CalculateAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        OnEffectAbsorb += AuraEffectAbsorbFn(spell_blessings_unbroken::Absorb, EFFECT_0);
    }
};

// Bedrock Slam (1840030): with Favor of Thunderous Bulwark the strike's damage is dealt as Nature damage by a helper bolt that cannot miss. The Slam itself then deals none.
class spell_blessings_bedrock_slam : public SpellScript
{
    PrepareSpellScript(spell_blessings_bedrock_slam);

    void HandleHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        Aura const* favor = caster ? caster->GetAura(SPELL_FAVOR_THUNDEROUS) : nullptr;
        int32 damage = GetHitDamage();
        if (!favor || !target || damage <= 0)
            return;

        PreventHitDamage();
        caster->CastCustomSpell(target, favor->GetSpellInfo()->Effects[EFFECT_0].TriggerSpell, &damage, nullptr, nullptr, true);
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_blessings_bedrock_slam::HandleHit);
    }
};

// Bedrock Barrier (1840040): with Favor of Thunderous Bulwark, 20% of every blow the Barrier absorbs is thrown back at the attacker as lightning.
class spell_blessings_bedrock_barrier : public AuraScript
{
    PrepareAuraScript(spell_blessings_bedrock_barrier);

    void Reflect(AuraEffect* /*aurEff*/, DamageInfo& dmgInfo, uint32& absorbAmount)
    {
        Unit* warden = GetTarget();
        Unit* attacker = dmgInfo.GetAttacker();
        Aura const* favor = warden->GetAura(SPELL_FAVOR_THUNDEROUS);
        if (!favor || !attacker || attacker == warden || !attacker->IsAlive() || !absorbAmount)
            return;

        int32 damage = int32(CalculatePct(absorbAmount, THUNDEROUS_REFLECT_PCT));
        if (damage > 0)
            warden->CastCustomSpell(attacker, favor->GetSpellInfo()->Effects[EFFECT_1].TriggerSpell, &damage, nullptr, nullptr, true);
    }

    void Register() override
    {
        AfterEffectAbsorb += AuraEffectAbsorbFn(spell_blessings_bedrock_barrier::Reflect, EFFECT_0);
    }
};

void AddMoonlitBlessingsWardenScripts()
{
    RegisterSpellScript(spell_blessings_cairn_guard);
    RegisterSpellScript(spell_blessings_earthen_reprisal);
    RegisterSpellScript(spell_blessings_avalanche);
    RegisterSpellScript(spell_blessings_grounding_bulwark);
    RegisterSpellScript(spell_blessings_living_rock);
    RegisterSpellScript(spell_blessings_ancestral_fortitude);
    RegisterSpellScript(spell_blessings_spirit_of_granite);
    RegisterSpellScript(spell_blessings_granite_skin);
    RegisterSpellScript(spell_blessings_tectonic_pull);
    RegisterSpellScript(spell_blessings_bedrock_keystone);
    RegisterSpellScript(spell_blessings_unbroken);
    RegisterSpellScript(spell_blessings_bedrock_slam);
    RegisterSpellScript(spell_blessings_bedrock_barrier);
}
