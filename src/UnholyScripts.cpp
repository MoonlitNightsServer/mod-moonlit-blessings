// Moonlit Blessings: C++ spell scripts for the Death Knight Unholy Boons and Favors (registered from AddSC_moonlit_blessings through AddMoonlitBlessingsUnholyScripts).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/unholy.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   Boons
//   spell_blessings_unholy_epidemic_pestilence        bound to every Pestilence rank: with Epidemic the diseases on the primary target are refreshed (the Glyph of Disease effect)
//   spell_blessings_unholy_outbreak_blood_boil        bound to every Blood Boil rank: with Outbreak the diseases spread to, and are refreshed on, every enemy the Blood Boil hits
//   spell_blessings_unholy_defile_start               bound to every Death and Decay rank: a new Death and Decay starts without Defile stacks
//   spell_blessings_unholy_death_and_decay_pulse      bound to the pulse (52212): Blightbearer's +15% against diseased enemies, Defile's stack after each pulse
//   spell_blessings_unholy_summon_gargoyle            bound to Summon Gargoyle: Gargoyle Sovereign gives the Gargoyle the caster's haste
//   spell_blessings_unholy_raise_dead                 bound to Raise Dead: All Will Serve calls two Skeleton Archers for 20 sec
//   spell_blessings_unholy_apocalypse                 Apocalypse: every fifth Plague Strike, Scourge Strike or Plague Slash raises two ghouls for 15 sec
//   spell_blessings_unholy_disease_spread             bound to Blood Plague and Frost Fever: with Plague Sovereign they spread to enemies within 15 yards when the victim dies
//
//   Favors
//   spell_blessings_unholy_blightbearer_coil          bound to the Death Coil damage (47632): Blightbearer's +15% against diseased enemies
//   spell_blessings_unholy_plague_reaper              Favor of the Plague Reaper: teaches Plague Rot (and takes it away again)
//   spell_blessings_unholy_plague_rot                 Plague Rot: needs Festering Rot on the target, multiplies the hit by the stacks, splashes half, removes the stacks
//   spell_blessings_unholy_tomb_necromancer           Favor of the Tomb Necromancer: teaches Plague Slash and Corpse Toss, turns the Raise Dead ghoul into the Skeleton Archer
//   spell_blessings_unholy_plague_slash               Plague Slash: the 30% chance to apply Blood Plague
//   spell_blessings_unholy_slash_necrosis             the Necromancer's Mastery addition: 20% of every Plague Slash again as Shadow damage (Necrosis)
//   spell_blessings_unholy_corpse_toss                Corpse Toss: a corpse within 20 yards or 20 runic power, raises the ghoul, leaps it onto the target, 4 claws
//   spell_blessings_unholy_archer_mark                the mark aura of every Skeleton Archer (the permanent one and the temporary ones): shoots the owner's target every 2 sec
//   spell_blessings_unholy_striker_mark               the mark aura of every raised ghoul: claws the owner's target now and then
//
// Spell ids come from the Unholy blocks 1,130,000 - 1,130,249; tools/talents/recipes/unholy.py asserts every unit named here (a helper is the unit id + 1 ... as listed in its docstring).
//
// CORE CHANGE (made 2026-10-22, owner approved): Plague Slash (1130121) is in MOONLIT_SHEATHED_AUTO_SPELLS (SpellInfo.h): it counts as an Auto Shot, follows the main-hand swing speed (Unit::_UpdateAutoRepeatSpell)
// and replaces the melee swing while it is the auto-repeat spell (Player update). The spell is a clone of Auto Shot 75 without the ranged-slot and ammo attributes.
// ~Moonlit Nights Team

#include "Pet.h"   // BlessingsHelpers.h needs the full Pet type (it is not included there)
#include "BlessingsHelpers.h"
#include "Cell.h"
#include "CellImpl.h"
#include "CharmInfo.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "MotionMaster.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "TemporarySummon.h"
#include "WorldSession.h"
#include <algorithm>
#include <iterator>
#include <list>
#include <vector>

