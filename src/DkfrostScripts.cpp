// Moonlit Blessings: C++ spell scripts for the Death Knight Frost (role id "dkfrost") Boons and Favors (registered from AddSC_moonlit_blessings through AddMoonlitBlessingsDkfrostScripts).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/dkfrost.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_dkfrost_razorice                  Boon: auto attacks deal 4% again as Frost damage and stack a Frost damage debuff on the target
//   spell_blessings_dkfrost_killing_frost             Boon (keystone): auto attacks cast the stock Killing Machine buff, twice the rate of the Boon of that name
//   spell_blessings_dkfrost_killing_machine_companion bound to the stock Killing Machine buff (51124): puts the +20% critical damage companion of Killing Frost on with it and takes it off with it
//   spell_blessings_dkfrost_killing_machine_ranged    supplement of the Boon Killing Machine: the icy projectile (a ranged auto attack) casts the Killing Machine buff too
//   spell_blessings_dkfrost_rime_icy_touch            supplement of the Boon Rime: Icy Touch has the same chance as Obliterate to reset Howling Blast and give Freezing Fog
//   spell_blessings_dkfrost_howling_blast             bound to the Howling Blast ranks: infects the target with Frost Fever, and every enemy hit with Howling Contagion
//   spell_blessings_dkfrost_pillar_of_winter          bound to Deathchill (49796): the Boon Pillar of Winter adds the Strength buff
//   spell_blessings_dkfrost_permafrost                Boon: auto attacks add to an ice shield (an absorb) with a cap
//   spell_blessings_dkfrost_wyrms_embrace             Boon (keystone): Icy Touch, Chains of Ice and Howling Blast copy Frost Fever to the nearest other enemy
//   spell_blessings_dkfrost_cold_twins                Favor of the Cold Twins: teaches Dual Wield
//   spell_blessings_dkfrost_greatblade_stance         Favor of the Glacial Greatblade: keeps the +4% damage stance on while a two-handed weapon is wielded
//   spell_blessings_dkfrost_shardcaster               Favor of the Shardcaster: teaches the Icy Projectile and Ice Shard while the Favor lasts
//   spell_blessings_dkfrost_no_frost_strike           bound to the Frost Strike ranks: the Favor of the Shardcaster cannot cast it (Ice Shard takes its place)
//   MoonlitDkfrostUnitScript                          UnitScript: Razorice, the extra Frost damage a target takes from the death knight (direct and periodic)
//
// Spell ids come from the Frost registry block 1,120,000 (the units are listed at the top of tools/talents/recipes/dkfrost.py, which asserts the ids named below). The amounts, ranges and helper
// spells are read from the effects of the Boon's or Favor's own passive, so the spell data stays the one place a number is written. Every script first checks that the death knight really has the
// Boon or Favor aura. The helper hits are cast triggered (no ammunition, no runes, no runic power).
// ~Moonlit Nights Team

#include "Cell.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "UnitScript.h"
#include "WorldSession.h"

#include <algorithm>
#include <chrono>
#include <list>

namespace
{
    enum DkfrostSpells : uint32
    {
        SPELL_ICY_PROJECTILE        = 1120000,   // the Shardcaster's auto attack (a clone of Auto Shot, taught by the Favor)
        SPELL_ICE_SHARD             = 1120010,   // the Shardcaster's runic power spender (a clone of Frost Strike, taught by the Favor)
        SPELL_RAZORICE              = 1120020,   // effect 0: amount = percent of damage again, trigger spell = the Frost hit; effect 1: amount = percent per stack, trigger spell = the debuff
        SPELL_KILLING_FROST         = 1120030,   // effect 0: procs a minute with the Boon Killing Machine, 1: without, 2: trigger spell = the critical damage companion
        SPELL_HOWLING_CONTAGION     = 1120050,   // effect 1 is a marker: Howling Blast infects every enemy it hits
        SPELL_PILLAR_OF_WINTER      = 1120060,   // effect 1: trigger spell = the Strength buff
        SPELL_PERMAFROST            = 1120070,   // effect 0: amount = percent of the damage, trigger spell = the shield; effect 1: cap in percent of maximum health
        SPELL_WYRMS_EMBRACE         = 1120090,   // effect 0: amount = yards
        SPELL_COLD_TWINS            = 1120100,   // the Favor passive
        SPELL_GREATBLADE_STANCE     = 1120121,   // the visible +4% damage stance
        SPELL_SHARDCASTER           = 1120130,   // the Favor passive
        SPELL_KILLING_MACHINE_RANGED = 1120140,  // effect 0: procs a minute
        SPELL_RIME_ICY_TOUCH        = 1120150    // the Rime supplement
    };

