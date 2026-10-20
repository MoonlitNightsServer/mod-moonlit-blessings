// Moonlit Blessings: C++ spell scripts for the Marksmanship Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/marksmanship.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_marksman_smoldering_coals  Boon: a critical Explosive Shot blast spreads the hunter's Serpent Sting to the nearest enemy that has none of her Stings
//   spell_blessings_marksman_cinderfall        Boon: bound to the stock Explosive Trap Effect ranks; an enemy that leaves the ground fire takes half of the burn it had left at once
//   spell_blessings_marksman_quick_nock        Boon: a critical Steady Shot makes the next Auto Shot ready at once
//   spell_blessings_marksman_dead_aim          Boon: Aimed Shot / Chimera Shot fired at an enemy below 35% health get a one-charge guaranteed critical strike buff
//   spell_blessings_marksman_lightning_draw    Boon: while Rapid Fire is on, casting does not delay the Auto Shot, slows to attack speed are cancelled, every fifth Auto Shot adds a free Arcane Shot
//   spell_blessings_marksman_quickdraw_decay   Favor of Quickdraw: the haste buff loses one stack every 2 sec (the last stack ends with the buff)
//   spell_blessings_marksman_wildfire_patch    Favor of Wildfire: a trap going off leaves a burning patch where it hit
//   MoonlitMarksmanshipUnitScript              UnitScript with two hooks:
//                                                Dead Aim        Aimed Shot / Chimera Shot deal more to an enemy marked by the hunter's Hunter's Mark
//                                                Pyre of the Hunt a lethal blow on an enemy that carries the hunter's Serpent Sting or Explosive Shot makes it burst
//
// Spell ids come from the Marksmanship registry block 1,320,000 (the units are listed at the top of tools/talents/recipes/marksmanship.py, which asserts the ids named here). The amounts,
// ranges and helper spells of the Boons and Favors are read from the effects of their own passives, so the spell data stays the one place a number is written. Every script first checks
// that the hunter really has the Boon or Favor aura. No script uses ammunition: the free Arcane Shot is cast triggered like any other shot (ammunition is the business of mod-moonlit-ammo).
// ~Moonlit Nights Team

#include "Cell.h"
#include "CellImpl.h"
#include "DynamicObject.h"
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
#include "Timer.h"
#include "UnitScript.h"

#include <algorithm>
#include <chrono>
#include <list>
#include <utility>
#include <vector>

namespace
{
    enum MarksmanshipSpells : uint32
    {
        SPELL_CINDERFALL      = 1320010,   // effect 1: amount = percent of the remaining burn dealt at once, trigger spell = that hit
        SPELL_DEAD_AIM        = 1320030,   // effect 0: misc = health percent, trigger spell = the crit buff (the proc script uses it); effect 1: amount = percent more damage to a marked target
        SPELL_PYRE_OF_THE_HUNT = 1320050,  // effect 0: amount = percent of the Sting / Shot damage, misc = yards, trigger spell = the burst

        SPELL_RAPID_FIRE      = 3045,      // stock Rapid Fire, the only player rank
        SPELL_ARCANE_SHOT_RANK_1 = 3044,   // head of the Arcane Shot rank chain
        SPELL_EXPLOSIVE_SHOT_BLAST = 53352 // stock: the blast the Explosive Shot aura casts every second
    };

    constexpr uint32 FLAG_SERPENT_STING = 0x4000;                 // SpellFamilyFlags[0]
    constexpr uint32 FLAG_AIMED_SHOT = 0x20000;                   // [0]
    constexpr uint32 FLAG_CHIMERA_SHOT = 0x1;                     // [2]
    constexpr uint32 FLAG_HUNTERS_MARK = 0x400;                   // [0]
    constexpr uint32 FLAG_EXPLOSIVE_SHOT = 0x80000000;            // [1]: the Explosive Shot cast and the periodic dummy aura it leaves
    constexpr uint32 PYRE_MAX_TICKS = 10;                         // Pyre of the Hunt counts at most this many ticks of the Sting as damage done
    constexpr uint32 PYRE_MAX_TARGETS = 10;                       // enemies one burst can hit
    constexpr uint32 EVENT_DELAY_MS = 1;                          // work that must not run inside the damage code that decided on it waits one millisecond of game time
    constexpr uint8 FIFTH_SHOT_STACKS = 4;                        // Lightning Draw: the counter holds four stacks when the fifth Auto Shot lands

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