namespace
{
    enum UnholySpells : uint32
    {
        SPELL_EPIDEMIC              = 1130010,
        SPELL_OUTBREAK              = 1130040,
        SPELL_DEFILE                = 1130050,
        SPELL_DEFILE_HELPER         = 1130051,   // the stacking helper (10 stacks, 2 sec)
        SPELL_GARGOYLE_SOVEREIGN    = 1130060,
        SPELL_GARGOYLE_HASTE        = 1130061,   // effect 0: casting speed, effect 1: melee speed (percent, set per Gargoyle)
        SPELL_ALL_WILL_SERVE        = 1130070,
        SPELL_APOCALYPSE            = 1130080,
        SPELL_PLAGUE_SOVEREIGN      = 1130090,
        SPELL_FAVOR_BLIGHTBEARER    = 1130100,
        SPELL_FAVOR_REAPER          = 1130110,
        SPELL_FESTERING_ROT         = 1130111,   // the stacking debuff (8 stacks, 30 sec)
        SPELL_PLAGUE_ROT            = 1130112,
        SPELL_PLAGUE_ROT_SPLASH     = 1130113,
        SPELL_FAVOR_NECROMANCER     = 1130120,
        SPELL_PLAGUE_SLASH          = 1130121,
        SPELL_CORPSE_TOSS           = 1130122,
        SPELL_SUMMON_ARCHER         = 1130131,
        SPELL_SUMMON_GHOUL          = 1130132,
        SPELL_MARK_ARCHER           = 1130133,   // periodic dummy every 500 ms; effect 1: disarm
        SPELL_MARK_STRIKER          = 1130134,   // effect 0: periodic dummy every 500 ms, its amount is the claw damage in percent of weapon damage; effect 1: the most claws (0: no limit)
        SPELL_SKELETON_SHOT         = 1130135,
        SPELL_RAISED_CLAW           = 1130136,

        SPELL_BLOOD_PLAGUE          = 55078,
        SPELL_FROST_FEVER           = 55095,
        SPELL_GLYPH_OF_DISEASE      = 63334,
        SPELL_NECROSIS_DAMAGE       = 51460
    };

    constexpr uint32 MN_NPC_RISEN_GHOUL = 26125;                 // the Raise Dead pet, and the body of every raised minion
    constexpr uint32 MN_NPC_EBON_GARGOYLE = 27829;               // the Gargoyle of Summon Gargoyle
    constexpr uint32 SKELETON_DISPLAY = 9790;                 // CreatureDisplayInfo 9790: the SkeletonNaked model
    constexpr uint32 BOW_ITEM = 14105;                        // "Monster - Bow, C01/B02 White": what the archers hold in the ranged slot (the stock Scourge Archer 14489 holds it)
    constexpr int32 ARCHER_SHOT_PCT = 35;                     // percent of the owner's attack power per shot
    constexpr uint32 ARCHER_SHOT_MS = 2000;
    constexpr uint32 MARK_TICK_MS = 500;
    constexpr float ARCHER_RANGE = 35.0f;
    constexpr uint32 LEAP_CLAWS = 4;                          // Corpse Toss: claws
    constexpr int32 LEAP_CLAW_PCT = 60;                       // Corpse Toss: percent of weapon damage per claw
    constexpr uint32 LEAP_LIFETIME_MS = 8000;
    constexpr uint32 LEAP_CLAW_MS = 1500;
    constexpr int32 CORPSE_TOSS_RUNIC_POWER = 200;            // 20 runic power (stored x10)
    constexpr float CORPSE_RANGE = 20.0f;
    constexpr uint32 WILL_SERVE_LIFETIME_MS = 20000;
    constexpr uint32 WILL_SERVE_COUNT = 2;
    constexpr uint32 APOCALYPSE_EVERY = 5;
    constexpr uint32 APOCALYPSE_GHOULS = 2;
    constexpr uint32 APOCALYPSE_LIFETIME_MS = 15000;
    constexpr int32 APOCALYPSE_CLAW_PCT = 30;
    constexpr uint32 APOCALYPSE_CLAW_MS = 2000;
    constexpr float SPREAD_RANGE = 15.0f;                     // Plague Sovereign
    constexpr float ROT_SPLASH_RANGE = 8.0f;                  // Plague Rot
    constexpr int32 BLIGHTBEARER_PCT = 15;
    constexpr uint32 SLASH_PLAGUE_CHANCE = 30;

