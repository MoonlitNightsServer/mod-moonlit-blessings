// Moonlit Blessings: C++ spell scripts for the Affliction Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/affliction.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_affliction_restless_spirits   Boon: a Haunt hit also strikes a second enemy for half the Haunt damage
//   spell_blessings_affliction_soul_fountain      Boon: a drain tick that kills its target restores health and mana (the ally heal is the generic hybrid script)
//   spell_blessings_affliction_doom_mark          Boon (Doom Sentence): the mark debuff checks every 2 sec that a curse of the warlock is still on its target
//   spell_blessings_affliction_soul_mender        Favor of the Soul Mender: teaches Soul Mend, Drain Vitality, Soul Tether and Siphon Ward while the Favor is chosen
//   spell_blessings_affliction_soul_mend          Soul Mend: the heal is exactly the health the warlock gives up
//   spell_blessings_affliction_withering          Favor of the Withering: an older Corruption or Curse of Agony keeps running as a copy when the spell is cast again
//   spell_blessings_affliction_withering_agony    the Curse of Agony copy: the stock damage ramp (half, full, one and a half, double), continued from where the carried application was
//   MoonlitAfflictionScript                       Doom Sentence (mark on a curse, leap on death), Favor of the Haunting (spread when Haunt ends) and the extra healing of
//                                                 Restless Spirits (when the Haunt aura ends)
//
// The Soul Fountain ally heal and the Soul Mender's Conversion (100% of the damage dealt heals) are the generic spell_blessings_shaman_hybrid_heal of ElementalScripts.cpp.
// Spell ids come from the Affliction registry block 1,910,000 (tools/talents/recipes/affliction.py asserts the units that are named here). Stock Warlock spells are found by
// their SpellFamilyFlags (the FLAG_* constants below, taken from Spell.dbc and asserted by affliction.py), never by id.
// ~Moonlit Nights Team

#include "BlessingsHelpers.h"
#include "Cell.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "UnitScript.h"
#include "WorldSession.h"
#include <algorithm>
#include <limits>
#include <list>
#include <vector>

namespace
{
    enum AfflictionSpells : uint32
    {
        SPELL_RESTLESS_SPIRITS      = 1910000,   // passive: effect 1 = percent of the Haunt damage (misc value = range in yards), effect 2 = percent extra healing of the returning soul
        SPELL_RESTLESS_STRIKE       = 1910001,   // the strike at the second enemy (base points 0 is set per cast)
        SPELL_SOUL_FOUNTAIN         = 1910010,   // passive: effect 1 = percent of health and mana a killing drain tick restores
        SPELL_DOOM_SENTENCE         = 1910020,   // passive: misc value of effect 0 = how far a curse leaps, in yards
        SPELL_DOOM_MARK             = 1910021,   // the debuff: 10% more damage from the warlock's damage over time
        SPELL_FAVOR_HAUNTING        = 1910030,   // passive: effect 2 = how many enemies the damage over time spreads to, misc value = range in yards
        SPELL_SOUL_MEND             = 1910060,
        SPELL_DRAIN_VITALITY        = 1910070,
        SPELL_SOUL_TETHER           = 1910080,
        SPELL_SIPHON_WARD           = 1910090,
        SPELL_WITHERING_CORRUPTION  = 1910120,   // two slots: 1910120 and 1910121
        SPELL_WITHERING_CORRUPTION2 = 1910121,
        SPELL_WITHERING_AGONY       = 1910130,   // two slots: 1910130 and 1910131
        SPELL_WITHERING_AGONY2      = 1910131
    };

    // SpellFamilyFlags of the stock Warlock spells (all ranks), from Spell.dbc
    constexpr uint32 FLAG_CORRUPTION            = 0x00000002;   // [0]
    constexpr uint32 FLAG_IMMOLATE              = 0x00000004;   // [0]
    constexpr uint32 FLAG_CURSE_OF_AGONY        = 0x00000400;   // [0]
    constexpr uint32 FLAG_UNSTABLE_AFFLICTION   = 0x00000100;   // [1]
    constexpr uint32 FLAG_SEED_OF_CORRUPTION    = 0x00000010;   // [1] the seed's damage over time
    constexpr uint32 FLAG_HAUNT                 = 0x00040000;   // [1]