    enum StockSpells : uint32
    {
        SPELL_KILLING_MACHINE       = 51124,     // the stock buff (the stock script spell_dk_killing_machine widens its mask to Icy Touch, Frost Strike and Howling Blast)
        SPELL_FROST_FEVER           = 55095,
        SPELL_FREEZING_FOG          = 59052,     // Rime: the next Howling Blast costs no runes
        SPELL_HOWLING_BLAST_RANK_1  = 49184,
        SPELL_DUAL_WIELD            = 674        // Dual Wield Passive: SPELL_EFFECT_DUAL_WIELD (the core drops dual wield from a player who does not know it)
    };

    constexpr uint32 EVENT_DELAY_MS = 1;         // work that must not run inside the code that decided on it waits one millisecond of game time

    // Every attackable enemy of `seeker` within `range` yards of `center` that `center` can see.
    std::list<Unit*> EnemiesAround(Unit* seeker, Unit* center, float range)
    {
        std::list<Unit*> targets;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(center, seeker, range);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(center, targets, check);
        Cell::VisitObjects(center, searcher, range);
        targets.remove_if([seeker, center](Unit* unit) { return !seeker->IsValidAttackTarget(unit) || !center->IsWithinLOSInMap(unit); });
        return targets;
    }

    // Hit `target` as `caster` with one of the flat-hit helper spells (instant, no spell power, cannot critically hit): the damage is exactly `amount`.
    void CastFlat(Unit* caster, Unit* target, uint32 spellId, int32 amount)
    {
        if (!caster || !target || amount <= 0 || !target->IsAlive())
            return;

        caster->CastCustomSpell(spellId, SPELLVALUE_BASE_POINT0, amount - 1, target, true);     // the spell adds 1 to its base points
    }

    // The buff `spellId` is on the unit exactly when `on` is true.
    void SyncStance(Unit* unit, uint32 spellId, bool on)
    {
        if (on)
        {
            if (!unit->HasAura(spellId))
                unit->CastSpell(unit, spellId, true);
        }
        else
            unit->RemoveAurasDueToSpell(spellId);
    }

    // A two-handed melee weapon in the main hand.
    bool IsTwoHanded(Unit const* unit)
    {
        Player const* player = unit ? unit->ToPlayer() : nullptr;
        if (!player)
            return false;

        Item const* item = player->GetWeaponForAttack(BASE_ATTACK, true);
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        return proto && proto->InventoryType == INVTYPE_2HWEAPON;
    }

    // One roll for a proc that happens `ppm` times a minute with the main-hand weapon (its speed with haste, like the stock Killing Machine).
    bool RollPerMinute(Player* player, float ppm, SpellInfo const* info)
    {
        return roll_chance_f(player->GetPPMProcChance(player->GetAttackTime(BASE_ATTACK), ppm, info));
    }
}

// Razorice. Effect 0 trigger spell = the Frost hit (flat), effect 1 trigger spell = the debuff. The proc is any auto attack (melee swing or icy projectile, both hands) that did damage.
class spell_blessings_dkfrost_razorice : public AuraScript
{
    PrepareAuraScript(spell_blessings_dkfrost_razorice);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* actor = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        if (!victim || victim == actor || !victim->IsAlive() || !actor->IsValidAttackTarget(victim))
            return;

