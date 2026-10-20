// Moonlit Blessings: C++ spell scripts for the Inquisitor Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/inquisitor.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_inquisitor_censure        the Censure kit spell: cuts the stacks back to the limit (5, or 8 with Favor of the Censure) and runs Spreading Censure
//   spell_blessings_inquisitor_condemn        the Condemn kit spell: 20% more damage for every Censure stack on the target
//   spell_blessings_inquisitor_sunfire_burst  Boon: a critical Exorcism bursts on the enemies around its target
//   spell_blessings_inquisitor_dawn_hammer    Boon: Holy Wrath echoes on every enemy it hit and refreshes Consecration
//   spell_blessings_inquisitor_final_verdict  Boon: a Judgement cast at 5 stacks of Censure consumes them and is a guaranteed critical hit
//   MoonlitInquisitorUnitScript               UnitScript with three hooks:
//                                               Final Sentence    damage-over-time ticks deal more to enemies close to death
//                                               Wrathful Radiance Consecration ticks deal more while the paladin stands in it
//                                               Burning Verdict   Judgement damage is higher on an enemy that carries the paladin's Censure
//                                               Funeral Pyre      a lethal blow on an enemy that carries Censure makes it erupt
//
// Spell ids come from the Inquisitor registry block 1,540,100 and the Paladin kit block 1,540,000 (the units are listed at the top of tools/talents/recipes/inquisitor.py, which asserts
// the ids named here). The amounts, ranges and helper spells of the Boons are read from the effects of their own passives, so the spell data stays the one place a number is written.
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

namespace
{
    enum InquisitorSpells : uint32
    {
        SPELL_CENSURE           = 1540020,   // kit: the brand (effect 0: the tick; stacks up to 8 in the data, the script holds it at the limit)
        SPELL_FINAL_SENTENCE    = 1540120,   // effect 0: amount = percent more damage, misc = health percent below which it applies
        SPELL_SPREADING_CENSURE = 1540160,   // effect 0: amount = cooldown in seconds, misc = yards
        SPELL_WRATHFUL_RADIANCE = 1540170,   // effect 0: amount = percent more Consecration damage
        SPELL_BURNING_VERDICT   = 1540180,   // effect 1: amount = percent more Judgement damage on an enemy with Censure
        SPELL_FUNERAL_PYRE      = 1540210,   // effect 0: amount = percent per stack, misc = yards, trigger spell = the burst
        SPELL_FAVOR_CENSURE     = 1540220    // effect 1: amount = the stack limit of Censure
    };

    constexpr uint32 FLAG_JUDGEMENT = 0x800000;                 // SpellFamilyFlags[0]: the damage spell of every Seal's Judgement (and two of the three Judgement casts)
    constexpr uint32 FLAG_CONSECRATION = 0x20;                  // SpellFamilyFlags[0]
    constexpr uint8 BASE_CENSURE_STACKS = 5;                    // the stack limit without Favor of the Censure
    constexpr uint8 SPREAD_AT_STACKS = 5;                       // Spreading Censure goes off when a Censure reaches this many stacks
    constexpr uint32 PYRE_MAX_TICKS = 10;                       // Funeral Pyre counts at most this many ticks of the Censure as damage done
    constexpr uint32 EVENT_DELAY_MS = 1;                        // work that must not run inside the damage or aura code that decided on it waits one millisecond of game time
    constexpr uint32 CONSECRATION_RANKS[] = { 26573, 20116, 20922, 20923, 20924, 27173, 48818, 48819 };   // asserted against Spell.dbc by tools/talents/recipes/inquisitor.py

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

    // How many stacks Censure may reach for this paladin.
    uint8 CensureStackLimit(Unit const* caster)
    {
        AuraEffect const* favor = caster->GetAuraEffect(SPELL_FAVOR_CENSURE, EFFECT_1);
        return favor ? uint8(std::max<int32>(favor->GetAmount(), BASE_CENSURE_STACKS)) : BASE_CENSURE_STACKS;
    }