    constexpr uint32 SOUL_MENDER_SPELLS[] = { SPELL_SOUL_MEND, SPELL_DRAIN_VITALITY, SPELL_SOUL_TETHER, SPELL_SIPHON_WARD };
    constexpr uint32 CORRUPTION_COPIES[] = { SPELL_WITHERING_CORRUPTION, SPELL_WITHERING_CORRUPTION2 };
    constexpr uint32 AGONY_COPIES[] = { SPELL_WITHERING_AGONY, SPELL_WITHERING_AGONY2 };

    constexpr int32 MIN_COPY_DURATION_MS = 1500;      // a damage over time with less time left than this is not worth passing on
    constexpr int32 SOUL_MEND_PCT = 20;               // percent of maximum health Soul Mend takes and gives
    constexpr float SOUL_MEND_MIN_CAST_PCT = 40.0f;   // Soul Mend cannot be cast at or below this health percent
    constexpr int32 SOUL_MEND_MIN_LEFT_PCT = 10;      // and never takes the warlock below this percent
    constexpr uint8 MAX_WITHERING_COPIES = 2;         // the main application plus two copies are the three applications

    enum WitheringKind : uint8
    {
        WITHERING_NONE       = 0,
        WITHERING_CORRUPTION = 1,
        WITHERING_AGONY      = 2
    };

    bool IsWarlockSpell(SpellInfo const* info)
    {
        return info && info->SpellFamilyName == SPELLFAMILY_WARLOCK;
    }

    bool IsHaunt(SpellInfo const* info)
    {
        return IsWarlockSpell(info) && (info->SpellFamilyFlags[1] & FLAG_HAUNT);
    }

    bool IsCurse(SpellInfo const* info)
    {
        return IsWarlockSpell(info) && info->GetSpellSpecific() == SPELL_SPECIFIC_CURSE;
    }

    bool IsUnstableOrImmolate(SpellInfo const* info)
    {
        return IsWarlockSpell(info) && ((info->SpellFamilyFlags[1] & FLAG_UNSTABLE_AFFLICTION) || (info->SpellFamilyFlags[0] & FLAG_IMMOLATE));
    }

    uint8 WitheringKindOf(SpellInfo const* info)
    {
        if (!IsWarlockSpell(info))
            return WITHERING_NONE;

        if (info->SpellFamilyFlags[0] & FLAG_CORRUPTION)
            return WITHERING_CORRUPTION;

        if (info->SpellFamilyFlags[0] & FLAG_CURSE_OF_AGONY)
            return WITHERING_AGONY;

        return WITHERING_NONE;
    }

    bool IsWitheringCopy(uint32 spellId)
    {
        return spellId == SPELL_WITHERING_CORRUPTION || spellId == SPELL_WITHERING_CORRUPTION2 || spellId == SPELL_WITHERING_AGONY || spellId == SPELL_WITHERING_AGONY2;
    }

    // The effects of a spell that are auras (what a copy applied with AddAura should carry).
    uint8 AuraEffectMask(SpellInfo const* info)
    {
        uint8 mask = 0;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (info->Effects[i].IsAura())
                mask |= uint8(1 << i);

        return mask;
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

    // Does `unit` carry a curse of this caster?
    bool HasOwnCurse(Unit const* unit, ObjectGuid casterGuid)
    {
        for (auto const& pair : unit->GetAppliedAuras())
        {
            Aura const* aura = pair.second->GetBase();
            if (aura->GetCasterGUID() == casterGuid && IsCurse(aura->GetSpellInfo()))
                return true;
        }

        return false;
    }

    // Would a copy of the damage over time `info` of this caster collide with something it already has on `unit` (the same spell, another Unstable Affliction or Immolate,
    // another curse)? A copy is only made where it adds something and takes nothing away.
    bool CopyWouldCollide(Unit const* unit, ObjectGuid casterGuid, SpellInfo const* info)
    {
        bool const group = IsUnstableOrImmolate(info);
        bool const curse = IsCurse(info);
        for (auto const& pair : unit->GetAppliedAuras())
        {
            Aura const* aura = pair.second->GetBase();
            if (aura->GetCasterGUID() != casterGuid)
                continue;

            SpellInfo const* other = aura->GetSpellInfo();
            if (other->Id == info->Id || (group && IsUnstableOrImmolate(other)) || (curse && IsCurse(other)))
                return true;
        }

        return false;
    }

    // An application of the caster's aura `source` on another unit: the same effects, the same amounts, the time the source has left.
    bool CopyAura(Player* caster, Aura const* source, Unit* to)
    {
        SpellInfo const* info = source->GetSpellInfo();
        uint8 effectMask = 0;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (source->HasEffect(i))
                effectMask |= uint8(1 << i);

        Aura* copy = caster->AddAura(info, effectMask, to);
        if (!copy)
            return false;

        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            AuraEffect* effect = copy->GetEffect(i);
            AuraEffect const* original = source->GetEffect(i);
            if (effect && original)
                effect->SetAmount(original->GetAmount());   // SetAmount also stops the aura from being recalculated back to the base value
        }

        copy->SetDuration(std::max(source->GetDuration(), MIN_COPY_DURATION_MS));
        return true;
    }