    int32 Param(Unit const* unit, uint32 spellId, uint8 effect = 0)
    {
        AuraEffect const* aurEff = unit ? unit->GetAuraEffect(spellId, effect) : nullptr;
        return aurEff ? aurEff->GetAmount() : 0;
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

    // The enemy the death knight is fighting: the selected one if it is attackable, otherwise the victim.
    Unit* FightTarget(Player* player)
    {
        if (!player->IsInCombat())                   // a mere click on a sleeping mob must not start a fight
            return nullptr;

        Unit* target = player->GetSelectedUnit();
        if (target && target->IsAlive() && player->IsValidAttackTarget(target))
            return target;

        target = player->GetVictim();
        return target && target->IsAlive() ? target : nullptr;
    }

    bool HasDisease(Unit const* target, Unit const* caster)
    {
        return target && caster && (target->HasAura(SPELL_BLOOD_PLAGUE, caster->GetGUID()) || target->HasAura(SPELL_FROST_FEVER, caster->GetGUID()));
    }

    // A corpse a Corpse Toss can use: a dead creature within range (not a mechanical or elemental one, the rule of Corpse Explosion).
    // Not a pet, a guardian or a friend (a skeleton archer or your own ghoul is not a corpse to use).
    Creature* FindCorpse(Unit* seeker, float range)
    {
        std::list<Unit*> units;
        Acore::AnyDeadUnitObjectInRangeCheck check(seeker, range);
        Acore::UnitListSearcher<Acore::AnyDeadUnitObjectInRangeCheck> searcher(seeker, units, check);
        Cell::VisitObjects(seeker, searcher, range);
        for (Unit* unit : units)
            if (Creature* corpse = unit->ToCreature())
                if (!corpse->IsPet() && !corpse->HasUnitTypeMask(UNIT_MASK_MINION | UNIT_MASK_GUARDIAN | UNIT_MASK_SUMMON) && !corpse->IsFriendlyTo(seeker)
                    && !(corpse->GetCreatureTypeMask() & CREATURE_TYPEMASK_MECHANICAL_OR_ELEMENTAL))
                    return corpse;

        return nullptr;
    }

    bool IsMinionMarked(Unit const* unit)
    {
        return unit->HasAura(SPELL_MARK_ARCHER) || unit->HasAura(SPELL_MARK_STRIKER);
    }

    // Dresses a creature as a Skeleton Archer and marks it: the skeleton model, a bow in the ranged slot, passive, the archer mark (shoots, cannot melee).
    void MakeArcher(Creature* archer)
    {
        archer->SetDisplayId(SKELETON_DISPLAY);                   // the native model stays the real one, so a pet saved while it is an archer never keeps the skeleton
        archer->SetUInt32Value(UNIT_VIRTUAL_ITEM_SLOT_ID + 2, BOW_ITEM);
        archer->SetReactState(REACT_PASSIVE);
        archer->AttackStop();
        if (!archer->HasAura(SPELL_MARK_ARCHER))
            archer->CastSpell(archer, SPELL_MARK_ARCHER, true);
    }

    // Takes the archer dress off a Raise Dead ghoul again (the Favor was dropped).
    void UnmakeArcher(Creature* archer)
    {
        archer->RemoveAurasDueToSpell(SPELL_MARK_ARCHER);
        archer->SetUInt32Value(UNIT_VIRTUAL_ITEM_SLOT_ID + 2, 0);
        archer->SetDisplayId(archer->GetNativeDisplayId());
        archer->SetReactState(REACT_DEFENSIVE);
    }

    // The ghoul lands (or appears) next to `target`, on the side of the owner, looking at it.
    void PlaceBeside(Creature* minion, Unit* owner, Unit* target, bool leap)
    {
        Position const pos = target->GetNearPosition(2.0f, target->GetRelativeAngle(owner));
        if (leap)
            minion->GetMotionMaster()->MoveJump(pos, 24.0f, 12.0f);
        else
            minion->NearTeleportTo(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), pos.GetOrientation());

        minion->SetFacingToObject(target);
    }

    // Calls one raised minion of the owner (a guardian of the stock Risen Ghoul, property 61: the owner's ally). The new guardian is the one of this summon spell that carries no mark yet.
    // archer: a Skeleton Archer (skeleton model, bow, shoots); otherwise a raised ghoul (claws for clawPercent of the owner's weapon damage, at most maxClaws times, 0: no limit).
    Creature* RaiseMinion(Player* owner, bool archer, uint32 lifetimeMs, int32 clawPercent = 0, uint32 maxClaws = 0)
    {
        uint32 const summonSpell = archer ? SPELL_SUMMON_ARCHER : SPELL_SUMMON_GHOUL;
        if (!sSpellMgr->GetSpellInfo(summonSpell) || !sSpellMgr->GetSpellInfo(archer ? SPELL_MARK_ARCHER : SPELL_MARK_STRIKER))
            return nullptr;

        owner->CastSpell(owner, summonSpell, true);

        std::vector<Unit*> controlled(owner->m_Controlled.begin(), owner->m_Controlled.end());
        for (Unit* unit : controlled)
        {
            Creature* minion = unit && unit->IsAlive() && unit->IsInWorld() ? unit->ToCreature() : nullptr;
            if (!minion || minion->GetEntry() != MN_NPC_RISEN_GHOUL || minion->GetUInt32Value(UNIT_CREATED_BY_SPELL) != summonSpell || IsMinionMarked(minion))
                continue;

            if (TempSummon* summon = minion->ToTempSummon())
                summon->SetTimer(lifetimeMs);

            minion->SetReactState(REACT_PASSIVE);
            if (archer)
            {
                MakeArcher(minion);
            }
            else
            {
                int32 const claw = std::max(clawPercent, 1) - 1;                 // base points: the aura shows base points + 1
                int32 const most = int32(maxClaws) - 1;                           // -1: amount 0, no limit
                minion->CastCustomSpell(minion, SPELL_MARK_STRIKER, &claw, &most, nullptr, true);
            }

            return minion;
        }

        return nullptr;
    }
}