        if (DamageInfo* damageInfo = eventInfo.GetDamageInfo())
            if (damageInfo->GetDamage() > 0)
                CastFlat(actor, victim, GetSpellInfo()->Effects[EFFECT_0].TriggerSpell, int32(CalculatePct(damageInfo->GetDamage(), uint32(std::max(aurEff->GetAmount(), 0)))));

        actor->CastSpell(victim, GetSpellInfo()->Effects[EFFECT_1].TriggerSpell, true);      // one more stack (the debuff stacks to 5 and renews its 15 sec)
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_dkfrost_razorice::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Killing Frost. The proc is an auto attack (melee swing, either hand, or icy projectile). Like the stock Killing Machine it ignores off-hand swings. The rate is effect 0 (5 a minute) when
// the death knight also has the Boon Killing Machine (whose supplement aura marks it) and effect 1 (10 a minute) when not; both are main-hand speed based.
class spell_blessings_dkfrost_killing_frost : public AuraScript
{
    PrepareAuraScript(spell_blessings_dkfrost_killing_frost);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Player* player = GetTarget()->ToPlayer();
        if (!player || (eventInfo.GetTypeMask() & PROC_FLAG_DONE_OFFHAND_ATTACK))
            return;

        AuraEffect const* withBoon = GetEffect(EFFECT_0);
        AuraEffect const* withoutBoon = GetEffect(EFFECT_1);
        if (!withBoon || !withoutBoon)
            return;