    // The caster's own main application (not a Withering copy) of Corruption or Curse of Agony on `target`.
    Aura* FindMainApplication(Unit* target, ObjectGuid casterGuid, uint8 kind)
    {
        for (auto const& pair : target->GetAppliedAuras())
        {
            Aura* aura = pair.second->GetBase();
            if (aura->GetCasterGUID() == casterGuid && !IsWitheringCopy(aura->GetId()) && WitheringKindOf(aura->GetSpellInfo()) == kind)
                return aura;
        }

        return nullptr;
    }

    // The Curse of Agony ramp of the stock script (spell_warl_curse_of_agony): ticks 1 to 4 deal half of the normal tick, 5 to 8 the normal tick, 9 to 12 one and a half, later twice.
    int32 AgonyAmountAtTick(int32 normal, uint32 tick)
    {
        if (tick <= 4)
            return normal / 2;

        if (tick <= 8)
            return normal;

        if (tick <= 12)
            return (normal + 1) * 3 / 2;

        return normal * 2;
    }

    // The normal tick of an application that has done `ticksDone` ticks and now deals `current` (before its first tick the stock script has not ramped it yet).
    int32 AgonyNormalFromCurrent(int32 current, uint32 ticksDone)
    {
        if (ticksDone == 0 || (ticksDone > 4 && ticksDone <= 8))
            return current;

        if (ticksDone <= 4)
            return current * 2;

        if (ticksDone <= 12)
            return current * 2 / 3;

        return current / 2;
    }

    // Spread the damage over time effects `caster` has on `from` (except the Haunt that ended) to up to `count` of the nearest other enemies within `range` yards of `from`.
    void SpreadDamageOverTime(Player* caster, Unit* from, Aura const* haunt, uint32 count, float range)
    {
        ObjectGuid const casterGuid = caster->GetGUID();
        std::vector<Aura const*> dots;
        for (auto const& pair : from->GetAppliedAuras())
        {
            Aura const* aura = pair.second->GetBase();
            SpellInfo const* info = aura->GetSpellInfo();
            if (aura == haunt || aura->GetCasterGUID() != casterGuid || !IsWarlockSpell(info) || aura->GetDuration() < MIN_COPY_DURATION_MS)
                continue;

            // periodic damage that is not a channel, not the seed's damage counter, and of the curses only Curse of Agony (a curse needs a target that has none)
            if (!info->HasAura(SPELL_AURA_PERIODIC_DAMAGE) || info->IsChanneled() || (info->SpellFamilyFlags[1] & FLAG_SEED_OF_CORRUPTION))
                continue;

            if (IsCurse(info) && !(info->SpellFamilyFlags[0] & FLAG_CURSE_OF_AGONY))
                continue;

            dots.push_back(aura);
        }

        if (dots.empty())
            return;

        std::list<Unit*> enemies = EnemiesAround(caster, from, range);
        enemies.remove_if([from](Unit* unit) { return unit == from || !unit->IsAlive(); });
        enemies.sort([from](Unit* a, Unit* b) { return from->GetDistance(a) < from->GetDistance(b); });

        uint32 done = 0;
        for (Unit* other : enemies)
        {
            if (done >= count)
                break;

            bool gave = false;
            for (Aura const* dot : dots)
                if (!CopyWouldCollide(other, casterGuid, dot->GetSpellInfo()))
                    gave = CopyAura(caster, dot, other) || gave;

            if (gave)
                ++done;
        }
    }