// Epidemic, Pestilence part. Bound to every Pestilence rank. Without the Glyph of Disease the stock script only spreads; with Epidemic the primary target's Blood Plague and Frost Fever
// (and the Crypt Fever or Ebon Plague riding on them) are refreshed to their full duration, the same refresh the glyph gives. (The stock script does the refresh itself when the glyph is there.)
class spell_blessings_unholy_epidemic_pestilence : public SpellScript
{
    PrepareSpellScript(spell_blessings_unholy_epidemic_pestilence);

    void HandleScriptEffect(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Unit* hit = GetHitUnit();
        Unit* target = GetExplTargetUnit();
        if (!caster || !hit || hit != target || !caster->HasAura(SPELL_EPIDEMIC) || caster->HasAura(SPELL_GLYPH_OF_DISEASE))
            return;

        for (uint32 const disease : { uint32(SPELL_BLOOD_PLAGUE), uint32(SPELL_FROST_FEVER) })
            if (Aura* aura = target->GetAura(disease, caster->GetGUID()))
                aura->RefreshDuration();

        // Crypt Fever (50508 ...) and Ebon Plague (51726 ...) ride on the diseases; the ranked spells are found by their first rank
        if (Aura* aura = target->GetAuraOfRankedSpell(51726, caster->GetGUID()))
            aura->RefreshDuration();
        else if (Aura* aura = target->GetAuraOfRankedSpell(50508, caster->GetGUID()))
            aura->RefreshDuration();
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_blessings_unholy_epidemic_pestilence::HandleScriptEffect, EFFECT_2, SPELL_EFFECT_SCRIPT_EFFECT);
    }
};

// Outbreak, Blood Boil part. Bound to every Blood Boil rank. After the Blood Boil: if any enemy it hit carries one of the caster's diseases, every enemy it hit gets it (or has it refreshed to full duration).
class spell_blessings_unholy_outbreak_blood_boil : public SpellScript
{
    PrepareSpellScript(spell_blessings_unholy_outbreak_blood_boil);

    void CollectHit(SpellEffIndex /*effIndex*/)
    {
        if (Unit* target = GetHitUnit())
            _hit.push_back(target->GetGUID());
    }

    void Spread()
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(SPELL_OUTBREAK))
            return;

        std::vector<Unit*> units;
        for (ObjectGuid const guid : _hit)
            if (Unit* unit = ObjectAccessor::GetUnit(*caster, guid))
                if (unit->IsAlive())
                    units.push_back(unit);

        for (uint32 const disease : { uint32(SPELL_BLOOD_PLAGUE), uint32(SPELL_FROST_FEVER) })
        {
            bool present = false;
            for (Unit* unit : units)
                present = present || unit->HasAura(disease, caster->GetGUID());

            if (!present)
                continue;

            for (Unit* unit : units)
            {
                if (Aura* aura = unit->GetAura(disease, caster->GetGUID()))
                    aura->RefreshDuration();
                else
                    caster->CastSpell(unit, disease, true);
            }
        }
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_blessings_unholy_outbreak_blood_boil::CollectHit, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
        AfterCast += SpellCastFn(spell_blessings_unholy_outbreak_blood_boil::Spread);
    }

private:
    std::vector<ObjectGuid> _hit;
};

// Defile, start. Bound to every Death and Decay rank: a new Death and Decay begins without the stacks of the last one.
class spell_blessings_unholy_defile_start : public SpellScript
{
    PrepareSpellScript(spell_blessings_unholy_defile_start);

    void HandleCast()
    {
        if (Unit* caster = GetCaster())
            caster->RemoveAurasDueToSpell(SPELL_DEFILE_HELPER);
    }

    void Register() override
    {
        OnCast += SpellCastFn(spell_blessings_unholy_defile_start::HandleCast);
    }
};

// The pulse of Death and Decay (52212, cast every second by the ground aura). Blightbearer: +15% against diseased enemies. Defile: after each pulse the helper gets one more stack (10 at most, +10% damage each,
// the helper lapses 2 sec after the last pulse), so the first pulse deals normal damage and the eleventh double.
class spell_blessings_unholy_death_and_decay_pulse : public SpellScript
{
    PrepareSpellScript(spell_blessings_unholy_death_and_decay_pulse);