    // Put `stacks` stacks of the paladin's Censure on `target` (never beyond the limit, never fewer than it already has).
    void PassCensure(Player* player, Unit* target, uint8 stacks)
    {
        player->CastSpell(target, SPELL_CENSURE, true);
        if (Aura* brand = target->GetAura(SPELL_CENSURE, player->GetGUID()))
        {
            uint8 const wanted = std::min(stacks, CensureStackLimit(player));
            if (brand->GetStackAmount() < wanted)
                brand->SetStackAmount(wanted);
        }
    }

    // What a Censure would still do on its target: the damage of one tick (spell power and stacks are already in the aura amount) times the ticks left.
    int32 RemainingCensureDamage(Unit* caster, Unit* target, Aura const* brand)
    {
        AuraEffect const* tick = brand->GetEffect(EFFECT_0);
        if (!tick || tick->GetAmplitude() <= 0)
            return 0;

        uint32 const perTick = target->SpellDamageBonusTaken(caster, brand->GetSpellInfo(), uint32(std::max(tick->GetAmount(), 0)), DOT, brand->GetStackAmount());
        int64 const total = int64(perTick) * int64(brand->GetDuration() / tick->GetAmplitude());
        return int32(std::min<int64>(total, 2000000000));
    }

    // The ground of the paladin's own Consecration, whichever rank it was cast with; null when there is none.
    DynamicObject* FindConsecration(Unit* paladin)
    {
        for (uint32 rank : CONSECRATION_RANKS)
            if (DynamicObject* ground = paladin->GetDynObject(rank))
                return ground;

        return nullptr;
    }

    // Funeral Pyre, after the death: the burst on every enemy around the corpse and the stacks to the nearest of them.
    void FuneralPyre(Unit* corpse, ObjectGuid playerGuid, uint32 burstSpell, float radius, int32 burst, uint8 stacks)
    {
        if (corpse->IsAlive() || !corpse->IsInWorld())
            return;

        Player* player = ObjectAccessor::GetPlayer(*corpse, playerGuid);
        if (!player || !player->IsInWorld() || !player->IsAlive())
            return;

        Unit* nearest = nullptr;
        float nearestDistance = radius + 1.0f;
        for (Unit* unit : EnemiesAround(player, corpse, radius))
        {
            if (unit == corpse || !unit->IsAlive())
                continue;

            CastFlat(player, unit, burstSpell, burst);
            float const distance = corpse->GetDistance(unit);
            if (distance < nearestDistance)
            {
                nearest = unit;
                nearestDistance = distance;
            }
        }

        if (nearest && nearest->IsAlive())
            PassCensure(player, nearest, stacks);
    }
}

// Censure: after the cast has put its stack on the target, hold the stacks at the limit, and run Spreading Censure when this cast brought the brand up to 5 stacks.
class spell_blessings_inquisitor_censure : public SpellScript
{
    PrepareSpellScript(spell_blessings_inquisitor_censure);

    void HandleBeforeHit(SpellMissInfo missInfo)
    {
        _stacksBefore = 0;
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (missInfo != SPELL_MISS_NONE || !caster || !target)
            return;

        if (Aura const* brand = target->GetAura(SPELL_CENSURE, caster->GetGUID()))
            _stacksBefore = brand->GetStackAmount();
    }

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target)
            return;

        Aura* brand = target->GetAura(SPELL_CENSURE, caster->GetGUID());
        if (!brand)
            return;

        uint8 const limit = CensureStackLimit(caster);
        if (brand->GetStackAmount() > limit)
            brand->SetStackAmount(limit);

        if (_stacksBefore < SPREAD_AT_STACKS && brand->GetStackAmount() >= SPREAD_AT_STACKS)
            Spread(caster, target);
    }

    // Spreading Censure: one stack on the nearest other enemy within range that has the fewest stacks; a cooldown (the amount of the Boon's effect 0, in seconds) keeps it rare.
    void Spread(Unit* caster, Unit* source)
    {
        Player* player = caster->ToPlayer();
        AuraEffect const* spreading = caster->GetAuraEffect(SPELL_SPREADING_CENSURE, EFFECT_0);
        if (!player || !spreading || player->HasSpellCooldown(SPELL_SPREADING_CENSURE))
            return;

        float const range = float(spreading->GetSpellInfo()->Effects[EFFECT_0].MiscValue);
        uint8 const limit = CensureStackLimit(caster);
        Unit* best = nullptr;
        uint8 bestStacks = 255;
        float bestDistance = range + 1.0f;
        for (Unit* unit : EnemiesAround(caster, source, range))
        {
            if (unit == source)
                continue;

            Aura const* brand = unit->GetAura(SPELL_CENSURE, caster->GetGUID());
            uint8 const stacks = brand ? brand->GetStackAmount() : 0;
            float const distance = source->GetDistance(unit);
            if (stacks >= limit || stacks > bestStacks || (stacks == bestStacks && distance >= bestDistance))
                continue;

            best = unit;
            bestStacks = stacks;
            bestDistance = distance;
        }

        if (!best)
            return;

        player->AddSpellCooldown(SPELL_SPREADING_CENSURE, 0, uint32(std::max(spreading->GetAmount(), 0)) * IN_MILLISECONDS);
        caster->CastSpell(best, SPELL_CENSURE, true);
    }

    void Register() override
    {
        BeforeHit += BeforeSpellHitFn(spell_blessings_inquisitor_censure::HandleBeforeHit);
        AfterHit += SpellHitFn(spell_blessings_inquisitor_censure::HandleAfterHit);
    }