    // A curse whose carrier died leaps to the nearest other enemy within `range` yards that has none of the warlock's curses.
    void LeapCurse(Player* caster, Unit* from, Aura const* curse, float range)
    {
        if (curse->GetDuration() < MIN_COPY_DURATION_MS)
            return;

        ObjectGuid const casterGuid = caster->GetGUID();
        std::list<Unit*> enemies = EnemiesAround(caster, from, range);
        enemies.remove_if([from, casterGuid](Unit* unit) { return unit == from || !unit->IsAlive() || HasOwnCurse(unit, casterGuid); });
        if (enemies.empty())
            return;

        Unit* nearest = *std::min_element(enemies.begin(), enemies.end(), [from](Unit* a, Unit* b) { return from->GetDistance(a) < from->GetDistance(b); });
        CopyAura(caster, curse, nearest);
    }
}

// Restless Spirits: effect 1 is a dummy whose proc is a hit of Haunt (the spell_proc row selects Haunt). The nearest other enemy within the misc value of the effect (yards) of
// Haunt's target is struck for the effect's percent of the Haunt damage. A triggered Haunt (none exists) would not count.
class spell_blessings_affliction_restless_spirits : public AuraScript
{
    PrepareAuraScript(spell_blessings_affliction_restless_spirits);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_RESTLESS_STRIKE });
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        Unit* caster = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        if (!victim || !damageInfo || !damageInfo->GetDamage() || !IsHaunt(eventInfo.GetSpellInfo()))
            return;

        if (Spell const* spell = eventInfo.GetProcSpell())
            if (spell->IsTriggered())
                return;

        float const range = float(GetSpellInfo()->Effects[aurEff->GetEffIndex()].MiscValue);
        Unit* best = nullptr;
        float bestDistance = range + 1.0f;
        for (Unit* unit : EnemiesAround(caster, victim, range))
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

        if (!best)
            return;

        int32 const damage = CalculatePct(int32(damageInfo->GetDamage()), aurEff->GetAmount());
        if (damage > 0)
            caster->CastCustomSpell(SPELL_RESTLESS_STRIKE, SPELLVALUE_BASE_POINT0, damage, best, true, nullptr, aurEff);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_affliction_restless_spirits::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

// Soul Fountain: effect 0 is the generic hybrid heal (another script on the same spell); effect 1 is this dummy on the same proc (a periodic tick of Drain Life or Drain Soul).
// The proc comes after the tick's damage was dealt, so a target that is dead now was killed by this tick: the warlock regains the effect's percent of his health and mana.
class spell_blessings_affliction_soul_fountain : public AuraScript
{
    PrepareAuraScript(spell_blessings_affliction_soul_fountain);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        Unit* caster = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        if (!victim || victim->IsAlive() || !caster->IsAlive())
            return;

        int32 const percent = aurEff->GetAmount();
        if (percent <= 0)
            return;

        BlessingsHelpers::HealUnit(caster, caster, caster->CountPctFromMaxHealth(percent), GetId());
        if (uint32 const maxMana = caster->GetMaxPower(POWER_MANA))
            caster->EnergizeBySpell(caster, GetId(), CalculatePct(maxMana, percent), POWER_MANA);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_affliction_soul_fountain::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

// The Doom Sentence mark (effect 1, a periodic dummy every 2 sec): the mark may only stay while the warlock still has the Boon and one of his curses is on the target.
// (The UnitScript below removes it as soon as the last curse goes; this is the safety net, for instance when the Boon is taken away in the fight.)
class spell_blessings_affliction_doom_mark : public AuraScript
{
    PrepareAuraScript(spell_blessings_affliction_doom_mark);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(SPELL_DOOM_SENTENCE) || !HasOwnCurse(GetTarget(), caster->GetGUID()))
            Remove();
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_affliction_doom_mark::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Favor of the Soul Mender: the hidden Favor aura teaches the four healing spells when it is applied and takes them away when it ends (the way Fire's Cauterizer does it).
// The aura's other effects are the 60% damage loss (effect 0) and the +150% on Drain Vitality (effect 2); this script owns the dummy effect 1.
class spell_blessings_affliction_soul_mender : public AuraScript
{
    PrepareAuraScript(spell_blessings_affliction_soul_mender);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Player* player = GetTarget()->ToPlayer())
            for (uint32 const spellId : SOUL_MENDER_SPELLS)
                player->learnSpell(spellId);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player || !player->GetSession() || player->GetSession()->PlayerLogout())
            return;

