// Moonlit Blessings: C++ spell scripts for the Guardian (Druid bear tank) Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/guardian.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_guardian_layered_bark            Boon: a once-a-second check that puts a +10% armour helper on while the druid's own Thorns is on him
//   spell_guardian_ironbark_plating        Boon: a helper of +30% armour is on while health is below the threshold (effect 2, 40%)
//   spell_guardian_mending_fur_heal        Boon: bound to the Frenzied Regeneration heal (22845): the heal is 25% stronger on a druid with Mending Fur
//   spell_guardian_heart_of_the_bear       Boon (keystone): a hit that takes health from 30% or more to below it starts a 6 sec heal (once per 2 min, the proc cooldown)
//   spell_guardian_wrath_of_the_bear       Boon (keystone): a dodge in a bear form casts a free Mangle (Bear) at the attacker and gives a 6 sec damage buff
//   spell_guardian_bear_helper             Favors of the Iron Hide and the Thick Fur: a helper buff that is on only while the druid is in a bear form
//   spell_guardian_savage_defender         Favor: a dodge or block strikes the attacker with Nature damage equal to 100% of attack power and gives a 6 sec damage buff
//   spell_guardian_thick_fur_regeneration  Favor: every 3 sec in combat a 2% heal
//
// A passive's helper spell id (the buff, the bolt or the heal a script casts) is read from the TriggerSpell column of one of its dummy effects, never hard-coded. The only hard-coded ids are
// stock spells (Mangle (Bear) ranks, the Thorns family flag) and Mending Fur's own id; recipes/guardian.py asserts them all.
// ~Moonlit Nights Team

#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"

namespace
{
    // Mending Fur (recipes/guardian.py asserts the id): effect 1 holds the extra healing in percent.
    constexpr uint32 SPELL_MENDING_FUR = 1240140;

    // The five stock ranks of Mangle (Bear), best first (the recipe asserts the names).
    constexpr uint32 MANGLE_BEAR_RANKS[] = { 48564, 48563, 33987, 33986, 33878 };

    // Thorns (all Druid ranks carry this bit of SpellFamilyFlags word 0; the recipe asserts rank 1).
    constexpr uint32 THORNS_FAMILY_FLAG = 0x00000100;

    bool InBearForm(Unit const* unit)
    {
        ShapeshiftForm const form = unit->GetShapeshiftForm();
        return form == FORM_BEAR || form == FORM_DIREBEAR;
    }

    // Put a helper buff on (once) or take it off.
    void SetHelper(Unit* unit, uint32 helper, bool wanted)
    {
        if (!helper)
            return;

        if (wanted)
        {
            if (!unit->HasAura(helper))
                unit->CastSpell(unit, helper, true);
        }
        else
            unit->RemoveAurasDueToSpell(helper);
    }
}

// Layered Bark: effect 1 ticks once a second, effect 2 names the +10% armour helper. The helper is on while the druid carries a Thorns that he cast himself.
class spell_guardian_layered_bark : public AuraScript
{
    PrepareAuraScript(spell_guardian_layered_bark);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* druid = GetTarget();
        bool const wanted = druid->IsAlive() && druid->GetAuraEffect(SPELL_AURA_DAMAGE_SHIELD, SPELLFAMILY_DRUID, THORNS_FAMILY_FLAG, 0, 0, druid->GetGUID());
        SetHelper(druid, GetSpellInfo()->Effects[EFFECT_2].TriggerSpell, wanted);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(GetSpellInfo()->Effects[EFFECT_2].TriggerSpell);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_guardian_layered_bark::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_guardian_layered_bark::HandleRemove, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Ironbark Plating: effect 0 ticks twice a second, effect 1 names the armour helper, effect 2 holds the health threshold in percent.
class spell_guardian_ironbark_plating : public AuraScript
{
    PrepareAuraScript(spell_guardian_ironbark_plating);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* druid = GetTarget();
        AuraEffect const* threshold = GetAura()->GetEffect(EFFECT_2);
        bool const wanted = threshold && druid->IsAlive() && druid->GetHealth() < druid->CountPctFromMaxHealth(threshold->GetAmount());
        SetHelper(druid, GetSpellInfo()->Effects[EFFECT_1].TriggerSpell, wanted);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(GetSpellInfo()->Effects[EFFECT_1].TriggerSpell);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_guardian_ironbark_plating::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_guardian_ironbark_plating::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Mending Fur: bound to the heal that the stock Frenzied Regeneration script casts (22845) with the amount it worked out from the rage. Effect 1 of the passive holds the extra percent.
class spell_guardian_mending_fur_heal : public SpellScript
{
    PrepareSpellScript(spell_guardian_mending_fur_heal);

    void HandleHeal(SpellEffIndex /*effIndex*/)
    {
        Unit* druid = GetCaster();
        if (!druid)
            return;

        if (AuraEffect const* mending = druid->GetAuraEffect(SPELL_MENDING_FUR, EFFECT_1))
        {
            int32 heal = GetHitHeal();
            AddPct(heal, mending->GetAmount());
            SetHitHeal(heal);
        }
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_guardian_mending_fur_heal::HandleHeal, EFFECT_0, SPELL_EFFECT_HEAL);
    }
};

// Heart of the Bear: effect 0 holds the health threshold in percent, effect 1 names the heal. Only a hit that carries the druid from the threshold or above to below it counts; the 2 min proc
// cooldown of the spell_proc row starts after this check, so a hit that does not qualify does not start it.
class spell_guardian_heart_of_the_bear : public AuraScript
{
    PrepareAuraScript(spell_guardian_heart_of_the_bear);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        Unit* druid = GetTarget();
        AuraEffect const* threshold = GetAura()->GetEffect(EFFECT_0);
        if (!damageInfo || !damageInfo->GetDamage() || !threshold || !druid->IsAlive())
            return false;