private:
    uint8 _stacksBefore = 0;
};

// Condemn: effect 1 is a dummy whose amount is the percent more damage for every Censure stack on the target. The stacks stay.
class spell_blessings_inquisitor_condemn : public SpellScript
{
    PrepareSpellScript(spell_blessings_inquisitor_condemn);

    void HandleDamage(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target)
            return;

        Aura const* brand = target->GetAura(SPELL_CENSURE, caster->GetGUID());
        if (!brand)
            return;

        int32 const perStack = GetSpellInfo()->Effects[EFFECT_1].CalcValue(caster);
        int32 const damage = GetHitDamage();
        SetHitDamage(damage + CalculatePct(damage, perStack * int32(brand->GetStackAmount())));
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_blessings_inquisitor_condemn::HandleDamage, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// Sunfire Burst: effect 0 holds the percent of the critical hit (amount), the yards around the target (misc value) and the burst spell (trigger spell). Only critical Exorcism hits reach
// here (the hit mask of the proc row).
class spell_blessings_inquisitor_sunfire_burst : public AuraScript
{
    PrepareAuraScript(spell_blessings_inquisitor_sunfire_burst);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return ValidateSpellInfo({ spellInfo->Effects[EFFECT_0].TriggerSpell });
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        DamageInfo const* damageInfo = eventInfo.GetDamageInfo();
        Unit* caster = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        if (!damageInfo || !victim || !damageInfo->GetDamage())
            return;

        int32 const burst = int32(CalculatePct(damageInfo->GetDamage(), aurEff->GetAmount()));
        uint32 const burstSpell = GetSpellInfo()->Effects[EFFECT_0].TriggerSpell;
        float const radius = float(GetSpellInfo()->Effects[EFFECT_0].MiscValue);
        for (Unit* unit : EnemiesAround(caster, victim, radius))
            if (unit != victim)
                CastFlat(caster, unit, burstSpell, burst);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_inquisitor_sunfire_burst::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Dawn Hammer: effect 0 holds the percent of the damage dealt again (amount), the seconds until it lands (misc value) and the echo spell (trigger spell). Every enemy the Wrath hits
// reaches here once; the first one of a cast also refreshes the paladin's Consecration.
class spell_blessings_inquisitor_dawn_hammer : public AuraScript
{
    PrepareAuraScript(spell_blessings_inquisitor_dawn_hammer);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return ValidateSpellInfo({ spellInfo->Effects[EFFECT_0].TriggerSpell });
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Player* player = GetTarget()->ToPlayer();
        DamageInfo const* damageInfo = eventInfo.GetDamageInfo();
        Unit* victim = eventInfo.GetActionTarget();
        if (!player || !victim || !damageInfo || !damageInfo->GetDamage())
            return;