    // The effects of a spell that are auras (what a copy of the spell applied with AddAura should carry).
    uint8 AuraEffectMask(SpellInfo const* info)
    {
        uint8 mask = 0;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (info->Effects[i].IsAura())
                mask |= uint8(1 << i);

        return mask;
    }

    // Hit `target` as `caster` with one of the flat-hit helper spells (instant, no spell power, cannot critically hit): the damage is exactly `amount`.
    void CastFlat(Unit* caster, Unit* target, uint32 spellId, int32 amount)
    {
        if (!caster || !target || amount <= 0 || !target->IsAlive())
            return;

        caster->CastCustomSpell(spellId, SPELLVALUE_BASE_POINT0, amount - 1, target, true);     // the spell adds 1 to its base points
    }

    // Does `unit` carry any Sting (Serpent, Viper, Scorpid, Wyvern) cast by `casterGuid`? Only one Sting per hunter can be on a target, so a spread must not replace another one.
    bool HasStingOf(Unit const* unit, ObjectGuid casterGuid)
    {
        for (auto const& pair : unit->GetAppliedAuras())
        {
            Aura const* aura = pair.second->GetBase();
            if (aura->GetCasterGUID() == casterGuid && aura->GetSpellInfo()->SpellFamilyName == SPELLFAMILY_HUNTER && aura->GetSpellInfo()->GetSpellSpecific() == SPELL_SPECIFIC_STING)
                return true;
        }

        return false;
    }

    // The highest rank of the spell chain that starts at `firstRank` which the player knows; 0 when none.
    uint32 KnownRank(Player const* player, uint32 firstRank)
    {
        for (uint32 id = sSpellMgr->GetLastSpellInChain(firstRank); id; id = sSpellMgr->GetPrevSpellInChain(id))
            if (player->HasSpell(id))
                return id;

        return 0;
    }

    // The slow effects on a hunter's attack speed (negative amounts of hostile haste auras) are set to zero. A cancelled slow stays cancelled until its aura is refreshed.
    void CancelAttackSpeedSlows(Player* player)
    {
        static AuraType const types[] = { SPELL_AURA_MOD_RANGED_HASTE, SPELL_AURA_MOD_MELEE_RANGED_HASTE, SPELL_AURA_MOD_ATTACKSPEED };
        for (AuraType type : types)
        {
            Unit::AuraEffectList const effects = player->GetAuraEffectsByType(type);       // a copy: changing an amount re-applies the effect, which edits the live list
            for (AuraEffect* aurEff : effects)
                if (aurEff->GetAmount() < 0 && aurEff->GetCasterGUID() != player->GetGUID() && !aurEff->GetSpellInfo()->IsPositive())
                    aurEff->ChangeAmount(0);
        }
    }

    // Pyre of the Hunt, after the death: the burst on every enemy around the corpse.
    void PyreBurst(Unit* corpse, ObjectGuid playerGuid, uint32 burstSpell, float radius, int32 burst)
    {
        if (corpse->IsAlive() || !corpse->IsInWorld())
            return;

        Player* player = ObjectAccessor::GetPlayer(*corpse, playerGuid);
        if (!player || !player->IsInWorld() || !player->IsAlive())
            return;

        uint32 hit = 0;
        for (Unit* unit : EnemiesAround(player, corpse, radius))
        {
            if (unit == corpse || !unit->IsAlive())
                continue;

            CastFlat(player, unit, burstSpell, burst);
            if (++hit >= PYRE_MAX_TARGETS)
                break;
        }
    }
}

