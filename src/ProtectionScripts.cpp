// Moonlit Blessings: C++ spell scripts for the Protection (Paladin tank) Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/protection.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_protection_radiant_rebuke      Boon: a block sends holy damage (25% of the block value) back at the attacker
//   spell_blessings_protection_righteous_zeal      bound to the three stock Judgement casts: a Judgement heals a Paladin who has Righteous Zeal for 2% of maximum health
//   spell_blessings_protection_thorned_aura        Boon: every 2 sec puts a hidden link on group members who hold the Paladin's Retribution Aura
//   spell_blessings_protection_thorned_link        the link: a melee hit on its holder adds the Retribution Aura's strike to the Paladin's threat on the attacker
//   spell_blessings_protection_citadel_of_light    Boon: a block adds a damage-reduction stack, the next hit that is not blocked clears them
//   spell_blessings_protection_mantle_of_the_aegis Boon: every 6 sec an absorb for the Paladin and the group members who hold one of his auras
//   spell_blessings_protection_warding_aura        Favor: every 2 sec a 5% damage-reduction buff for the Paladin and the group members who hold one of his auras
//
// A passive's helper spell id (the bolt, buff or link a script casts) is read from the TriggerSpell column of one of its effects, never hard-coded; the one exception is
// Righteous Zeal (the script lives on stock spells and cannot name it), asserted in the recipe. ~Moonlit Nights Team

#include "BlessingsHelpers.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"

namespace
{
    // The id of the Righteous Zeal passive (tools/talents/recipes/protection.py asserts it).
    constexpr uint32 SPELL_RIGHTEOUS_ZEAL = 1520010;

    constexpr int32 RADIANT_REBUKE_PCT_OF_BLOCK = 25;
    constexpr int32 MANTLE_PCT_OF_MAX_HEALTH = 3;

    // The Paladin, then every other player of his group who is in the world, alive and on his map. (A Paladin who is not a player is only himself.)
    template <typename Callback>
    void ForEachAlly(Unit* paladin, Callback callback)
    {
        callback(paladin);

        Player* player = paladin->ToPlayer();
        Group* group = player ? player->GetGroup() : nullptr;
        if (!group)
            return;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != player && member->IsInWorld() && member->IsAlive() && member->IsInMap(player))
                callback(member);
        }
    }

    // The Paladin aura (Devotion, Retribution, Concentration, a Resistance aura, the Crusader Aura ...) that `paladin` cast and `ally` holds, or null. An aura is a Paladin
    // spell with an area-aura effect. With `retributionOnly` only an aura that has a damage shield (Retribution Aura) counts.
    Aura const* FindAuraOf(Unit const* paladin, Unit const* ally, bool retributionOnly)
    {
        for (auto const& pair : ally->GetAppliedAuras())
        {
            Aura const* aura = pair.second->GetBase();
            if (aura->GetCasterGUID() != paladin->GetGUID())
                continue;

            SpellInfo const* info = aura->GetSpellInfo();
            if (info->SpellFamilyName != SPELLFAMILY_PALADIN || !info->HasAreaAuraEffect())
                continue;

            if (retributionOnly && !info->HasAura(SPELL_AURA_DAMAGE_SHIELD))
                continue;

            return aura;
        }

        return nullptr;
    }
}

// Radiant Rebuke: effect 0 is a trigger-spell proc on blocks; the default cast (a bolt of no size) is replaced by a bolt of 25% of the block value.
class spell_blessings_protection_radiant_rebuke : public AuraScript
{
    PrepareAuraScript(spell_blessings_protection_radiant_rebuke);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        Unit* paladin = GetTarget();
        Unit* attacker = eventInfo.GetActor();
        if (!attacker || attacker == paladin || !attacker->IsAlive())
            return;

        int32 damage = int32(CalculatePct(paladin->GetShieldBlockValue(), RADIANT_REBUKE_PCT_OF_BLOCK));
        if (damage <= 0)
            return;

        paladin->CastCustomSpell(attacker, GetSpellInfo()->Effects[EFFECT_0].TriggerSpell, &damage, nullptr, nullptr, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_protection_radiant_rebuke::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// Righteous Zeal: bound to Judgement of Light, of Wisdom and of Justice. Effect 1 of the passive names the percentage heal.
class spell_blessings_protection_righteous_zeal : public SpellScript
{
    PrepareSpellScript(spell_blessings_protection_righteous_zeal);

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->IsAlive())
            return;

        AuraEffect const* zeal = caster->GetAuraEffect(SPELL_RIGHTEOUS_ZEAL, EFFECT_1);
        if (!zeal)
            return;

        if (uint32 const heal = zeal->GetSpellInfo()->Effects[EFFECT_1].TriggerSpell)
            caster->CastSpell(caster, heal, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_blessings_protection_righteous_zeal::HandleAfterCast);
    }
};

// Thorned Aura: effect 1 ticks every 2 sec, effect 2 names the link. Group members under the Retribution Aura get the link, others lose it.
class spell_blessings_protection_thorned_aura : public AuraScript
{
    PrepareAuraScript(spell_blessings_protection_thorned_aura);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* paladin = GetTarget();
        if (!paladin->IsAlive())
            return;