    void HandleDamage(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (caster && target && caster->HasAura(SPELL_FAVOR_BLIGHTBEARER) && HasDisease(target, caster))
            SetHitDamage(GetHitDamage() + CalculatePct(GetHitDamage(), BLIGHTBEARER_PCT));
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        if (caster && caster->HasAura(SPELL_DEFILE))
            caster->CastSpell(caster, SPELL_DEFILE_HELPER, true);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_blessings_unholy_death_and_decay_pulse::HandleDamage, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
        AfterCast += SpellCastFn(spell_blessings_unholy_death_and_decay_pulse::HandleAfterCast);
    }
};

// Blightbearer: the Death Coil damage (47632, the enemy half of the coil) deals 15% more to diseased enemies.
class spell_blessings_unholy_blightbearer_coil : public SpellScript
{
    PrepareSpellScript(spell_blessings_unholy_blightbearer_coil);

    void HandleDamage(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (caster && target && caster->HasAura(SPELL_FAVOR_BLIGHTBEARER) && HasDisease(target, caster))
            SetHitDamage(GetHitDamage() + CalculatePct(GetHitDamage(), BLIGHTBEARER_PCT));
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_blessings_unholy_blightbearer_coil::HandleDamage, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// Gargoyle Sovereign, haste part. Bound to Summon Gargoyle (a second script beside the stock one): the Gargoyle gets the caster's haste (casting speed and melee speed) as a hidden aura.
class spell_blessings_unholy_summon_gargoyle : public SpellScript
{
    PrepareSpellScript(spell_blessings_unholy_summon_gargoyle);

    void HandleAfterCast()
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!player || !player->HasAura(SPELL_GARGOYLE_SOVEREIGN))
            return;

        float const castSpeed = player->GetFloatValue(UNIT_MOD_CAST_SPEED);           // 0.9 means 10% faster casts
        int32 const haste = castSpeed > 0.1f ? int32(100.0f / castSpeed - 100.0f + 0.5f) : 0;
        if (haste <= 0)
            return;

        int32 const amount = haste - 1;                                                  // base points: the aura shows base points + 1
        std::vector<Unit*> controlled(player->m_Controlled.begin(), player->m_Controlled.end());
        bool found = false;
        for (Unit* unit : controlled)
        {
            if (unit && unit->IsAlive() && unit->GetEntry() == MN_NPC_EBON_GARGOYLE)
            {
                unit->CastCustomSpell(unit, SPELL_GARGOYLE_HASTE, &amount, &amount, nullptr, true);
                found = true;
            }
        }

        if (!found)
            if (Creature* gargoyle = player->FindNearestCreature(MN_NPC_EBON_GARGOYLE, 40.0f))
                if (gargoyle->GetOwnerGUID() == player->GetGUID())
                    gargoyle->CastCustomSpell(gargoyle, SPELL_GARGOYLE_HASTE, &amount, &amount, nullptr, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_blessings_unholy_summon_gargoyle::HandleAfterCast);
    }
};

// All Will Serve. Bound to Raise Dead: two more Skeleton Archers for 20 sec (they shoot the owner's target; they are a gift of the cast, not pets).
class spell_blessings_unholy_raise_dead : public SpellScript
{
    PrepareSpellScript(spell_blessings_unholy_raise_dead);

    void HandleAfterCast()
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!player || !player->HasAura(SPELL_ALL_WILL_SERVE))
            return;

        for (uint32 i = 0; i < WILL_SERVE_COUNT; ++i)
            RaiseMinion(player, true, WILL_SERVE_LIFETIME_MS);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_blessings_unholy_raise_dead::HandleAfterCast);
    }
};

// Apocalypse. The passive's proc (Plague Strike, Scourge Strike, Plague Slash) is counted here; on the fifth the aura raises two ghouls beside the target.
class spell_blessings_unholy_apocalypse : public AuraScript
{
    PrepareAuraScript(spell_blessings_unholy_apocalypse);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        Player* player = GetTarget()->ToPlayer();
        Unit* target = eventInfo.GetActionTarget();
        if (!player || !target || !target->IsAlive() || ++_count < APOCALYPSE_EVERY)
            return;

        _count = 0;
        for (uint32 i = 0; i < APOCALYPSE_GHOULS; ++i)
            if (Creature* ghoul = RaiseMinion(player, false, APOCALYPSE_LIFETIME_MS, APOCALYPSE_CLAW_PCT, 0))
                PlaceBeside(ghoul, player, target, false);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_unholy_apocalypse::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }

private:
    uint32 _count = 0;
};