// Smoldering Coals: effect 2's misc value is the spread range in yards. The proc is a critical blast of Explosive Shot.
class spell_blessings_marksman_smoldering_coals : public AuraScript
{
    PrepareAuraScript(spell_blessings_marksman_smoldering_coals);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        if (!victim || !victim->IsAlive())
            return;

        AuraEffect const* sting = victim->GetAuraEffect(SPELL_AURA_PERIODIC_DAMAGE, SPELLFAMILY_HUNTER, FLAG_SERPENT_STING, 0, 0, caster->GetGUID());
        if (!sting)
            return;

        float const range = float(GetSpellInfo()->Effects[EFFECT_2].MiscValue);
        Unit* best = nullptr;
        float bestDistance = range + 1.0f;
        for (Unit* unit : EnemiesAround(caster, victim, range))
        {
            if (unit == victim || !unit->IsAlive() || HasStingOf(unit, caster->GetGUID()))
                continue;

            float const distance = victim->GetDistance(unit);
            if (distance < bestDistance)
            {
                best = unit;
                bestDistance = distance;
            }
        }

        if (best)
        {
            SpellInfo const* info = sting->GetSpellInfo();
            caster->AddAura(info, AuraEffectMask(info), best);
        }
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_marksman_smoldering_coals::HandleProc, EFFECT_2, SPELL_AURA_DUMMY);
    }
};

// Cinderfall: bound to the stock Explosive Trap Effect ranks (their second effect is the persistent ground fire). When the enemy's application is taken away by leaving the fire
// (removal by default; expiry, death and dispels are other modes), the hunter's Cinderfall hits it for a share of the burn it still had coming.
class spell_blessings_marksman_cinderfall : public AuraScript
{
    PrepareAuraScript(spell_blessings_marksman_cinderfall);

    void HandleRemove(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        AuraApplication const* app = GetTargetApplication();
        if (!app || app->GetRemoveMode() != AURA_REMOVE_BY_DEFAULT)
            return;

        Unit* target = GetTarget();
        Unit* hunter = GetCaster();
        if (!hunter || !hunter->IsPlayer() || !target || !target->IsAlive() || target == hunter || aurEff->GetAmplitude() <= 0 || GetDuration() <= 0)
            return;

        AuraEffect const* cinderfall = hunter->GetAuraEffect(SPELL_CINDERFALL, EFFECT_1);
        if (!cinderfall)
            return;

        // Only an enemy that really stepped out of the fire counts: when the whole fire is taken away (a new Explosive Trap replaces it) the enemy is still inside it.
        Aura const* fire = GetAura();
        if (!fire || fire->GetType() != DYNOBJ_AURA_TYPE)
            return;

        DynamicObject const* ground = fire->GetDynobjOwner();
        if (!ground || target->IsWithinDist(ground, ground->GetRadius() + 0.5f))
            return;

        uint32 perTick = hunter->SpellDamageBonusDone(target, GetSpellInfo(), uint32(std::max(aurEff->GetAmount(), 0)), DOT, aurEff->GetEffIndex());
        perTick = target->SpellDamageBonusTaken(hunter, GetSpellInfo(), perTick, DOT);
        int64 const remaining = int64(perTick) * int64(GetDuration() / aurEff->GetAmplitude());
        int32 const burst = int32(std::min<int64>(remaining * int64(cinderfall->GetAmount()) / 100, 2000000000));
        CastFlat(hunter, target, cinderfall->GetSpellInfo()->Effects[EFFECT_1].TriggerSpell, burst);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_marksman_cinderfall::HandleRemove, EFFECT_1, SPELL_AURA_PERIODIC_DAMAGE, AURA_EFFECT_HANDLE_REAL);
    }
};