        uint32 const link = GetSpellInfo()->Effects[EFFECT_2].TriggerSpell;
        ForEachAlly(paladin, [&](Unit* ally)
        {
            if (ally == paladin)    // a Retribution Aura on the Paladin himself already credits him with the threat
                return;

            bool const under = FindAuraOf(paladin, ally, true) != nullptr;
            bool const linked = ally->HasAura(link, paladin->GetGUID());
            if (under && !linked)
                paladin->CastSpell(ally, link, true);
            else if (!under && linked)
                ally->RemoveAurasDueToSpell(link, paladin->GetGUID());
        });
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_protection_thorned_aura::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// The Thorned Aura link (on an ally, cast by the Paladin): effect 0 names the Boon. A melee hit on the ally adds the damage the Retribution Aura strikes back with to the
// Paladin's threat on the attacker. Everything is re-checked here, so a link that has outlived the Boon, the aura or the group does nothing.
class spell_blessings_protection_thorned_link : public AuraScript
{
    PrepareAuraScript(spell_blessings_protection_thorned_link);

    void HandleProc(ProcEventInfo& eventInfo)
    {
        Unit* ally = GetTarget();
        Unit* paladin = GetCaster();
        Unit* enemy = eventInfo.GetActor();
        if (!paladin || paladin == ally || !paladin->IsAlive() || !enemy || enemy == ally || enemy->IsPlayer() || !enemy->IsAlive())
            return;

        if (!paladin->HasAura(GetSpellInfo()->Effects[EFFECT_0].TriggerSpell))
            return;

        Aura const* aura = FindAuraOf(paladin, ally, true);
        if (!aura)
            return;

        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            AuraEffect const* shield = aura->GetEffect(i);
            if (!shield || shield->GetAuraType() != SPELL_AURA_DAMAGE_SHIELD || shield->GetAmount() <= 0)
                continue;

            // what the strike will do: the core adds the Paladin's damage bonuses to the stored amount once more at each strike
            uint32 const damage = paladin->SpellDamageBonusDone(enemy, aura->GetSpellInfo(), uint32(shield->GetAmount()), SPELL_DIRECT_DAMAGE, shield->GetEffIndex());
            enemy->AddThreat(paladin, float(damage), SPELL_SCHOOL_MASK_HOLY, aura->GetSpellInfo());
            return;
        }
    }

    void Register() override
    {
        OnProc += AuraProcFn(spell_blessings_protection_thorned_link::HandleProc);
    }
};

// Citadel of Light: effect 1 names the stacking damage-reduction buff. The proc row delivers every blockable hit taken, blocked or not.
class spell_blessings_protection_citadel_of_light : public AuraScript
{
    PrepareAuraScript(spell_blessings_protection_citadel_of_light);

    void HandleProc(ProcEventInfo& eventInfo)
    {
        Unit* paladin = GetTarget();
        uint32 const buff = GetSpellInfo()->Effects[EFFECT_1].TriggerSpell;
        if (eventInfo.GetHitMask() & (PROC_HIT_BLOCK | PROC_HIT_FULL_BLOCK))
            paladin->CastSpell(paladin, buff, true);        // one more stack (up to 3), 10 sec from now
        else
            paladin->RemoveAurasDueToSpell(buff);           // this hit has already been reduced by the stacks: they are spent
    }

    void Register() override
    {
        OnProc += AuraProcFn(spell_blessings_protection_citadel_of_light::HandleProc);
    }
};

// Mantle of the Aegis: effect 0 ticks every 6 sec, effect 1 names the absorb.
class spell_blessings_protection_mantle_of_the_aegis : public AuraScript
{
    PrepareAuraScript(spell_blessings_protection_mantle_of_the_aegis);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* paladin = GetTarget();
        if (!paladin->IsAlive())
            return;

        int32 absorb = int32(paladin->CountPctFromMaxHealth(MANTLE_PCT_OF_MAX_HEALTH));
        if (absorb <= 0)
            return;

        uint32 const shield = GetSpellInfo()->Effects[EFFECT_1].TriggerSpell;
        ForEachAlly(paladin, [&](Unit* ally)
        {
            if (!FindAuraOf(paladin, ally, false))
                return;

            ally->RemoveAurasDueToSpell(shield, paladin->GetGUID());    // renewed to full, not added to what is left
            paladin->CastCustomSpell(ally, shield, &absorb, nullptr, nullptr, true);
        });
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_protection_mantle_of_the_aegis::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Favor of the Warding Aura: effect 0 ticks every 2 sec, effect 1 names the 4 sec buff, which each tick renews on everyone under one of the Paladin's auras.
class spell_blessings_protection_warding_aura : public AuraScript
{
    PrepareAuraScript(spell_blessings_protection_warding_aura);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* paladin = GetTarget();
        if (!paladin->IsAlive())
            return;

        uint32 const buff = GetSpellInfo()->Effects[EFFECT_1].TriggerSpell;
        ForEachAlly(paladin, [&](Unit* ally)
        {
            if (FindAuraOf(paladin, ally, false))
                paladin->CastSpell(ally, buff, true);
        });
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_protection_warding_aura::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

void AddMoonlitBlessingsProtectionScripts()
{
    RegisterSpellScript(spell_blessings_protection_radiant_rebuke);
    RegisterSpellScript(spell_blessings_protection_righteous_zeal);
    RegisterSpellScript(spell_blessings_protection_thorned_aura);
    RegisterSpellScript(spell_blessings_protection_thorned_link);
    RegisterSpellScript(spell_blessings_protection_citadel_of_light);
    RegisterSpellScript(spell_blessings_protection_mantle_of_the_aegis);
    RegisterSpellScript(spell_blessings_protection_warding_aura);
}
