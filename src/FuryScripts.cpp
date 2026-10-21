// Moonlit Blessings: C++ spell scripts for the Fury Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/fury.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_fury_spinning_strike   Boon: a Heroic Strike hit also strikes the nearest other enemy near its target for a share of the damage
//   spell_blessings_fury_titan_stance      Boons (Titan's Might and Titan's Wrath): keeps the stance (spell id + 1) on while a two-handed weapon is wielded in each hand;
//                                          for Titan's Wrath it also lets the Bloodthirst proc (free Whirlwind) through only then
//   spell_blessings_fury_storm_dancer      Boon: a Whirlwind readies Bloodthirst again (spell and category cooldown removed)
//   spell_blessings_fury_unbound_fury      Boon: while Enraged the stance (spell id + 1: stun and fear immunity) is on and every landed blow gives 1 rage
//   spell_blessings_fury_berserker         Favor of the Berserker: the passive holds the data; MoonlitFuryScript extends the Enrage effects and keeps the stance up
//   spell_blessings_fury_titans_grip       Favor of the Titan's Grip: turns the core's Titan's Grip switch on (and keeps it on)
//   MoonlitFuryScript                      Favor of the Berserker and Unbound Fury: reacts at once when an Enrage effect goes on or off the warrior
//
// "Enraged" = a positive aura of dispel type Enrage on the warrior (Enrage, Wrecking Crew, Death Wish, Berserker Rage, Bloodrage, Recklessness).
// Spell ids come from the Fury registry block 2,020,000 (tools/talents/recipes/fury.py asserts every unit and helper named here).
// ~Moonlit Nights Team

#include "Cell.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "UnitScript.h"
#include "WorldSession.h"
#include <list>

namespace
{
    enum FurySpells : uint32
    {
        SPELL_SPINNING_STRIKE_HIT = 2020001,    // fixed physical hit (the second strike of Spinning Strike)
        SPELL_UNBOUND_FURY        = 2020050,    // the Boon's passive: Unbound Fury is active while the warrior has this aura
        SPELL_UNBOUND_FURY_STANCE = 2020051,    // stun and fear immunity
        SPELL_FAVOR_BERSERKER     = 2020060,    // the Favor's passive: effect 0 holds the extra Enrage duration in milliseconds
        SPELL_BERSERKER_STANCE    = 2020061     // damage and attack speed
    };

    constexpr float SPINNING_STRIKE_RANGE = 8.0f;           // yards from the Heroic Strike target within which the second enemy is searched
    constexpr uint32 BLOODTHIRST_FLAGS1 = 0x00000400;       // word 1 of the Bloodthirst class mask (the cast and its ranks; the heal aura 23885 carries no mask)

    // Both hands hold a two-handed weapon (an off-hand weapon that is broken does not count).
    bool IsTitanWielding(Unit const* unit)
    {
        Player const* player = unit ? unit->ToPlayer() : nullptr;
        if (!player)
            return false;

        for (WeaponAttackType const attackType : { BASE_ATTACK, OFF_ATTACK })
        {
            Item const* weapon = player->GetWeaponForAttack(attackType, true);
            if (!weapon || weapon->GetTemplate()->InventoryType != INVTYPE_2HWEAPON)
                return false;
        }

        return true;
    }

    // Keep a stance buff on exactly while its condition holds.
    void SyncStance(Unit* unit, uint32 stanceId, bool wanted)
    {
        bool const has = unit->HasAura(stanceId);
        if (wanted && !has)
            unit->CastSpell(unit, stanceId, true);
        else if (!wanted && has)
            unit->RemoveAurasDueToSpell(stanceId);
    }

    // An Enrage effect: a positive aura of dispel type Enrage.
    bool IsEnrageAura(Aura const* aura)
    {
        SpellInfo const* info = aura ? aura->GetSpellInfo() : nullptr;
        return info && info->Dispel == DISPEL_ENRAGE;
    }

    // Is an Enrage effect on the unit? `ignored` is an aura that is just being removed and must not count.
    bool IsEnraged(Unit const* unit, Aura const* ignored = nullptr)
    {
        for (auto const& entry : unit->GetAppliedAuras())
        {
            AuraApplication const* app = entry.second;
            Aura const* aura = app->GetBase();
            if (aura != ignored && app->IsPositive() && !app->GetRemoveMode() && IsEnrageAura(aura))
                return true;
        }

        return false;
    }