// Quick Nock: a critical Steady Shot (the proc is in the second effect) readies the Auto Shot. The ranged swing timer is set to zero; the core fires the shot on its next update.
class spell_blessings_marksman_quick_nock : public AuraScript
{
    PrepareAuraScript(spell_blessings_marksman_quick_nock);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        if (Player* player = GetTarget()->ToPlayer())
            player->setAttackTimer(RANGED_ATTACK, 0);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_marksman_quick_nock::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

// Dead Aim: the proc is the CAST of Aimed Shot or Chimera Shot. Effect 0's misc value is the health percent, its trigger spell the one-charge buff that makes the shot a certain critical
// strike (the buff is in place before the shot's damage is worked out). A buff left over from a shot that never landed is taken off first.
class spell_blessings_marksman_dead_aim : public AuraScript
{
    PrepareAuraScript(spell_blessings_marksman_dead_aim);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        uint32 const buffSpell = GetSpellInfo()->Effects[EFFECT_0].TriggerSpell;
        caster->RemoveAurasDueToSpell(buffSpell);

        Spell const* spell = eventInfo.GetProcSpell();
        Unit* victim = spell ? spell->m_targets.GetUnitTarget() : eventInfo.GetActionTarget();
        if (!victim || !victim->IsAlive() || victim == caster || victim->IsFriendlyTo(caster))
            return;