        for (uint32 const spellId : SOUL_MENDER_SPELLS)
            player->removeSpell(spellId, SPEC_MASK_ALL, false);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_affliction_soul_mender::HandleApply, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_affliction_soul_mender::HandleRemove, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Soul Mend: the heal is 20% of the warlock's maximum health and the same amount is taken from him (never below 10% of his health, and the cast needs more than 40%).
class spell_blessings_affliction_soul_mend : public SpellScript
{
    PrepareSpellScript(spell_blessings_affliction_soul_mend);

    SpellCastResult CheckCast()
    {
        if (GetCaster()->GetHealthPct() <= SOUL_MEND_MIN_CAST_PCT)
        {
            SetCustomCastResultMessage(SPELL_CUSTOM_ERROR_NOT_ENOUGH_HEALTH);
            return SPELL_FAILED_CUSTOM_ERROR;
        }

        return SPELL_CAST_OK;
    }

    void HandleHit()
    {
        Unit* caster = GetCaster();
        int32 const wanted = int32(caster->CountPctFromMaxHealth(SOUL_MEND_PCT));
        int32 const spare = int32(caster->GetHealth()) - int32(caster->CountPctFromMaxHealth(SOUL_MEND_MIN_LEFT_PCT));
        int32 const amount = std::min(wanted, spare);
        if (amount <= 0)
        {
            PreventHitHeal();
            return;
        }

        SetHitHeal(amount);
        caster->ModifyHealth(-amount);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_blessings_affliction_soul_mend::CheckCast);
        OnHit += SpellHitFn(spell_blessings_affliction_soul_mend::HandleHit);
    }
};

// Favor of the Withering. The passive's effect 0 is a dummy (amount = the applications one target may carry, the main one included) whose proc is a cast of Corruption or Curse of
// Agony in two phases of the same spell: CAST (the old application is still as it was; it is remembered) and HIT (the cast landed; the old application becomes a copy). The new cast
// has already started a full application on the real spell by then. A cast that misses never reaches the HIT phase, so nothing is moved.
class spell_blessings_affliction_withering : public AuraScript
{
    PrepareAuraScript(spell_blessings_affliction_withering);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_WITHERING_CORRUPTION, SPELL_WITHERING_CORRUPTION2, SPELL_WITHERING_AGONY, SPELL_WITHERING_AGONY2 });
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        Player* caster = GetTarget()->ToPlayer();
        Spell const* spell = eventInfo.GetProcSpell();
        uint8 const kind = WitheringKindOf(eventInfo.GetSpellInfo());
        if (!caster || !spell || spell->IsTriggered() || kind == WITHERING_NONE)
            return;

        if (eventInfo.GetSpellPhaseMask() & PROC_SPELL_PHASE_CAST)
            Remember(caster, spell, kind);
        else if (eventInfo.GetSpellPhaseMask() & PROC_SPELL_PHASE_HIT)
            Move(caster, eventInfo.GetActionTarget(), kind, aurEff->GetAmount());
    }

    // The cast starts: note the damage per tick, the time left and the ticks done of the application that is on the target now.
    void Remember(Player* caster, Spell const* spell, uint8 kind)
    {
        _kept = Kept();
        Unit* target = spell->m_targets.GetUnitTarget();
        Aura const* main = target ? FindMainApplication(target, caster->GetGUID(), kind) : nullptr;
        AuraEffect const* effect = main ? main->GetEffect(EFFECT_0) : nullptr;
        if (!effect)
            return;

        _kept.target = target->GetGUID();
        _kept.kind = kind;
        _kept.amount = effect->GetAmount();
        _kept.duration = main->GetDuration();
        _kept.ticks = effect->GetTickNumber();
        _kept.valid = true;
    }

    // The cast landed: the remembered application goes into a free copy slot (or replaces the copy with the least time left).
    void Move(Player* caster, Unit* victim, uint8 kind, int32 applications)
    {
        Kept const kept = _kept;
        _kept = Kept();
        if (!kept.valid || kept.kind != kind || !victim || victim->GetGUID() != kept.target || kept.duration < MIN_COPY_DURATION_MS)
            return;

        uint32 const* slots = kind == WITHERING_CORRUPTION ? CORRUPTION_COPIES : AGONY_COPIES;
        uint8 const copies = uint8(std::min<int32>(std::max<int32>(applications - 1, 0), MAX_WITHERING_COPIES));
        uint32 chosen = 0;
        int32 shortest = std::numeric_limits<int32>::max();
        for (uint8 i = 0; i < copies; ++i)
        {
            Aura const* existing = victim->GetAura(slots[i], caster->GetGUID());
            if (!existing)
            {
                chosen = slots[i];
                break;
            }

            if (existing->GetDuration() < shortest)
            {
                shortest = existing->GetDuration();
                chosen = slots[i];
            }
        }

        SpellInfo const* info = chosen ? sSpellMgr->GetSpellInfo(chosen) : nullptr;
        if (!info)
            return;

        victim->RemoveAurasDueToSpell(chosen, caster->GetGUID());
        Aura* copy = caster->AddAura(info, AuraEffectMask(info), victim);
        if (!copy)
            return;

        if (AuraEffect* damage = copy->GetEffect(EFFECT_0))
            damage->SetAmount(kept.amount);

        if (kind == WITHERING_AGONY)
        {
            if (AuraEffect* done = copy->GetEffect(EFFECT_1))
                done->SetAmount(int32(kept.ticks));

            if (AuraEffect* normal = copy->GetEffect(EFFECT_2))
                normal->SetAmount(AgonyNormalFromCurrent(kept.amount, kept.ticks));
        }

        copy->SetDuration(kept.duration);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_affliction_withering::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }

private:
    struct Kept
    {
        ObjectGuid target;
        uint8 kind = WITHERING_NONE;
        int32 amount = 0;
        int32 duration = 0;
        uint32 ticks = 0;
        bool valid = false;
    };

    Kept _kept;
};

// The Withering Curse of Agony copy: effect 0 is the damage, effect 1 (dummy) the ticks the carried application had done, effect 2 (dummy) its normal tick. Before every tick the
// damage is set to the stock ramp for that tick number counted on from the carried application (the stock script ramps by the copy's own tick number only, so it has no say here).
class spell_blessings_affliction_withering_agony : public AuraScript
{
    PrepareAuraScript(spell_blessings_affliction_withering_agony);

    void HandleUpdate(AuraEffect* aurEff)
    {
        AuraEffect const* done = GetEffect(EFFECT_1);
        AuraEffect const* normal = GetEffect(EFFECT_2);
        if (!done || !normal || normal->GetAmount() <= 0)
            return;                         // a copy made some other way (spread) keeps the amount it was given

        aurEff->SetAmount(AgonyAmountAtTick(normal->GetAmount(), aurEff->GetTickNumber() + uint32(std::max(done->GetAmount(), 0))));
    }

    void Register() override
    {
        OnEffectUpdatePeriodic += AuraEffectUpdatePeriodicFn(spell_blessings_affliction_withering_agony::HandleUpdate, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
    }
};

// Doom Sentence (a curse of the warlock goes on or comes off an enemy), the spread of Favor of the Haunting and the extra heal of Restless Spirits (the Haunt aura ends).
// Both hooks reject everything that is not a Warlock aura of a player on a unit before they look at anything else.
class MoonlitAfflictionScript : public UnitScript
{
public:
    MoonlitAfflictionScript() : UnitScript("MoonlitAfflictionScript", true, { UNITHOOK_ON_AURA_APPLY, UNITHOOK_ON_AURA_REMOVE }) { }

    // Doom Sentence: a curse goes on, the mark goes on with it.
    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        if (!unit || !aura || aura->GetType() != UNIT_AURA_TYPE || !aura->GetCasterGUID().IsPlayer() || !IsCurse(aura->GetSpellInfo()))
            return;

        Player* caster = ObjectAccessor::GetPlayer(*unit, aura->GetCasterGUID());
        if (!caster || !caster->IsInWorld() || !caster->GetAuraEffect(SPELL_DOOM_SENTENCE, EFFECT_0))
            return;