// Plague Sovereign, spreading. Bound to Blood Plague and Frost Fever: when the afflicted enemy dies the disease is cast on every enemy within 15 yards of it, from the same caster, at full duration.
class spell_blessings_unholy_disease_spread : public AuraScript
{
    PrepareAuraScript(spell_blessings_unholy_disease_spread);

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_DEATH)
            return;

        Unit* caster = GetCaster();
        Unit* victim = GetTarget();
        if (!caster || !caster->IsAlive() || !caster->HasAura(SPELL_PLAGUE_SOVEREIGN))
            return;

        for (Unit* other : EnemiesAround(caster, victim, SPREAD_RANGE))
            if (other != victim && other->IsAlive())
                caster->CastSpell(other, GetId(), true);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_unholy_disease_spread::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE, AURA_EFFECT_HANDLE_REAL);
    }
};

// Favor of the Plague Reaper: effect 1 (a dummy) teaches Plague Rot and takes it away again.
class spell_blessings_unholy_plague_reaper : public AuraScript
{
    PrepareAuraScript(spell_blessings_unholy_plague_reaper);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Player* player = GetTarget()->ToPlayer())
            player->learnSpell(SPELL_PLAGUE_ROT);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player || !player->GetSession() || player->GetSession()->PlayerLogout())
            return;

        player->removeSpell(SPELL_PLAGUE_ROT, SPEC_MASK_ALL, false);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_unholy_plague_reaper::HandleApply, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_unholy_plague_reaper::HandleRemove, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Plague Rot (effect 0: 45% of weapon damage as Shadow damage, once). It can only be cast on a target with Festering Rot; the hit is multiplied by the stacks, half of it goes to every other enemy within
// 8 yards of the target (the hidden splash bolt), and the stacks are removed.
class spell_blessings_unholy_plague_rot : public SpellScript
{
    PrepareSpellScript(spell_blessings_unholy_plague_rot);

    SpellCastResult CheckStacks()
    {
        Unit* caster = GetCaster();
        Unit* target = GetExplTargetUnit();
        if (!caster || !target || !target->HasAura(SPELL_FESTERING_ROT, caster->GetGUID()))
            return SPELL_FAILED_CANT_DO_THAT_RIGHT_NOW;

        return SPELL_CAST_OK;
    }

    void HandleHit()
    {
        if (GetHitDamage() <= 0)                     // OnHit also runs for a miss, dodge, parry or resist: then nothing is spent
            return;

        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        Aura* rot = caster && target ? target->GetAura(SPELL_FESTERING_ROT, caster->GetGUID()) : nullptr;
        if (!rot)
        {
            SetHitDamage(0);
            return;
        }

        int32 const stacks = rot->GetStackAmount();
        int32 const damage = int32(GetHitDamage()) * stacks;
        SetHitDamage(damage);

        int32 const splash = std::max(damage / 2 - 1, 0);                              // base points: the bolt deals base points + 1
        for (Unit* other : EnemiesAround(caster, target, ROT_SPLASH_RANGE))
            if (other != target)
                caster->CastCustomSpell(other, SPELL_PLAGUE_ROT_SPLASH, &splash, nullptr, nullptr, true);

        target->RemoveAurasDueToSpell(SPELL_FESTERING_ROT, caster->GetGUID());
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_blessings_unholy_plague_rot::CheckStacks);
        OnHit += SpellHitFn(spell_blessings_unholy_plague_rot::HandleHit);
    }
};

// Plague Slash: 30% chance to apply Blood Plague to the target it hit.
class spell_blessings_unholy_plague_slash : public SpellScript
{
    PrepareSpellScript(spell_blessings_unholy_plague_slash);

    void HandleHit()
    {
        if (GetHitDamage() <= 0)                     // OnHit also runs for a miss
            return;

        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (caster && target && target->IsAlive() && roll_chance_i(SLASH_PLAGUE_CHANCE))
            caster->CastSpell(target, SPELL_BLOOD_PLAGUE, true);
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_blessings_unholy_plague_slash::HandleHit);
    }
};

// The Necromancer's Mastery addition: the Necrosis of the Mastery (a share of the auto attack again as Shadow damage) only names melee swings, so a Plague Slash gets its own copy. Effect 0 is a dummy
// whose amount is the percent (20, the five ranks of Necrosis); the damage is the stock Necrosis damage spell.
class spell_blessings_unholy_slash_necrosis : public AuraScript
{
    PrepareAuraScript(spell_blessings_unholy_slash_necrosis);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        return damageInfo && damageInfo->GetDamage();
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        Unit* target = eventInfo.GetActionTarget();
        if (!target)
            return;