        uint32 const limit = druid->CountPctFromMaxHealth(threshold->GetAmount());
        uint32 const health = druid->GetHealth();
        return health < limit && health + damageInfo->GetDamage() >= limit;
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        Unit* druid = GetTarget();
        if (uint32 const heal = GetSpellInfo()->Effects[EFFECT_1].TriggerSpell)
            druid->CastSpell(druid, heal, true);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_guardian_heart_of_the_bear::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_guardian_heart_of_the_bear::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Wrath of the Bear: effect 0 names the 6 sec damage buff. The proc row delivers dodges only (taken, phase 0). A triggered cast ignores the form rule, so the bear forms are checked here;
// the highest Mangle (Bear) rank the druid knows is cast free on the attacker (the cast starts no cooldown for a triggered spell that ignores cooldowns).
class spell_guardian_wrath_of_the_bear : public AuraScript
{
    PrepareAuraScript(spell_guardian_wrath_of_the_bear);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        Unit* druid = GetTarget();
        Player* player = druid->ToPlayer();
        if (!player || !player->IsAlive() || !InBearForm(player))
            return;

        if (uint32 const buff = GetSpellInfo()->Effects[EFFECT_0].TriggerSpell)
            player->CastSpell(player, buff, true);

        Unit* enemy = eventInfo.GetActor();
        if (!enemy || enemy == player || !enemy->IsAlive() || player->IsFriendlyTo(enemy))
            return;

        for (uint32 const rank : MANGLE_BEAR_RANKS)
        {
            if (player->HasSpell(rank))
            {
                player->CastSpell(enemy, rank, true);
                break;
            }
        }
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_guardian_wrath_of_the_bear::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Favor of the Iron Hide and Favor of the Thick Fur (their first passive): effect 0 ticks twice a second, effect 1 names a helper buff that is on only in the bear forms. The Favor aura itself
// has no shapeshift requirement (the Lua system adds it with AddAura, and the core strips a passive with a requirement for good when the druid leaves the form), so this driver does the gating.
class spell_guardian_bear_helper : public AuraScript
{
    PrepareAuraScript(spell_guardian_bear_helper);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* druid = GetTarget();
        SetHelper(druid, GetSpellInfo()->Effects[EFFECT_1].TriggerSpell, druid->IsAlive() && InBearForm(druid));
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(GetSpellInfo()->Effects[EFFECT_1].TriggerSpell);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_guardian_bear_helper::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_guardian_bear_helper::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Favor of the Savage Defender: effect 0 names the Nature bolt, effect 1 the 6 sec damage buff. The proc row delivers dodged and blocked hits only (taken, phase 0, 1 sec cooldown).
// The bolt is a plain Nature bolt whose damage is set here to the druid's attack power (100%); it carries no spell power and no threat effect: the threat is that of the damage, which
// Bear Resolve's +60% multiplies.
class spell_guardian_savage_defender : public AuraScript
{
    PrepareAuraScript(spell_guardian_savage_defender);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        Unit* druid = GetTarget();
        if (!druid->IsAlive())
            return;

        if (uint32 const buff = GetSpellInfo()->Effects[EFFECT_1].TriggerSpell)
            druid->CastSpell(druid, buff, true);

        Unit* enemy = eventInfo.GetActor();
        if (!enemy || enemy == druid || !enemy->IsAlive() || druid->IsFriendlyTo(enemy))
            return;

        int32 damage = int32(druid->GetTotalAttackPowerValue(BASE_ATTACK));
        if (damage <= 0)
            return;

        if (uint32 const bolt = GetSpellInfo()->Effects[EFFECT_0].TriggerSpell)
            druid->CastCustomSpell(enemy, bolt, &damage, nullptr, nullptr, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_guardian_savage_defender::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Favor of the Thick Fur (its second passive): effect 0 ticks every 3 sec, effect 1 names the 2% heal; cast only while the druid is in combat.
class spell_guardian_thick_fur_regeneration : public AuraScript
{
    PrepareAuraScript(spell_guardian_thick_fur_regeneration);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* druid = GetTarget();
        if (!druid->IsAlive() || !druid->IsInCombat())
            return;

        if (uint32 const heal = GetSpellInfo()->Effects[EFFECT_1].TriggerSpell)
            druid->CastSpell(druid, heal, true);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_guardian_thick_fur_regeneration::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

void AddMoonlitBlessingsGuardianScripts()
{
    RegisterSpellScript(spell_guardian_bear_helper);
    RegisterSpellScript(spell_guardian_heart_of_the_bear);
    RegisterSpellScript(spell_guardian_ironbark_plating);
    RegisterSpellScript(spell_guardian_layered_bark);
    RegisterSpellScript(spell_guardian_mending_fur_heal);
    RegisterSpellScript(spell_guardian_savage_defender);
    RegisterSpellScript(spell_guardian_thick_fur_regeneration);
    RegisterSpellScript(spell_guardian_wrath_of_the_bear);
}