        int32 const echo = int32(CalculatePct(damageInfo->GetDamage(), aurEff->GetAmount()));
        uint32 const echoSpell = GetSpellInfo()->Effects[EFFECT_0].TriggerSpell;
        uint32 const delay = uint32(std::max(GetSpellInfo()->Effects[EFFECT_0].MiscValue, 1)) * IN_MILLISECONDS;
        ObjectGuid const playerGuid = player->GetGUID();
        victim->m_Events.AddEventAtOffset([victim, playerGuid, echoSpell, echo]()
        {
            Player* paladin = ObjectAccessor::GetPlayer(*victim, playerGuid);
            if (paladin && paladin->IsInWorld() && paladin->IsAlive() && victim->IsInWorld() && victim->IsAlive())
                CastFlat(paladin, victim, echoSpell, echo);
        }, std::chrono::milliseconds(delay));

        uint32 const now = getMSTime();
        if (!_refreshed || getMSTimeDiff(_lastRefresh, now) > REFRESH_WINDOW_MS)
        {
            _refreshed = true;
            _lastRefresh = now;
            RefreshConsecration(player);
        }
    }

    // The ground of Consecration lasts its full length again (the length of the rank, with the paladin's duration modifiers).
    static void RefreshConsecration(Player* player)
    {
        for (uint32 rank : CONSECRATION_RANKS)
        {
            DynamicObject* ground = player->GetDynObject(rank);
            SpellInfo const* info = sSpellMgr->GetSpellInfo(rank);
            if (!ground || !info)
                continue;

            int32 duration = info->GetDuration();
            player->ApplySpellMod(rank, SPELLMOD_DURATION, duration);
            if (duration > ground->GetDuration())
                ground->SetDuration(duration);
        }
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_inquisitor_dawn_hammer::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }

private:
    static constexpr uint32 REFRESH_WINDOW_MS = 500;      // the targets of one Wrath all proc within this window
    uint32 _lastRefresh = 0;
    bool _refreshed = false;
};

// Final Verdict: a proc on the CAST of a Judgement (not on its damage). Effect 0 holds the percent of the remaining Censure damage (amount), the stacks needed (misc value) and the burst
// spell (trigger spell); effect 1's trigger spell is the one-charge critical strike buff. The buff is in place before the core casts the Judgement's damage spell a moment later.
class spell_blessings_inquisitor_final_verdict : public AuraScript
{
    PrepareAuraScript(spell_blessings_inquisitor_final_verdict);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return ValidateSpellInfo({ spellInfo->Effects[EFFECT_0].TriggerSpell, spellInfo->Effects[EFFECT_1].TriggerSpell });
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        uint32 const burstSpell = GetSpellInfo()->Effects[EFFECT_0].TriggerSpell;
        uint32 const buffSpell = GetSpellInfo()->Effects[EFFECT_1].TriggerSpell;
        caster->RemoveAurasDueToSpell(buffSpell);              // a buff left over from a Judgement that never landed

        Spell const* spell = eventInfo.GetProcSpell();
        Unit* victim = spell ? spell->m_targets.GetUnitTarget() : nullptr;
        if (!victim || !victim->IsAlive() || victim->IsFriendlyTo(caster))
            return;

        Aura* brand = victim->GetAura(SPELL_CENSURE, caster->GetGUID());
        if (!brand || int32(brand->GetStackAmount()) < GetSpellInfo()->Effects[EFFECT_0].MiscValue)
            return;

        int32 const burst = int32(CalculatePct(RemainingCensureDamage(caster, victim, brand), aurEff->GetAmount()));
        victim->RemoveAurasDueToSpell(SPELL_CENSURE, caster->GetGUID());
        caster->CastSpell(caster, buffSpell, true);
        CastFlat(caster, victim, burstSpell, burst);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_inquisitor_final_verdict::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

class MoonlitInquisitorUnitScript : public UnitScript
{
public:
    MoonlitInquisitorUnitScript() : UnitScript("MoonlitInquisitorUnitScript", true,
        { UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK, UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN, UNITHOOK_ON_DAMAGE }) { }

