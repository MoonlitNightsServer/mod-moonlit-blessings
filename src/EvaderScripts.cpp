// Moonlit Blessings: C++ spell scripts for the Evader (Ruffian) Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/evader.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_ruffian_evasion          bound to the two stock Evasion ranks (5277, 26669): the "while Evasion is active / after it ends" Boons and the Favor of the Shadow Veil
//   spell_ruffian_perfect_counter  Boon (keystone): lets only the real Riposte through the proc of the Perfect Counter debuff
//   spell_ruffian_smoke_sweep      the kit spell Smoke Sweep: its flat threat is added WITH the threat modifiers (the core's THREAT effect ignores them)
//
// Spell ids come from the registry block 1,740,100 (Evader). The passive ids below are asserted by the recipe (unit()); the helpers are the passive id + 1 (the window while Evasion lasts, or the tail
// after it) and + 2 (the Favor's tail). Stock ids: Evasion 5277 and 26669, the Riposte class flag. ~Moonlit Nights Team

#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"

namespace
{
    // The two stock Evasion ranks (recipes/evader.py asserts their names and class mask).
    constexpr uint32 SPELL_EVASION_RANK_1 = 5277;
    constexpr uint32 SPELL_EVASION_RANK_2 = 26669;

    // Stock Riposte (14251): SpellFamilyFlags word 1, bit 0x100 (recipes/evader.py asserts it).
    constexpr uint32 RIPOSTE_FAMILY_FLAG_1 = 0x00000100;

    // A Boon or Favor that reacts to Evasion: the passive the rogue has, the helper cast while Evasion is up (0 = none) and the helper cast when it has ended (0 = none).
    struct EvasionPiece
    {
        uint32 passive;
        uint32 window;
        uint32 tail;
    };

    constexpr EvasionPiece EVASION_PIECES[] =
    {
        { 1740140, 1740141, 0       },   // Veil of Shadows: 20% less spell damage while Evasion lasts
        { 1740150, 0,       1740151 },   // Lingering Veil: 8 sec of 10% less damage and 20% more speed after it
        { 1740160, 1740161, 0       },   // Ghost Walk: immune to slows, roots and stuns while Evasion lasts
        { 1740220, 1740221, 1740222 }    // Favor of the Shadow Veil: 25% less damage while Evasion lasts and for 6 sec after it
    };

    bool HasEvasion(Unit const* unit)
    {
        return unit->HasAura(SPELL_EVASION_RANK_1) || unit->HasAura(SPELL_EVASION_RANK_2);
    }
}

// Evasion (either rank). On apply: every piece the rogue has puts its window helper up and clears a tail that is still running. On remove: the windows go; when Evasion ran out or was cancelled, and the rogue
// lives, the tails start. A rank that replaces the other one (the old aura is removed while the new one is on) changes nothing. Nothing here is a stored pointer: the target is read from the aura each time.
class spell_ruffian_evasion : public AuraScript
{
    PrepareAuraScript(spell_ruffian_evasion);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* rogue = GetTarget();
        if (!rogue->IsInWorld())
            return;     // an aura restored at login: the windows were saved with it

        for (EvasionPiece const& piece : EVASION_PIECES)
        {
            if (!rogue->HasAura(piece.passive))
                continue;

            if (piece.tail)
                rogue->RemoveAurasDueToSpell(piece.tail);
            if (piece.window)
                rogue->CastSpell(rogue, piece.window, true);
        }
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* rogue = GetTarget();
        if (!rogue->IsInWorld() || HasEvasion(rogue))
            return;     // logging out, or the other rank is on (Evasion has not ended)

        AuraApplication const* application = GetTargetApplication();
        bool const ended = application && (application->GetRemoveMode() == AURA_REMOVE_BY_EXPIRE || application->GetRemoveMode() == AURA_REMOVE_BY_CANCEL);
        for (EvasionPiece const& piece : EVASION_PIECES)
        {
            if (piece.window)
                rogue->RemoveAurasDueToSpell(piece.window);
            if (piece.tail && ended && rogue->IsAlive() && rogue->HasAura(piece.passive))
                rogue->CastSpell(rogue, piece.tail, true);
        }
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_ruffian_evasion::HandleApply, EFFECT_0, SPELL_AURA_MOD_DODGE_PERCENT, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_ruffian_evasion::HandleRemove, EFFECT_0, SPELL_AURA_MOD_DODGE_PERCENT, AURA_EFFECT_HANDLE_REAL);
    }
};

// Perfect Counter: the proc row matches the NAME Riposte, which also reaches Blind and Pick Pocket (they share a class bit); only the stock Riposte may put the debuff on the target.
class spell_ruffian_perfect_counter : public AuraScript
{
    PrepareAuraScript(spell_ruffian_perfect_counter);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        return spellInfo && spellInfo->SpellFamilyName == SPELLFAMILY_ROGUE && (spellInfo->SpellFamilyFlags[1] & RIPOSTE_FAMILY_FLAG_1);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_ruffian_perfect_counter::CheckProc);
    }
};

// Smoke Sweep: effect 1 is a flat THREAT effect on every enemy within 8 yards. The core adds such threat with every modifier switched off (Spell::EffectThreat passes ignoreModifiers), so the threat
// modifiers of the Boons (Choking Smoke, Hall of Mirrors, the Favor of the Smoke and Mirrors) and the Evasion Stance's +25% would not touch it. The default effect is therefore suppressed and the same
// amount is added with the modifiers on. It is threat only: it never taunts.
class spell_ruffian_smoke_sweep : public SpellScript
{
    PrepareSpellScript(spell_ruffian_smoke_sweep);

    void HandleThreat(SpellEffIndex /*effIndex*/)
    {
        PreventHitDefaultEffect(EFFECT_1);

        Unit* rogue = GetCaster();
        Unit* enemy = GetHitUnit();
        if (!rogue || !enemy || !rogue->IsAlive() || !enemy->IsAlive() || rogue->IsFriendlyTo(enemy) || !enemy->CanHaveThreatList())
            return;

        enemy->GetThreatMgr().AddThreat(rogue, float(GetEffectValue()), GetSpellInfo());
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ruffian_smoke_sweep::HandleThreat, EFFECT_1, SPELL_EFFECT_THREAT);
    }
};

void AddMoonlitBlessingsEvaderScripts()
{
    RegisterSpellScript(spell_ruffian_evasion);
    RegisterSpellScript(spell_ruffian_perfect_counter);
    RegisterSpellScript(spell_ruffian_smoke_sweep);
}