    // Put the Enrage-driven stances of the Boon and of the Favor on the warrior or take them off (nothing happens for a Boon or Favor the warrior does not have).
    void SyncEnrageStances(Unit* unit, Aura const* ignored = nullptr)
    {
        bool const unbound = unit->HasAura(SPELL_UNBOUND_FURY);
        bool const berserker = unit->HasAura(SPELL_FAVOR_BERSERKER);
        if (!unbound && !berserker)
        {
            // neither is known any more: take a stance that is left over off
            SyncStance(unit, SPELL_UNBOUND_FURY_STANCE, false);
            SyncStance(unit, SPELL_BERSERKER_STANCE, false);
            return;
        }

        bool const enraged = unit->IsAlive() && IsEnraged(unit, ignored);
        SyncStance(unit, SPELL_UNBOUND_FURY_STANCE, unbound && enraged);
        SyncStance(unit, SPELL_BERSERKER_STANCE, berserker && enraged);
    }

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

    // Take the cooldown of every Bloodthirst rank the player knows away (the spell and its category).
    void ReadyBloodthirst(Player* player)
    {
        for (auto const& entry : player->GetSpellMap())
        {
            if (entry.second->State == PLAYERSPELL_REMOVED)
                continue;

            SpellInfo const* info = sSpellMgr->GetSpellInfo(entry.first);
            if (!info || info->SpellFamilyName != SPELLFAMILY_WARRIOR || !(info->SpellFamilyFlags[1] & BLOODTHIRST_FLAGS1))
                continue;

            if (info->GetCategory())
                player->RemoveCategoryCooldown(info->GetCategory());

            player->RemoveSpellCooldown(info->Id, true);
        }
    }
}

// Spinning Strike: effect 0 holds the percent (30). The strike goes to the nearest other enemy within SPINNING_STRIKE_RANGE of the Heroic Strike target.
class spell_blessings_fury_spinning_strike : public AuraScript
{
    PrepareAuraScript(spell_blessings_fury_spinning_strike);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        DamageInfo const* damageInfo = eventInfo.GetDamageInfo();
        Unit* victim = eventInfo.GetActionTarget();
        if (!damageInfo || !damageInfo->GetDamage() || !victim)
            return;

        Unit* caster = GetTarget();
        Unit* best = nullptr;
        float bestDistance = SPINNING_STRIKE_RANGE + 1.0f;
        for (Unit* unit : EnemiesAround(caster, victim, SPINNING_STRIKE_RANGE))
        {
            if (unit == victim || !unit->IsAlive())
                continue;

            float const distance = victim->GetDistance(unit);
            if (distance < bestDistance)
            {
                best = unit;
                bestDistance = distance;
            }
        }

        int32 const amount = int32(CalculatePct(damageInfo->GetDamage(), aurEff->GetAmount()));
        if (best && amount > 0)
            caster->CastCustomSpell(SPELL_SPINNING_STRIKE_HIT, SPELLVALUE_BASE_POINT0, amount - 1, best, true, nullptr, aurEff);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_fury_spinning_strike::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Titan's Might and Titan's Wrath: effect 0 is the one second check. The stance is the spell with the next id (the recipe asserts it); it is on while a two-handed weapon is wielded in
// each hand. For Titan's Wrath the Bloodthirst proc (a trigger of the free Whirlwind buff) is let through only while that holds.
class spell_blessings_fury_titan_stance : public AuraScript
{
    PrepareAuraScript(spell_blessings_fury_titan_stance);

    bool CheckProc(ProcEventInfo& /*eventInfo*/)
    {
        return IsTitanWielding(GetTarget());
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        SyncStance(target, GetId() + 1, IsTitanWielding(target));
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* target = GetTarget();
        SyncStance(target, GetId() + 1, target->IsAlive() && IsTitanWielding(target));
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(GetId() + 1);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_blessings_fury_titan_stance::CheckProc);
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_fury_titan_stance::HandleApply, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_fury_titan_stance::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_fury_titan_stance::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Storm Dancer: a Whirlwind readies Bloodthirst again. (The Bloodthirst half, the free Whirlwind buff, is plain spell data.)
class spell_blessings_fury_storm_dancer : public AuraScript
{
    PrepareAuraScript(spell_blessings_fury_storm_dancer);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        if (Player* player = GetTarget()->ToPlayer())
            ReadyBloodthirst(player);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_fury_storm_dancer::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Unbound Fury: effect 0 is the one second check (the stance is also put on and taken off at once by MoonlitFuryScript), effect 1 the rage proc, which passes only while Enraged.
class spell_blessings_fury_unbound_fury : public AuraScript
{
    PrepareAuraScript(spell_blessings_fury_unbound_fury);