    // Each tick of a Paladin damage-over-time effect (this hook also runs for heals over time: the friendly check leaves those out).
    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* spellInfo) override
    {
        if (!attacker || !attacker->IsPlayer() || !target || !spellInfo || !damage || spellInfo->SpellFamilyName != SPELLFAMILY_PALADIN)
            return;

        Player* player = attacker->ToPlayer();
        if (target == player || target->IsFriendlyTo(player))
            return;

        if (AuraEffect const* sentence = player->GetAuraEffect(SPELL_FINAL_SENTENCE, EFFECT_0))
            if (target->HealthBelowPct(sentence->GetSpellInfo()->Effects[EFFECT_0].MiscValue))
                damage += CalculatePct(damage, sentence->GetAmount());

        if (spellInfo->SpellFamilyFlags[0] & FLAG_CONSECRATION)
            if (AuraEffect const* radiance = player->GetAuraEffect(SPELL_WRATHFUL_RADIANCE, EFFECT_0))
                if (DynamicObject* ground = player->GetDynObject(spellInfo->Id))
                    if (player->IsWithinDist(ground, ground->GetRadius()))
                        damage += CalculatePct(damage, radiance->GetAmount());
    }

    // Burning Verdict: the damage of a Judgement (before the hit lands; direct damage only).
    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo) override
    {
        if (!attacker || !attacker->IsPlayer() || !target || !spellInfo || damage <= 0 || spellInfo->SpellFamilyName != SPELLFAMILY_PALADIN || !(spellInfo->SpellFamilyFlags[0] & FLAG_JUDGEMENT))
            return;

        Player* player = attacker->ToPlayer();
        AuraEffect const* verdict = player->GetAuraEffect(SPELL_BURNING_VERDICT, EFFECT_1);
        if (verdict && target != player && target->HasAura(SPELL_CENSURE, player->GetGUID()))
            damage += CalculatePct(damage, verdict->GetAmount());
    }

    // Funeral Pyre: this runs inside Unit::DealDamage after the absorbs, before the death takes the auras away (OnUnitDeath comes after they are gone). Everything the burst needs is read
    // here; it goes off one millisecond later and only if the enemy really died.
    void OnDamage(Unit* /*attacker*/, Unit* victim, uint32& damage) override
    {
        if (!victim || !damage || damage < victim->GetHealth() || !victim->HasAura(SPELL_CENSURE))
            return;

        auto const brands = victim->GetAppliedAuras().equal_range(SPELL_CENSURE);
        for (auto it = brands.first; it != brands.second; ++it)
        {
            Aura* brand = it->second->GetBase();
            Unit* caster = brand->GetCaster();
            Player* player = caster ? caster->ToPlayer() : nullptr;
            AuraEffect const* pyre = player && player->IsInWorld() ? player->GetAuraEffect(SPELL_FUNERAL_PYRE, EFFECT_0) : nullptr;
            AuraEffect const* tick = brand->GetEffect(EFFECT_0);
            if (!pyre || !tick || victim == player)
                continue;

            // the damage the Censure has done is estimated: its damage per tick (stacks included) times the ticks so far
            uint8 const stacks = brand->GetStackAmount();
            int64 const done = int64(std::max(tick->GetAmount(), 0)) * int64(std::min(tick->GetTickNumber(), PYRE_MAX_TICKS));
            int32 const burst = int32(std::min<int64>(done * int64(pyre->GetAmount()) * int64(stacks) / 100, 2000000000));
            if (burst <= 0)
                continue;

            uint32 const burstSpell = pyre->GetSpellInfo()->Effects[EFFECT_0].TriggerSpell;
            float const radius = float(pyre->GetSpellInfo()->Effects[EFFECT_0].MiscValue);
            ObjectGuid const playerGuid = player->GetGUID();
            Unit* corpse = victim;
            corpse->m_Events.AddEventAtOffset([corpse, playerGuid, burstSpell, radius, burst, stacks]()
            {
                FuneralPyre(corpse, playerGuid, burstSpell, radius, burst, stacks);
            }, std::chrono::milliseconds(EVENT_DELAY_MS));
        }
    }
};

void AddMoonlitBlessingsInquisitorScripts()
{
    RegisterSpellScript(spell_blessings_inquisitor_censure);
    RegisterSpellScript(spell_blessings_inquisitor_condemn);
    RegisterSpellScript(spell_blessings_inquisitor_sunfire_burst);
    RegisterSpellScript(spell_blessings_inquisitor_dawn_hammer);
    RegisterSpellScript(spell_blessings_inquisitor_final_verdict);
    new MoonlitInquisitorUnitScript();
}