        if (victim->HealthBelowPct(GetSpellInfo()->Effects[EFFECT_0].MiscValue))
            caster->CastSpell(caster, buffSpell, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_marksman_dead_aim::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Lightning Draw. Effect 0 (dummy, proc on Auto Shot hits): the trigger spell is the counter; the fifth Auto Shot during Rapid Fire spends it and adds a free Arcane Shot. Effect 1
// (periodic dummy, 0.5 sec): its trigger spell is the steady hand buff (casting does not delay the Auto Shot); both buffs and the slow cancelling live only while Rapid Fire does.
class spell_blessings_marksman_lightning_draw : public AuraScript
{
    PrepareAuraScript(spell_blessings_marksman_lightning_draw);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Player* player = GetTarget()->ToPlayer();
        if (!player || !player->HasAura(SPELL_RAPID_FIRE))
            return;

        uint32 const counter = GetSpellInfo()->Effects[EFFECT_0].TriggerSpell;
        Aura const* count = player->GetAura(counter);
        if (!count || count->GetStackAmount() < FIFTH_SHOT_STACKS)
        {
            player->CastSpell(player, counter, true);
            return;
        }

        player->RemoveAurasDueToSpell(counter);
        Unit* victim = eventInfo.GetActionTarget();
        if (!victim || !victim->IsAlive() || !player->IsValidAttackTarget(victim))
            return;

        if (uint32 const arcane = KnownRank(player, SPELL_ARCANE_SHOT_RANK_1))
            player->CastSpell(victim, arcane, true);
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player || !player->IsAlive())
            return;

        uint32 const steadyHand = GetSpellInfo()->Effects[EFFECT_1].TriggerSpell;
        if (player->HasAura(SPELL_RAPID_FIRE))
        {
            if (!player->HasAura(steadyHand))
                player->CastSpell(player, steadyHand, true);

            CancelAttackSpeedSlows(player);
        }
        else
        {
            player->RemoveAurasDueToSpell(steadyHand);
            player->RemoveAurasDueToSpell(GetSpellInfo()->Effects[EFFECT_0].TriggerSpell);
        }
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_marksman_lightning_draw::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_marksman_lightning_draw::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Favor of Quickdraw: bound to the haste buff itself. Every 2 sec one stack falls off (the last one is left to the buff's own 10 sec, which every shot renews), and a stack added by a
// shot renews the buff without touching this timer.
class spell_blessings_marksman_quickdraw_decay : public AuraScript
{
    PrepareAuraScript(spell_blessings_marksman_quickdraw_decay);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        if (Aura* aura = GetAura())
            if (aura->GetStackAmount() > 1)
                aura->ModStackAmount(-1);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_marksman_quickdraw_decay::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Wildfire Patch: the second hidden aura of Favor of Wildfire. Procs when the Immolation Trap or the Explosive Trap goes off (the activation flag). Amount = percent of one tick of that trap's
// burn that the patch deals every tick, trigger spell = the patch (a ground fire cloned from the Explosive Trap Effect, its damage is effect 2's base points).
class spell_blessings_marksman_wildfire_patch : public AuraScript
{
    PrepareAuraScript(spell_blessings_marksman_wildfire_patch);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        SpellInfo const* trap = eventInfo.GetSpellInfo();
        if (!caster->IsPlayer() || !victim || !trap)
            return;

        // the trap's burn is the periodic damage effect of the spell: the first effect for the Immolation Trap, the second for the Explosive Trap
        uint8 index = EFFECT_0;
        if (trap->Effects[EFFECT_0].ApplyAuraName != SPELL_AURA_PERIODIC_DAMAGE)
            index = EFFECT_1;
        if (trap->Effects[index].ApplyAuraName != SPELL_AURA_PERIODIC_DAMAGE)
            return;

        uint32 tick = caster->SpellDamageBonusDone(victim, trap, uint32(std::max(trap->Effects[index].CalcValue(caster), 0)), DOT, index);
        tick = victim->SpellDamageBonusTaken(caster, trap, tick, DOT);
        int32 const patchTick = int32(std::min<uint32>(CalculatePct(tick, aurEff->GetAmount()), 2000000000u));
        SpellInfo const* patch = sSpellMgr->GetSpellInfo(GetSpellInfo()->Effects[EFFECT_0].TriggerSpell);
        if (!patch || patchTick <= 0)
            return;

        SpellCastTargets targets;
        targets.SetDst(victim->GetPositionX(), victim->GetPositionY(), victim->GetPositionZ(), victim->GetOrientation());
        CustomSpellValues values;
        values.AddSpellMod(SPELLVALUE_BASE_POINT1, patchTick - 1);          // the patch's periodic damage is its second effect; the spell adds 1 to its base points
        caster->CastSpell(targets, patch, &values, TRIGGERED_FULL_MASK);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_marksman_wildfire_patch::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

class MoonlitMarksmanshipUnitScript : public UnitScript
{
public:
    MoonlitMarksmanshipUnitScript() : UnitScript("MoonlitMarksmanshipUnitScript", true, { UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN, UNITHOOK_ON_DAMAGE }) { }

    // Dead Aim: the damage of Aimed Shot and Chimera Shot (before the hit lands) against an enemy that carries the hunter's own Hunter's Mark.
    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo) override
    {
        if (!attacker || !attacker->IsPlayer() || !target || !spellInfo || damage <= 0 || spellInfo->SpellFamilyName != SPELLFAMILY_HUNTER)
            return;

        if (!(spellInfo->SpellFamilyFlags[0] & FLAG_AIMED_SHOT) && !(spellInfo->SpellFamilyFlags[2] & FLAG_CHIMERA_SHOT))
            return;

        Player* player = attacker->ToPlayer();
        AuraEffect const* deadAim = player->GetAuraEffect(SPELL_DEAD_AIM, EFFECT_1);
        if (deadAim && target != player && target->GetAuraEffect(SPELL_AURA_MOD_STALKED, SPELLFAMILY_HUNTER, FLAG_HUNTERS_MARK, 0, 0, player->GetGUID()))
            damage += CalculatePct(damage, deadAim->GetAmount());
    }

    // Pyre of the Hunt: this runs inside Unit::DealDamage after the absorbs, before the death takes the auras away (OnUnitDeath comes after they are gone). Everything the burst needs is read
    // here; it goes off one millisecond later and only if the enemy really died. The damage the Sting and the Shot "dealt" is estimated: the damage per tick (spell power and attack
    // power already included for the Sting, worked out for the Explosive Shot blast) times the ticks so far.
    void OnDamage(Unit* /*attacker*/, Unit* victim, uint32& damage) override
    {
        if (!victim || !damage || damage < victim->GetHealth())
            return;

        std::vector<std::pair<ObjectGuid, int64>> done;            // hunter -> estimated damage of her Sting and Shot on the victim
        auto add = [&done](Player* hunter, int64 amount)
        {
            for (auto& entry : done)
                if (entry.first == hunter->GetGUID())
                {
                    entry.second += amount;
                    return;
                }

            done.emplace_back(hunter->GetGUID(), amount);
        };

        for (AuraEffect const* sting : victim->GetAuraEffectsByType(SPELL_AURA_PERIODIC_DAMAGE))
        {
            SpellInfo const* info = sting->GetSpellInfo();
            if (info->SpellFamilyName != SPELLFAMILY_HUNTER || !(info->SpellFamilyFlags[0] & FLAG_SERPENT_STING))
                continue;

            Unit* caster = sting->GetCaster();
            if (caster && caster->IsPlayer() && caster->IsInWorld() && caster->GetAuraEffect(SPELL_PYRE_OF_THE_HUNT, EFFECT_0))
                add(caster->ToPlayer(), int64(std::max(sting->GetAmount(), 0)) * int64(std::min(sting->GetTickNumber(), PYRE_MAX_TICKS)));
        }

        for (AuraEffect const* shot : victim->GetAuraEffectsByType(SPELL_AURA_PERIODIC_DUMMY))
        {
            SpellInfo const* info = shot->GetSpellInfo();
            if (info->SpellFamilyName != SPELLFAMILY_HUNTER || !(info->SpellFamilyFlags[1] & FLAG_EXPLOSIVE_SHOT))
                continue;

            Unit* caster = shot->GetCaster();
            SpellInfo const* blast = sSpellMgr->GetSpellInfo(SPELL_EXPLOSIVE_SHOT_BLAST);
            if (caster && blast && caster->IsPlayer() && caster->IsInWorld() && caster->GetAuraEffect(SPELL_PYRE_OF_THE_HUNT, EFFECT_0))
            {
                uint32 const perBlast = caster->SpellDamageBonusDone(victim, blast, uint32(std::max(shot->GetAmount(), 0)), SPELL_DIRECT_DAMAGE, EFFECT_0);
                add(caster->ToPlayer(), int64(perBlast) * int64(std::min(shot->GetTickNumber(), PYRE_MAX_TICKS)));
            }
        }

        for (auto const& entry : done)
        {
            Player* hunter = ObjectAccessor::GetPlayer(*victim, entry.first);
            AuraEffect const* pyre = hunter ? hunter->GetAuraEffect(SPELL_PYRE_OF_THE_HUNT, EFFECT_0) : nullptr;
            if (!pyre || victim == hunter)
                continue;

            int32 const burst = int32(std::min<int64>(entry.second * int64(pyre->GetAmount()) / 100, 2000000000));
            if (burst <= 0)
                continue;

            uint32 const burstSpell = pyre->GetSpellInfo()->Effects[EFFECT_0].TriggerSpell;
            float const radius = float(pyre->GetSpellInfo()->Effects[EFFECT_0].MiscValue);
            ObjectGuid const hunterGuid = hunter->GetGUID();
            Unit* corpse = victim;
            corpse->m_Events.AddEventAtOffset([corpse, hunterGuid, burstSpell, radius, burst]()
            {
                PyreBurst(corpse, hunterGuid, burstSpell, radius, burst);
            }, std::chrono::milliseconds(EVENT_DELAY_MS));
        }
    }
};

void AddMoonlitBlessingsMarksmanshipScripts()
{
    RegisterSpellScript(spell_blessings_marksman_smoldering_coals);
    RegisterSpellScript(spell_blessings_marksman_cinderfall);
    RegisterSpellScript(spell_blessings_marksman_quick_nock);
    RegisterSpellScript(spell_blessings_marksman_dead_aim);
    RegisterSpellScript(spell_blessings_marksman_lightning_draw);
    RegisterSpellScript(spell_blessings_marksman_quickdraw_decay);
    RegisterSpellScript(spell_blessings_marksman_wildfire_patch);
    new MoonlitMarksmanshipUnitScript();
}