    bool CheckProc(ProcEventInfo& /*eventInfo*/)
    {
        return IsEnraged(GetTarget());
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        SyncEnrageStances(GetTarget());
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        SyncEnrageStances(GetTarget());
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        // the passive is going: SyncEnrageStances would still see it, so take the stance off directly
        GetTarget()->RemoveAurasDueToSpell(SPELL_UNBOUND_FURY_STANCE);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_blessings_fury_unbound_fury::CheckProc);
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_fury_unbound_fury::HandleApply, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_fury_unbound_fury::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_fury_unbound_fury::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Favor of the Berserker: effect 1 is the one second check. The extra Enrage duration (effect 0) is applied by MoonlitFuryScript.
class spell_blessings_fury_berserker : public AuraScript
{
    PrepareAuraScript(spell_blessings_fury_berserker);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        SyncEnrageStances(GetTarget());
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        SyncEnrageStances(GetTarget());
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(SPELL_BERSERKER_STANCE);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_fury_berserker::HandleApply, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_fury_berserker::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_fury_berserker::HandleRemove, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Favor of the Titan's Grip: the core's Titan's Grip switch (what stock Titan's Grip, 46917, would turn on) is on while the Favor is. The core clears it on a spec change, so the one second
// check turns it on again. Like Warden's Titan's Maul, the off-hand weapon is put back in the bags when the Favor is dropped (not when the player merely logs out).
class spell_blessings_fury_titans_grip : public AuraScript
{
    PrepareAuraScript(spell_blessings_fury_titans_grip);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Player* player = GetTarget()->ToPlayer())
            player->SetCanTitanGrip(true);
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        if (Player* player = GetTarget()->ToPlayer())
            if (!player->CanTitanGrip())
                player->SetCanTitanGrip(true);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player)
            return;

        player->SetCanTitanGrip(false);
        if (player->GetSession() && !player->GetSession()->PlayerLogout())
            player->AutoUnequipOffhandIfNeed();
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_fury_titans_grip::HandleApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_fury_titans_grip::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_fury_titans_grip::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Favor of the Berserker and Unbound Fury, at the moment an Enrage effect goes on (or comes off) a player. Everything is rejected early: a player first, then an Enrage aura the player
// cast on himself, then the Boon or Favor itself.
class MoonlitFuryScript : public UnitScript
{
public:
    MoonlitFuryScript() : UnitScript("MoonlitFuryScript", true, { UNITHOOK_ON_AURA_APPLY, UNITHOOK_ON_AURA_REMOVE }) { }

    // The hook also fires when an aura that is already on the unit is refreshed or stacked, so the longer duration is always computed from the spell's own duration, never added to the
    // current one.
    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        Player* player = unit ? unit->ToPlayer() : nullptr;
        if (!player || !aura || aura->GetType() != UNIT_AURA_TYPE || !IsEnrageAura(aura) || aura->GetCasterGUID() != player->GetGUID())
            return;

        if (AuraEffect const* favor = player->GetAuraEffect(SPELL_FAVOR_BERSERKER, EFFECT_0))
        {
            int32 const duration = aura->CalcMaxDuration(player);
            if (duration > 0 && !aura->IsPermanent() && favor->GetAmount() > 0)
            {
                aura->SetMaxDuration(duration + favor->GetAmount());
                aura->SetDuration(duration + favor->GetAmount());
            }
        }

        SyncEnrageStances(player);
    }

    void OnAuraRemove(Unit* unit, AuraApplication* aurApp, AuraRemoveMode /*mode*/) override
    {
        Player* player = unit ? unit->ToPlayer() : nullptr;
        Aura* aura = aurApp ? aurApp->GetBase() : nullptr;
        if (!player || !aura || aura->GetType() != UNIT_AURA_TYPE || !IsEnrageAura(aura))
            return;

        SyncEnrageStances(player, aura);
    }
};

void AddMoonlitBlessingsFuryScripts()
{
    RegisterSpellScript(spell_blessings_fury_spinning_strike);
    RegisterSpellScript(spell_blessings_fury_titan_stance);
    RegisterSpellScript(spell_blessings_fury_storm_dancer);
    RegisterSpellScript(spell_blessings_fury_unbound_fury);
    RegisterSpellScript(spell_blessings_fury_berserker);
    RegisterSpellScript(spell_blessings_fury_titans_grip);
    new MoonlitFuryScript();
}
