// Moonlit Blessings: C++ spell scripts for the Protection (Warrior tank) Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/warprotection.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_warprotection_immovable_bastion   Boon: a block heals 1% of maximum health, but only while Shield Block is active
//
//   spell_blessings_warprotection_bulwark_engine      Boon (keystone): a block or dodge gives 5 rage and threat on the attacker (100% of attack power)
//   spell_blessings_warprotection_gladiator           Favor of the Gladiator (role flip to damage): strips the Defensive Resolve tank baseline
//
// The helper spells (the heal, the rage) are read from the TriggerSpell column of the passive's effects. The hard-coded ids are the stock Shield Block and the Defensive Resolve passive,
// both asserted by the recipe.
// ~Moonlit Nights Team

#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"

namespace
{
    // The stock Shield Block (tools/talents/recipes/warprotection.py asserts the id).
    constexpr uint32 SPELL_SHIELD_BLOCK = 2565;
    // The Defensive Resolve passive, the tank baseline (blessing_spells.DEFENSIVE_RESOLVE; the recipe asserts the id).
    constexpr uint32 SPELL_DEFENSIVE_RESOLVE = 2040010;
}

// Immovable Bastion: effect 1 is a trigger-spell proc on blocks that casts a 1% heal; the cast only goes through while the warrior has Shield Block up.
class spell_blessings_warprotection_immovable_bastion : public AuraScript
{
    PrepareAuraScript(spell_blessings_warprotection_immovable_bastion);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        if (!GetTarget()->HasAura(SPELL_SHIELD_BLOCK))
            PreventDefaultAction();
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_warprotection_immovable_bastion::HandleProc, EFFECT_1, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// Bulwark Engine: effect 0 holds the threat in percent of the attack power, effect 1 names the rage helper. The proc row delivers blocked and dodged hits only (taken, phase 0).
class spell_blessings_warprotection_bulwark_engine : public AuraScript
{
    PrepareAuraScript(spell_blessings_warprotection_bulwark_engine);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        Unit* warrior = GetTarget();
        if (!warrior->IsAlive())
            return;

        warrior->CastSpell(warrior, GetSpellInfo()->Effects[EFFECT_1].TriggerSpell, true);

        Unit* enemy = eventInfo.GetActor();
        if (!enemy || enemy == warrior || !enemy->IsAlive() || enemy->IsPlayer())
            return;

        float const threat = float(warrior->GetTotalAttackPowerValue(BASE_ATTACK)) * float(aurEff->GetAmount()) / 100.0f;
        if (threat > 0.0f)
            enemy->AddThreat(warrior, threat, SPELL_SCHOOL_MASK_NORMAL, GetSpellInfo());
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_warprotection_bulwark_engine::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Favor of the Gladiator: while the aura is on, the Defensive Resolve passive (the tank baseline) is taken away. The Lua system lists the same spell in the Favor's removes and teaches it again
// when the Favor is dropped; this script makes the flip hold even if the spell was learned in between (a level up). Nothing is undone here when the aura ends.
class spell_blessings_warprotection_gladiator : public AuraScript
{
    PrepareAuraScript(spell_blessings_warprotection_gladiator);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* warrior = GetTarget();
        warrior->RemoveAurasDueToSpell(SPELL_DEFENSIVE_RESOLVE);
        if (Player* player = warrior->ToPlayer())
            if (player->HasSpell(SPELL_DEFENSIVE_RESOLVE))
                player->removeSpell(SPELL_DEFENSIVE_RESOLVE, SPEC_MASK_ALL, false);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_warprotection_gladiator::HandleApply, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

void AddMoonlitBlessingsWarprotectionScripts()
{
    RegisterSpellScript(spell_blessings_warprotection_bulwark_engine);
    RegisterSpellScript(spell_blessings_warprotection_gladiator);
    RegisterSpellScript(spell_blessings_warprotection_immovable_bastion);
}