        int32 const bp = CalculatePct(int32(eventInfo.GetDamageInfo()->GetDamage()), aurEff->GetAmount());
        GetTarget()->CastCustomSpell(SPELL_NECROSIS_DAMAGE, SPELLVALUE_BASE_POINT0, bp, target, true, nullptr, aurEff);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_blessings_unholy_slash_necrosis::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_blessings_unholy_slash_necrosis::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Favor of the Tomb Necromancer. Effect 1 (dummy): teaches Plague Slash and Corpse Toss, takes them (and the archer) away again. Effect 2 (periodic dummy, every second): the ghoul of Raise Dead becomes
// the Skeleton Archer (the stock ghoul, re-skinned and marked; it follows, never melees, and shoots).
class spell_blessings_unholy_tomb_necromancer : public AuraScript
{
    PrepareAuraScript(spell_blessings_unholy_tomb_necromancer);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Player* player = GetTarget()->ToPlayer())
        {
            player->learnSpell(SPELL_PLAGUE_SLASH);
            player->learnSpell(SPELL_CORPSE_TOSS);
        }
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player || !player->GetSession() || player->GetSession()->PlayerLogout())
            return;

        player->removeSpell(SPELL_PLAGUE_SLASH, SPEC_MASK_ALL, false);
        player->removeSpell(SPELL_CORPSE_TOSS, SPEC_MASK_ALL, false);
        player->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
        Pet* pet = player->GetPet();
        if (pet && pet->HasAura(SPELL_MARK_ARCHER))
            UnmakeArcher(pet);                                      // the Skeleton Archer becomes the plain ghoul again
    }

    void HandleTick(AuraEffect const* /*aurEff*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player || !player->IsAlive())
            return;

        Pet* pet = player->GetPet();
        if (!pet || !pet->IsAlive() || pet->GetEntry() != MN_NPC_RISEN_GHOUL)
            return;

        MakeArcher(pet);
        if (CharmInfo* charmInfo = pet->GetCharmInfo())
            charmInfo->SetCommandState(COMMAND_FOLLOW);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_unholy_tomb_necromancer::HandleApply, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_unholy_tomb_necromancer::HandleRemove, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_unholy_tomb_necromancer::HandleTick, EFFECT_2, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Corpse Toss (effect 0 is a script effect on the enemy). Needs a corpse within 20 yards or 20 runic power. A raised ghoul appears at the caster, leaps beside the target at once and claws it 4 times for 60% of
// weapon damage each (the striker mark does the clawing), and falls apart after 8 sec.
class spell_blessings_unholy_corpse_toss : public SpellScript
{
    PrepareSpellScript(spell_blessings_unholy_corpse_toss);

    SpellCastResult CheckResource()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return SPELL_FAILED_ERROR;

        if (FindCorpse(caster, CORPSE_RANGE))
            return SPELL_CAST_OK;

        return int32(caster->GetPower(POWER_RUNIC_POWER)) >= CORPSE_TOSS_RUNIC_POWER ? SPELL_CAST_OK : SPELL_FAILED_NO_POWER;
    }

    void HandleToss(SpellEffIndex /*effIndex*/)
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        Unit* target = GetHitUnit();
        if (!player || !target)
            return;

        if (Creature* corpse = FindCorpse(player, CORPSE_RANGE))
            corpse->RemoveCorpse();                  // the corpse is used up (its loot goes with it)
        else
        {
            if (int32(player->GetPower(POWER_RUNIC_POWER)) < CORPSE_TOSS_RUNIC_POWER)
                return;

            player->ModifyPower(POWER_RUNIC_POWER, -CORPSE_TOSS_RUNIC_POWER);
        }

        if (Creature* ghoul = RaiseMinion(player, false, LEAP_LIFETIME_MS, LEAP_CLAW_PCT, LEAP_CLAWS))
            PlaceBeside(ghoul, player, target, true);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_blessings_unholy_corpse_toss::CheckResource);
        OnEffectHitTarget += SpellEffectFn(spell_blessings_unholy_corpse_toss::HandleToss, EFFECT_0, SPELL_EFFECT_SCRIPT_EFFECT);
    }
};

// The mark of a Skeleton Archer (periodic dummy every 500 ms, effect 1 disarms it): every 2 sec (scaled by the archer's attack speed, so Ghoul Frenzy speeds it up) it shoots the owner's target for
// 35% of the owner's attack power, if the target is within 35 yards. The archer leaves when its owner is gone.
class spell_blessings_unholy_archer_mark : public AuraScript
{
    PrepareAuraScript(spell_blessings_unholy_archer_mark);