        float const ppm = float(player->HasAura(SPELL_KILLING_MACHINE_RANGED) ? withBoon->GetAmount() : withoutBoon->GetAmount());
        if (RollPerMinute(player, ppm, GetSpellInfo()))
            player->CastSpell(player, SPELL_KILLING_MACHINE, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_dkfrost_killing_frost::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Bound to the stock Killing Machine buff (51124, one effect: a flat critical chance modifier). Every Killing Machine buff the death knight gets while he has Killing Frost brings the +20% critical
// damage companion (effect 2 of Killing Frost names it), and the companion leaves with the buff, whether it was spent or ran out.
class spell_blessings_dkfrost_killing_machine_companion : public AuraScript
{
    PrepareAuraScript(spell_blessings_dkfrost_killing_machine_companion);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        if (AuraEffect const* frost = target->GetAuraEffect(SPELL_KILLING_FROST, EFFECT_2))
            target->CastSpell(target, frost->GetSpellInfo()->Effects[EFFECT_2].TriggerSpell, true);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        if (SpellInfo const* frost = sSpellMgr->GetSpellInfo(SPELL_KILLING_FROST))
            target->RemoveAurasDueToSpell(frost->Effects[EFFECT_2].TriggerSpell);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_dkfrost_killing_machine_companion::HandleApply, EFFECT_0, SPELL_AURA_ADD_FLAT_MODIFIER, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_dkfrost_killing_machine_companion::HandleRemove, EFFECT_0, SPELL_AURA_ADD_FLAT_MODIFIER, AURA_EFFECT_HANDLE_REAL);
    }
};

// Supplement of the Boon Killing Machine: the stock aura procs on melee swings only; this one is the same chance (effect 0: procs a minute, the rank 5 rate) for the icy projectile.
class spell_blessings_dkfrost_killing_machine_ranged : public AuraScript
{
    PrepareAuraScript(spell_blessings_dkfrost_killing_machine_ranged);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        Player* player = GetTarget()->ToPlayer();
        if (player && RollPerMinute(player, float(aurEff->GetAmount()), GetSpellInfo()))
            player->CastSpell(player, SPELL_KILLING_MACHINE, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_dkfrost_killing_machine_ranged::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Supplement of the Boon Rime: the proc (15%) is a cast of Icy Touch; it resets the cooldown of Howling Blast (all ranks, like the stock spell_dk_rime) and casts Freezing Fog.
class spell_blessings_dkfrost_rime_icy_touch : public AuraScript
{
    PrepareAuraScript(spell_blessings_dkfrost_rime_icy_touch);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        Player* player = GetTarget()->ToPlayer();
        if (!player)
            return;

        for (SpellInfo const* info = sSpellMgr->GetSpellInfo(SPELL_HOWLING_BLAST_RANK_1); info; info = info->GetNextRankSpell())
            player->RemoveSpellCooldown(info->Id, true);

        player->CastSpell(player, SPELL_FREEZING_FOG, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_dkfrost_rime_icy_touch::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Howling Blast (bound to the whole rank chain). The stock spell infects only with a glyph, but the kit text of the Frost Blessing says Howling Blast infects the target, so effect 0 (the dummy on
// the explicit target) casts Frost Fever on it. Effect 1 is the damage on every enemy within 10 yards: with Howling Contagion each of them is infected too.
class spell_blessings_dkfrost_howling_blast : public SpellScript
{
    PrepareSpellScript(spell_blessings_dkfrost_howling_blast);

    void InfectTarget(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (caster && target && target->IsAlive() && caster->IsValidAttackTarget(target))
            caster->CastSpell(target, SPELL_FROST_FEVER, true);
    }

    void InfectEveryone(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (caster && target && target->IsAlive() && caster->HasAura(SPELL_HOWLING_CONTAGION) && caster->IsValidAttackTarget(target))
            caster->CastSpell(target, SPELL_FROST_FEVER, true);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_blessings_dkfrost_howling_blast::InfectTarget, EFFECT_0, SPELL_EFFECT_DUMMY);
        OnEffectHitTarget += SpellEffectFn(spell_blessings_dkfrost_howling_blast::InfectEveryone, EFFECT_1, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// Pillar of Winter: bound to Deathchill (49796). After the cast the death knight with the Boon gets the Strength buff named by effect 1's trigger spell.
class spell_blessings_dkfrost_pillar_of_winter : public SpellScript
{
    PrepareSpellScript(spell_blessings_dkfrost_pillar_of_winter);

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        if (AuraEffect const* pillar = caster->GetAuraEffect(SPELL_PILLAR_OF_WINTER, EFFECT_1))
            caster->CastSpell(caster, pillar->GetSpellInfo()->Effects[EFFECT_1].TriggerSpell, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_blessings_dkfrost_pillar_of_winter::HandleAfterCast);
    }
};

// Permafrost. Effect 0: amount = percent of the damage an auto attack did, trigger spell = the shield (one absorb of all schools). Effect 1: amount = the cap in percent of maximum health.
// A hit adds to the shield that is there (its amount is set) and renews its duration; without one the shield is cast with the sum.
class spell_blessings_dkfrost_permafrost : public AuraScript
{
    PrepareAuraScript(spell_blessings_dkfrost_permafrost);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Player* player = GetTarget()->ToPlayer();
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        AuraEffect const* cap = GetEffect(EFFECT_1);
        if (!player || !damageInfo || !cap || damageInfo->GetDamage() == 0)
            return;

        uint32 const shieldSpell = GetSpellInfo()->Effects[EFFECT_0].TriggerSpell;
        int32 const gain = int32(CalculatePct(damageInfo->GetDamage(), uint32(std::max(aurEff->GetAmount(), 0))));
        int32 const ceiling = int32(CalculatePct(player->GetMaxHealth(), uint32(std::max(cap->GetAmount(), 0))));
        Aura* shield = player->GetAura(shieldSpell);
        AuraEffect* shieldEffect = shield ? shield->GetEffect(EFFECT_0) : nullptr;
        int32 const total = std::min(gain + (shieldEffect ? shieldEffect->GetAmount() : 0), ceiling);
        if (total <= 0)
            return;

        if (shieldEffect)
        {
            shieldEffect->SetAmount(total);
            shield->RefreshDuration();
        }
        else
            player->CastCustomSpell(shieldSpell, SPELLVALUE_BASE_POINT0, total - 1, player, true);       // the spell adds 1 to its base points
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_dkfrost_permafrost::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Wyrm's Embrace. Effect 0: amount = yards. The proc is a hit of Icy Touch, Chains of Ice or Howling Blast (0.2 sec apart at most). One millisecond later the disease is on the target and is copied to
// the nearest other enemy within range that has none of the death knight's Frost Fever. The copy is a direct cast of Frost Fever, which the proc does not match, so it never spreads again.
class spell_blessings_dkfrost_wyrms_embrace : public AuraScript
{
    PrepareAuraScript(spell_blessings_dkfrost_wyrms_embrace);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        if (!caster->IsPlayer() || !victim || victim == caster || !caster->IsValidAttackTarget(victim))
            return;

        ObjectGuid const victimGuid = victim->GetGUID();
        float const range = float(std::max(aurEff->GetAmount(), 0));
        caster->m_Events.AddEventAtOffset([caster, victimGuid, range]()
        {
            Unit* source = ObjectAccessor::GetUnit(*caster, victimGuid);
            if (!source || !source->IsAlive() || !caster->IsAlive() || !source->HasAura(SPELL_FROST_FEVER, caster->GetGUID()))
                return;

            Unit* best = nullptr;
            float bestDistance = range + 1.0f;
            for (Unit* unit : EnemiesAround(caster, source, range))
            {
                if (unit == source || !unit->IsAlive() || unit->HasAura(SPELL_FROST_FEVER, caster->GetGUID()) || unit->HasBreakableByDamageCrowdControlAura())
                    continue;

                float const distance = source->GetDistance(unit);
                if (distance < bestDistance)
                {
                    best = unit;
                    bestDistance = distance;
                }
            }

            if (best)
                caster->CastSpell(best, SPELL_FROST_FEVER, true);
        }, std::chrono::milliseconds(EVENT_DELAY_MS));
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_dkfrost_wyrms_embrace::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Favor of the Cold Twins: the Favor aura teaches Dual Wield (a death knight whose trainer is closed to him learns it nowhere else). It is not taken away again, like the Survival Twin Talons.
// Hooked on effect 1 (the off-hand damage aura).
class spell_blessings_dkfrost_cold_twins : public AuraScript
{
    PrepareAuraScript(spell_blessings_dkfrost_cold_twins);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (player && player->getClass() == CLASS_DEATH_KNIGHT && !player->HasSpell(SPELL_DUAL_WIELD))
            player->learnSpell(SPELL_DUAL_WIELD);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_dkfrost_cold_twins::HandleApply, EFFECT_1, SPELL_AURA_MOD_OFFHAND_DAMAGE_PCT, AURA_EFFECT_HANDLE_REAL);
    }
};

// Favor of the Glacial Greatblade, second aura: once a second (and at once when the Favor is switched on) the stance buff (+4% damage) is put on the death knight while the main-hand weapon is
// two-handed and taken off otherwise.
class spell_blessings_dkfrost_greatblade_stance : public AuraScript
{
    PrepareAuraScript(spell_blessings_dkfrost_greatblade_stance);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        SyncStance(target, SPELL_GREATBLADE_STANCE, IsTwoHanded(target));
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* target = GetTarget();
        SyncStance(target, SPELL_GREATBLADE_STANCE, target->IsAlive() && IsTwoHanded(target));
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(SPELL_GREATBLADE_STANCE);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_dkfrost_greatblade_stance::HandleApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_dkfrost_greatblade_stance::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_dkfrost_greatblade_stance::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Favor of the Shardcaster: a Favor is only an aura, so it teaches the Icy Projectile and Ice Shard on apply and takes them away (and stops the firing) when it is dropped, but not when the
// player merely logs out.
class spell_blessings_dkfrost_shardcaster : public AuraScript
{
    PrepareAuraScript(spell_blessings_dkfrost_shardcaster);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player || player->getClass() != CLASS_DEATH_KNIGHT)
            return;

        if (!player->HasSpell(SPELL_ICY_PROJECTILE))
            player->learnSpell(SPELL_ICY_PROJECTILE);

        if (!player->HasSpell(SPELL_ICE_SHARD))
            player->learnSpell(SPELL_ICE_SHARD);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player || !player->GetSession() || player->GetSession()->PlayerLogout() || player->getClass() != CLASS_DEATH_KNIGHT)
            return;

        player->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
        player->removeSpell(SPELL_ICY_PROJECTILE, SPEC_MASK_ALL, false);
        player->removeSpell(SPELL_ICE_SHARD, SPEC_MASK_ALL, false);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_dkfrost_shardcaster::HandleApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_dkfrost_shardcaster::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Bound to the Frost Strike rank chain: Ice Shard takes the place of Frost Strike for the Favor of the Shardcaster. (The Lua system should take the spell out of the spell book through
// blessing_spells.FAVOR_REMOVES; this is the safety net.)
class spell_blessings_dkfrost_no_frost_strike : public SpellScript
{
    PrepareSpellScript(spell_blessings_dkfrost_no_frost_strike);

    SpellCastResult CheckCast()
    {
        if (GetCaster()->HasAura(SPELL_SHARDCASTER))
            return SPELL_FAILED_SPELL_UNAVAILABLE;

        return SPELL_CAST_OK;
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_blessings_dkfrost_no_frost_strike::CheckCast);
    }
};

class MoonlitDkfrostUnitScript : public UnitScript
{
public:
    MoonlitDkfrostUnitScript() : UnitScript("MoonlitDkfrostUnitScript", true, { UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN, UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK }) { }

    // Razorice: the extra percent of Frost damage a target takes from the death knight who put the debuff on it (the amount of effect 1 times the stacks).
    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo) override
    {
        if (damage > 0)
            if (uint32 const percent = RazoricePercent(target, attacker, spellInfo))
                damage += int32(CalculatePct(uint32(damage), percent));
    }

    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* spellInfo) override
    {
        if (damage > 0)
            if (uint32 const percent = RazoricePercent(target, attacker, spellInfo))
                damage += CalculatePct(damage, percent);
    }

private:
    static uint32 RazoricePercent(Unit* target, Unit* attacker, SpellInfo const* spellInfo)
    {
        if (!target || !attacker || !spellInfo || target == attacker || !attacker->IsPlayer() || !(spellInfo->GetSchoolMask() & SPELL_SCHOOL_MASK_FROST))
            return 0;

        if (spellInfo->Id == SPELL_RAZORICE + 1)          // the Razorice flat hit itself is exactly 4%, never raised by its own debuff
            return 0;

        AuraEffect const* razorice = attacker->GetAuraEffect(SPELL_RAZORICE, EFFECT_1);
        if (!razorice)
            return 0;

        Aura const* debuff = target->GetAura(razorice->GetSpellInfo()->Effects[EFFECT_1].TriggerSpell, attacker->GetGUID());
        return debuff ? uint32(std::max(razorice->GetAmount(), 0)) * debuff->GetStackAmount() : 0;
    }
};

void AddMoonlitBlessingsDkfrostScripts()
{
    RegisterSpellScript(spell_blessings_dkfrost_razorice);
    RegisterSpellScript(spell_blessings_dkfrost_killing_frost);
    RegisterSpellScript(spell_blessings_dkfrost_killing_machine_companion);
    RegisterSpellScript(spell_blessings_dkfrost_killing_machine_ranged);
    RegisterSpellScript(spell_blessings_dkfrost_rime_icy_touch);
    RegisterSpellScript(spell_blessings_dkfrost_howling_blast);
    RegisterSpellScript(spell_blessings_dkfrost_pillar_of_winter);
    RegisterSpellScript(spell_blessings_dkfrost_permafrost);
    RegisterSpellScript(spell_blessings_dkfrost_wyrms_embrace);
    RegisterSpellScript(spell_blessings_dkfrost_cold_twins);
    RegisterSpellScript(spell_blessings_dkfrost_greatblade_stance);
    RegisterSpellScript(spell_blessings_dkfrost_shardcaster);
    RegisterSpellScript(spell_blessings_dkfrost_no_frost_strike);
    new MoonlitDkfrostUnitScript();
}
