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
#include "Random.h"
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
    // Glyph of the Gladiator: the stance (tools/talents/recipes/warprotection.py, unit 2030080) and Shield Crash's teacher; the stock stances the glyph takes away.
    constexpr uint32 SPELL_GLADIATOR_STANCE = 2030080;
    constexpr uint32 SPELL_GLADIATOR_STANCE_TEACHER = 2030090;
    constexpr uint32 SPELL_BATTLE_STANCE = 2457;
    constexpr uint32 SPELL_DEFENSIVE_STANCE = 71;
    constexpr uint32 SPELL_BERSERKER_STANCE = 2458;
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
        Player* player = warrior->ToPlayer();
        if (!player)
            return;

        if (player->HasSpell(SPELL_DEFENSIVE_RESOLVE))
            player->removeSpell(SPELL_DEFENSIVE_RESOLVE, SPEC_MASK_ALL, false);

        // Glyph of the Gladiator: the three stock stances are gone (the Prime module takes them away and gives them back) and the new stance is learned at once and entered when a shield is carried
        if (!player->HasSpell(SPELL_GLADIATOR_STANCE))
            player->learnSpell(SPELL_GLADIATOR_STANCE);

        player->CastSpell(player, SPELL_GLADIATOR_STANCE, true);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* warrior = GetTarget();
        warrior->RemoveAurasDueToSpell(SPELL_GLADIATOR_STANCE);
        Player* player = warrior->ToPlayer();
        if (!player || !player->GetSession() || player->GetSession()->PlayerLogout())
            return;

        if (player->HasSpell(SPELL_GLADIATOR_STANCE))
            player->removeSpell(SPELL_GLADIATOR_STANCE, SPEC_MASK_ALL, false);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_warprotection_gladiator::HandleApply, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_warprotection_gladiator::HandleRemove, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Glyph of the Gladiator, Improved Gladiator Stance rewrite: effect 0 holds the Enrage spell; each melee hit has a chance equal to block + parry + dodge to cast it.
class spell_blessings_warprotection_improved_gladiator : public AuraScript
{
    PrepareAuraScript(spell_blessings_warprotection_improved_gladiator);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        Player* player = GetTarget()->ToPlayer();
        if (!player)
            return;

        float const chance = player->GetFloatValue(PLAYER_BLOCK_PERCENTAGE) + player->GetFloatValue(PLAYER_PARRY_PERCENTAGE) + player->GetFloatValue(PLAYER_DODGE_PERCENTAGE);
        if (roll_chance_f(chance))
            player->CastSpell(player, uint32(aurEff->GetAmount()), true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_warprotection_improved_gladiator::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Glyph of the Gladiator, Shield Specialization rewrite: a melee hit has a chance equal to the block chance to open Revenge (the state a block, dodge or parry opens).
class spell_blessings_warprotection_gladiator_revenge : public AuraScript
{
    PrepareAuraScript(spell_blessings_warprotection_gladiator_revenge);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        Player* player = GetTarget()->ToPlayer();
        if (!player || !roll_chance_f(player->GetFloatValue(PLAYER_BLOCK_PERCENTAGE)))
            return;

        player->ModifyAuraState(AURA_STATE_DEFENSE, true);
        player->StartReactiveTimer(REACTIVE_DEFENSE);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_warprotection_gladiator_revenge::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

void AddMoonlitBlessingsWarprotectionScripts()
{
    RegisterSpellScript(spell_blessings_warprotection_bulwark_engine);
    RegisterSpellScript(spell_blessings_warprotection_gladiator);
    RegisterSpellScript(spell_blessings_warprotection_gladiator_revenge);
    RegisterSpellScript(spell_blessings_warprotection_improved_gladiator);
    RegisterSpellScript(spell_blessings_warprotection_immovable_bastion);
}