    void HandleTick(AuraEffect const* /*aurEff*/)
    {
        Creature* archer = GetTarget()->ToCreature();
        if (!archer || !archer->IsAlive())
            return;

        Unit* owner = archer->GetOwner();
        Player* player = owner ? owner->ToPlayer() : nullptr;
        if (!player || !player->IsAlive())
        {
            if (TempSummon* summon = archer->ToTempSummon())
                if (!archer->IsPet())
                    summon->UnSummon();

            return;
        }

        if (archer->IsPet() && !player->HasAura(SPELL_FAVOR_NECROMANCER))      // a pet saved as an archer, loaded after the Favor was dropped
        {
            UnmakeArcher(archer);
            return;
        }

        uint32 const interval = uint32(float(ARCHER_SHOT_MS) * std::max(archer->m_modAttackSpeedPct[BASE_ATTACK], 0.2f));
        _elapsed = std::min(_elapsed + MARK_TICK_MS, interval);
        if (_elapsed < interval)
            return;

        Unit* target = FightTarget(player);
        if (!target || archer->GetDistance(target) > ARCHER_RANGE || !archer->IsWithinLOSInMap(target))
            return;

        _elapsed = 0;
        archer->SetFacingToObject(target);
        int32 const shot = std::max(int32(CalculatePct(player->GetTotalAttackPowerValue(BASE_ATTACK), ARCHER_SHOT_PCT)) - 1, 0);   // base points: the bolt deals base points + 1
        archer->CastCustomSpell(target, SPELL_SKELETON_SHOT, &shot, nullptr, nullptr, true);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_unholy_archer_mark::HandleTick, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }

private:
    uint32 _elapsed = 0;
};

// The mark of a raised ghoul (periodic dummy every 500 ms; effect 0's amount is the claw damage in percent of the owner's weapon damage, effect 1's amount the most claws, 0 for no limit): it claws the
// owner's target now and then. The owner deals the damage (a hidden weapon percent spell cast from the owner), the ghoul only plays the blow.
class spell_blessings_unholy_striker_mark : public AuraScript
{
    PrepareAuraScript(spell_blessings_unholy_striker_mark);

    void HandleTick(AuraEffect const* aurEff)
    {
        Creature* ghoul = GetTarget()->ToCreature();
        if (!ghoul || !ghoul->IsAlive())
            return;

        Unit* owner = ghoul->GetOwner();
        Player* player = owner ? owner->ToPlayer() : nullptr;
        if (!player || !player->IsAlive())
        {
            if (TempSummon* summon = ghoul->ToTempSummon())
                summon->UnSummon();

            return;
        }

        uint32 const most = uint32(std::max(Param(ghoul, SPELL_MARK_STRIKER, EFFECT_1), 0));
        if (most && _claws >= most)
            return;

        uint32 const interval = most ? LEAP_CLAW_MS : APOCALYPSE_CLAW_MS;
        _elapsed = std::min(_elapsed + MARK_TICK_MS, interval);
        if (_elapsed < interval)
            return;

        Unit* target = FightTarget(player);
        if (!target || ghoul->GetDistance(target) > 40.0f)
            return;

        _elapsed = 0;
        ++_claws;
        ghoul->SetFacingToObject(target);
        ghoul->HandleEmoteCommand(EMOTE_ONESHOT_ATTACK_UNARMED);
        int32 const claw = std::max(aurEff->GetAmount(), 1) - 1;                          // base points: the hit is base points + 1 percent of weapon damage
        player->CastCustomSpell(target, SPELL_RAISED_CLAW, &claw, nullptr, nullptr, true);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_unholy_striker_mark::HandleTick, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }

private:
    uint32 _elapsed = 0;
    uint32 _claws = 0;
};

void AddMoonlitBlessingsUnholyScripts()
{
    RegisterSpellScript(spell_blessings_unholy_epidemic_pestilence);
    RegisterSpellScript(spell_blessings_unholy_outbreak_blood_boil);
    RegisterSpellScript(spell_blessings_unholy_defile_start);
    RegisterSpellScript(spell_blessings_unholy_death_and_decay_pulse);
    RegisterSpellScript(spell_blessings_unholy_blightbearer_coil);
    RegisterSpellScript(spell_blessings_unholy_summon_gargoyle);
    RegisterSpellScript(spell_blessings_unholy_raise_dead);
    RegisterSpellScript(spell_blessings_unholy_apocalypse);
    RegisterSpellScript(spell_blessings_unholy_disease_spread);
    RegisterSpellScript(spell_blessings_unholy_plague_reaper);
    RegisterSpellScript(spell_blessings_unholy_plague_rot);
    RegisterSpellScript(spell_blessings_unholy_plague_slash);
    RegisterSpellScript(spell_blessings_unholy_slash_necrosis);
    RegisterSpellScript(spell_blessings_unholy_tomb_necromancer);
    RegisterSpellScript(spell_blessings_unholy_corpse_toss);
    RegisterSpellScript(spell_blessings_unholy_archer_mark);
    RegisterSpellScript(spell_blessings_unholy_striker_mark);
}