        if (!unit->HasAura(SPELL_DOOM_MARK, caster->GetGUID()))
            caster->AddAura(SPELL_DOOM_MARK, unit);
    }

    void OnAuraRemove(Unit* unit, AuraApplication* aurApp, AuraRemoveMode mode) override
    {
        if (!unit || !aurApp)
            return;

        Aura* aura = aurApp->GetBase();
        if (!aura || aura->GetType() != UNIT_AURA_TYPE || !aura->GetCasterGUID().IsPlayer())
            return;

        SpellInfo const* info = aura->GetSpellInfo();
        if (!IsWarlockSpell(info))
            return;

        if (IsHaunt(info))
            HauntEnded(unit, aura, mode);
        else if (IsCurse(info))
            CurseEnded(unit, aura, mode);
    }

private:
    // Haunt ended. Restless Spirits: the soul that returns heals 50% more (the stock script heals the warlock for the amount stored in effect 1 of the Haunt aura; this adds the
    // Boon's percent of the same amount). Favor of the Haunting: when Haunt runs out or is dispelled (not when its target dies: the effects go with it), the damage over time spreads.
    static void HauntEnded(Unit* unit, Aura* haunt, AuraRemoveMode mode)
    {
        Player* caster = ObjectAccessor::GetPlayer(*unit, haunt->GetCasterGUID());
        if (!caster || !caster->IsAlive() || !caster->IsInWorld())
            return;

        AuraEffect const* spirits = caster->GetAuraEffect(SPELL_RESTLESS_SPIRITS, EFFECT_2);
        AuraEffect const* soul = haunt->GetEffect(EFFECT_1);
        if (spirits && soul && soul->GetAmount() > 0)
        {
            int32 const extra = CalculatePct(soul->GetAmount(), spirits->GetAmount());
            if (extra > 0)
                BlessingsHelpers::HealUnit(caster, caster, uint32(extra), haunt->GetId());
        }

        if (mode != AURA_REMOVE_BY_EXPIRE && mode != AURA_REMOVE_BY_ENEMY_SPELL)
            return;

        AuraEffect const* spread = caster->GetAuraEffect(SPELL_FAVOR_HAUNTING, EFFECT_2);
        if (spread && unit->IsAlive())
            SpreadDamageOverTime(caster, unit, haunt, uint32(std::max(spread->GetAmount(), 0)), float(spread->GetSpellInfo()->Effects[EFFECT_2].MiscValue));
    }

    // A curse ended. The mark goes with the last curse; when the carrier died, the curse leaps (the copy goes on the next enemy and marks it through OnAuraApply).
    static void CurseEnded(Unit* unit, Aura* curse, AuraRemoveMode mode)
    {
        ObjectGuid const casterGuid = curse->GetCasterGUID();
        Player* caster = ObjectAccessor::GetPlayer(*unit, casterGuid);
        if (!caster || !caster->IsInWorld())
            return;

        if (unit->HasAura(SPELL_DOOM_MARK, casterGuid) && !HasOwnCurse(unit, casterGuid))
            unit->RemoveAurasDueToSpell(SPELL_DOOM_MARK, casterGuid);

        if (mode != AURA_REMOVE_BY_DEATH || !caster->IsAlive())
            return;

        AuraEffect const* doom = caster->GetAuraEffect(SPELL_DOOM_SENTENCE, EFFECT_0);
        if (doom)
            LeapCurse(caster, unit, curse, float(doom->GetSpellInfo()->Effects[EFFECT_0].MiscValue));
    }
};

void AddMoonlitBlessingsAfflictionScripts()
{
    RegisterSpellScript(spell_blessings_affliction_restless_spirits);
    RegisterSpellScript(spell_blessings_affliction_soul_fountain);
    RegisterSpellScript(spell_blessings_affliction_doom_mark);
    RegisterSpellScript(spell_blessings_affliction_soul_mender);
    RegisterSpellScript(spell_blessings_affliction_soul_mend);
    RegisterSpellScript(spell_blessings_affliction_withering);
    RegisterSpellScript(spell_blessings_affliction_withering_agony);
    new MoonlitAfflictionScript();
}
