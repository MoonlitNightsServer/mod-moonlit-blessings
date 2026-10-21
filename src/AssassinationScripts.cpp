// Moonlit Blessings: C++ spell scripts for the Assassination Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/assassination.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_assassination_venom_surge     Venom Surge: a poison hit or Deadly Poison tick strikes again for the same damage (the proc row picks the poisons and the 15%)
//   spell_blessings_assassination_garrote_artist  Favor of the Garrote Artist: a Garrote rank without its own silence (below rank 7) silences for 3 sec
//   spell_blessings_assassination_venom_master    Favor of the Venom Master: after Envenom the extra Deadly Poison doses are counted again
//   MoonlitAssassinationUnitScript                Silent Death (longer silences, more damage against a silenced target), Favor of the Bleeder (other bleeds),
//                                                 Favor of the Venom Master (7 doses of Deadly Poison, poisons pass on when the target dies)
//
// Spell ids come from the Assassination registry block 1,710,000 (units are listed at the top of tools/talents/recipes/assassination.py: the enum below must match them, and the recipe
// asserts every id and class mask named here). All state lives in auras (GUIDs and spell ids only): nothing is kept between hooks.
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
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include <algorithm>
#include <list>

namespace
{
    enum AssassinationSpells : uint32
    {
        SPELL_SILENT_DEATH         = 1710020,   // effect 0: percent more damage to a silenced target, effect 1: extra silence time in ms
        SPELL_VENOM_SURGE_STRIKE   = 1710031,   // the echo hit (damage is passed in)
        SPELL_FAVOR_BLEEDER        = 1710040,   // effect 2: percent more damage from other bleeds
        SPELL_FAVOR_VENOM_MASTER   = 1710060,   // effect 1: extra Deadly Poison doses, effect 2: spread range in yards
        SPELL_VENOM_DOSES          = 1710061,   // hidden marker on the target: stack = (doses above the normal maximum) + 1

        SPELL_GARROTE_SILENCE      = 1330,      // Garrote - Silence (a Garrote of rank 7 or more triggers it with its third effect)
        SPELL_IMPROVED_KICK_SILENCE = 18425     // Silenced - Improved Kick
    };

    // SpellFamilyFlags of the stock Rogue spells (the recipe asserts them)
    constexpr uint32 FLAG_GARROTE = 0x00000100;             // [0]
    constexpr uint32 FLAG_RUPTURE = 0x00100000;             // [0]
    constexpr uint32 FLAG_INSTANT_POISON = 0x00002000;      // [0]
    constexpr uint32 FLAG_DEADLY_POISON = 0x00010000;       // [0]
    constexpr uint32 FLAG_WOUND_POISON = 0x10000000;        // [0]
    constexpr uint32 FLAG_ENVENOM = 0x00000008;             // [1]
    constexpr uint32 FLAG_POISON_AURA = 0x00080000;         // [1]: Deadly, Crippling, Mind-numbing and Wound Poison (the poisons that leave an aura on the target)

    constexpr int32 MIN_SPREAD_DURATION_MS = 2000;          // a poison with less time left than this is not worth passing on

    bool IsRogueSpell(SpellInfo const* info)
    {
        return info && info->SpellFamilyName == SPELLFAMILY_ROGUE;
    }

    // The Deadly Poison aura (the one that stacks): Deadly Poison, word 0 bit 0x10000, with the poison-aura bit of word 1.
    bool IsDeadlyPoisonAura(SpellInfo const* info)
    {
        return IsRogueSpell(info) && (info->SpellFamilyFlags[0] & FLAG_DEADLY_POISON) && (info->SpellFamilyFlags[1] & FLAG_POISON_AURA) && info->StackAmount > 1;
    }

    bool IsPoisonAura(SpellInfo const* info)
    {
        return IsRogueSpell(info) && (info->SpellFamilyFlags[1] & FLAG_POISON_AURA);
    }

    // Rupture and Garrote carry their own modifier from the Favor of the Bleeder (a real spell modifier), so the script leaves them alone.
    bool IsRuptureOrGarrote(SpellInfo const* info)
    {
        return IsRogueSpell(info) && (info->SpellFamilyFlags[0] & (FLAG_GARROTE | FLAG_RUPTURE));
    }

    // Silent Death: the percent more damage the rogue deals to this target (0 when the rogue has no Silent Death or the target is not silenced). Runs on every hit, so everything is rejected early.
    int32 SilentDeathPercent(Unit* target, Unit* attacker)
    {
        if (!attacker || !target || !attacker->IsPlayer() || target == attacker)
            return 0;

        AuraEffect const* death = attacker->GetAuraEffect(SPELL_SILENT_DEATH, EFFECT_0);
        if (!death || !target->HasAuraType(SPELL_AURA_MOD_SILENCE) || target->IsFriendlyTo(attacker))
            return 0;

        return std::max(death->GetAmount(), 0);
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

    // Silent Death: the silence the rogue just applied (Garrote's or Improved Kick's) lasts the extra time. The hook also fires when the aura is refreshed, so the length is always worked out from the
    // spell's own duration (never added to the current one); the Spell then applies diminishing returns to this longer length.
    void LengthenSilence(Unit* victim, Aura* aura)
    {
        Player* rogue = ObjectAccessor::GetPlayer(*victim, aura->GetCasterGUID());
        if (!rogue || rogue == victim)
            return;

        AuraEffect const* death = rogue->GetAuraEffect(SPELL_SILENT_DEATH, EFFECT_1);
        if (!death || death->GetAmount() <= 0)
            return;

        int32 const duration = aura->CalcMaxDuration(rogue);
        if (duration <= 0 || aura->IsPermanent())
            return;

        aura->SetMaxDuration(duration + death->GetAmount());
        aura->SetDuration(duration + death->GetAmount());
    }

    // Favor of the Venom Master: more doses of Deadly Poison than the normal five. The core limits a stack to the spell's maximum (it sets a higher count back to five before this hook runs), so the
    // number of doses above five is remembered in the hidden marker aura (stack = extra doses + 1, present from the moment the poison reaches five) and put back each time a dose lands.
    void RaiseDoses(Unit* victim, Aura* poison)
    {
        Player* rogue = ObjectAccessor::GetPlayer(*victim, poison->GetCasterGUID());
        if (!rogue || rogue == victim)
            return;

        AuraEffect const* favor = rogue->GetAuraEffect(SPELL_FAVOR_VENOM_MASTER, EFFECT_1);
        if (!favor)
            return;

        uint8 const normalMax = uint8(poison->GetSpellInfo()->StackAmount);
        uint8 const extraMax = uint8(std::clamp(favor->GetAmount(), 0, 2));
        Aura* marker = victim->GetAura(SPELL_VENOM_DOSES, rogue->GetGUID());
        if (poison->GetStackAmount() < normalMax)
        {
            if (marker)
                marker->Remove();

            return;
        }

        if (!marker)
        {
            marker = rogue->AddAura(SPELL_VENOM_DOSES, victim);          // the poison has just reached its normal maximum: five doses, no extra yet
            if (!marker)
                return;
        }
        else
        {
            if (marker->GetStackAmount() <= extraMax)
                marker->ModStackAmount(1);                               // one more dose above the normal five (a full marker stays as it is)

            poison->SetStackAmount(uint8(normalMax - 1 + marker->GetStackAmount()));     // the core set it back to five: put the doses back
            poison->RefreshDuration();                                   // a dose that the core clamped did not refresh the timer
        }

        marker->SetMaxDuration(poison->GetMaxDuration());
        marker->SetDuration(poison->GetDuration());
    }

    // Favor of the Venom Master: a poison of the rogue on a target that dies passes to the nearest other enemy within `range` yards (that does not carry it yet): the same poison with the doses
    // it had (at most the normal five) and the time it had left.
    void SpreadPoison(Player* rogue, Unit* from, Aura const* source, float range)
    {
        SpellInfo const* info = source->GetSpellInfo();
        ObjectGuid const casterGuid = rogue->GetGUID();

        std::list<Unit*> enemies = EnemiesAround(rogue, from, range);
        enemies.remove_if([from, info, casterGuid](Unit* unit) { return unit == from || !unit->IsAlive() || unit->GetAura(info->Id, casterGuid); });
        if (enemies.empty())
            return;

        Unit* best = enemies.front();
        for (Unit* unit : enemies)
            if (from->GetDistance(unit) < from->GetDistance(best))
                best = unit;

        uint8 effectMask = 0;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (source->HasEffect(i))
                effectMask |= uint8(1 << i);

        Aura* copy = rogue->AddAura(info, effectMask, best);
        if (!copy)
            return;

        if (info->StackAmount > 1)
            copy->SetStackAmount(uint8(std::min<uint32>(source->GetStackAmount(), info->StackAmount)));

        copy->SetDuration(source->GetDuration());
    }
}

// Venom Surge: the proc row selects Instant Poison and Wound Poison hits and Deadly Poison ticks (15%). The application of a poison has no damage and is skipped. Effect 1 holds the percent of the
// damage that is copied (100). The echo (a free Nature hit) carries no class mask, so it cannot proc anything, this Boon included.
class spell_blessings_assassination_venom_surge : public AuraScript
{
    PrepareAuraScript(spell_blessings_assassination_venom_surge);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        DamageInfo const* damageInfo = eventInfo.GetDamageInfo();
        Unit* victim = eventInfo.GetActionTarget();
        if (!damageInfo || !damageInfo->GetDamage() || !victim || !victim->IsAlive())
            return;

        int32 const damage = int32(CalculatePct(damageInfo->GetDamage(), aurEff->GetAmount()));
        if (damage > 0)
            GetTarget()->CastCustomSpell(SPELL_VENOM_SURGE_STRIKE, SPELLVALUE_BASE_POINT0, damage - 1, victim, true, nullptr, aurEff);     // the spell adds 1 to its base points
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_assassination_venom_surge::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

// Favor of the Garrote Artist: the proc row selects Garrote. A Garrote that triggers the silence by itself (rank 7 and up, third effect) is left alone: a second cast would only count a
// diminishing returns step. A lower rank gets the same silence from the Favor.
class spell_blessings_assassination_garrote_artist : public AuraScript
{
    PrepareAuraScript(spell_blessings_assassination_garrote_artist);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        SpellInfo const* info = eventInfo.GetSpellInfo();
        Unit* victim = eventInfo.GetActionTarget();
        if (!info || !victim || !victim->IsAlive())
            return;

        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (info->Effects[i].TriggerSpell == SPELL_GARROTE_SILENCE)
                return;

        GetTarget()->CastSpell(victim, SPELL_GARROTE_SILENCE, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_assassination_garrote_artist::HandleProc, EFFECT_2, SPELL_AURA_DUMMY);
    }
};

// Favor of the Venom Master: the proc row selects Envenom, which takes doses off the Deadly Poison (inside the spell, before the proc). The marker is made to match what is left: gone when fewer
// than five doses remain (or the poison is gone), otherwise (doses - 4).
class spell_blessings_assassination_venom_master : public AuraScript
{
    PrepareAuraScript(spell_blessings_assassination_venom_master);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* rogue = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        if (!victim)
            return;

        Aura* marker = victim->GetAura(SPELL_VENOM_DOSES, rogue->GetGUID());
        if (!marker)
            return;

        AuraEffect const* deadly = victim->GetAuraEffect(SPELL_AURA_PERIODIC_DAMAGE, SPELLFAMILY_ROGUE, FLAG_DEADLY_POISON, 0, 0, rogue->GetGUID());
        uint32 const normalMax = deadly ? deadly->GetSpellInfo()->StackAmount : 0;
        uint8 const doses = deadly ? deadly->GetBase()->GetStackAmount() : 0;
        if (!deadly || doses < normalMax)
            marker->Remove();
        else
            marker->SetStackAmount(uint8(doses - normalMax + 1));
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_assassination_venom_master::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

// One script object for everything that is not a proc: damage hooks (every hit, so each one rejects early), the silence length, the extra doses and the spread on death.
class MoonlitAssassinationUnitScript : public UnitScript
{
public:
    MoonlitAssassinationUnitScript() : UnitScript("MoonlitAssassinationUnitScript", true,
        { UNITHOOK_MODIFY_MELEE_DAMAGE, UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN, UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK, UNITHOOK_ON_AURA_APPLY, UNITHOOK_ON_AURA_REMOVE }) { }

    // Silent Death: melee swings.
    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        if (!damage)
            return;

        if (int32 const percent = SilentDeathPercent(target, attacker))
            damage += CalculatePct(damage, percent);
    }

    // Silent Death: direct damage of abilities and poisons.
    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* /*spellInfo*/) override
    {
        if (damage <= 0)
            return;

        if (int32 const percent = SilentDeathPercent(target, attacker))
            damage += CalculatePct(damage, percent);
    }

    // Silent Death and the Favor of the Bleeder: each tick of a damage over time (the hook also runs for heals over time: the friendly check leaves those out).
    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* spellInfo) override
    {
        if (!damage || !attacker || !target || !spellInfo || !attacker->IsPlayer() || target == attacker || target->IsFriendlyTo(attacker))
            return;

        uint32 const base = damage;
        if (int32 const percent = SilentDeathPercent(target, attacker))
            damage += CalculatePct(base, percent);

        if ((spellInfo->GetAllEffectsMechanicMask() & (1ULL << MECHANIC_BLEED)) && !IsRuptureOrGarrote(spellInfo))
            if (AuraEffect const* bleeder = attacker->GetAuraEffect(SPELL_FAVOR_BLEEDER, EFFECT_2))
                damage += CalculatePct(base, std::max(bleeder->GetAmount(), 0));
    }

    // Silent Death (the silences) and the Favor of the Venom Master (the doses). Both id checks come first: this hook runs for every aura on every unit.
    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        if (!unit || !aura || aura->GetType() != UNIT_AURA_TYPE || !aura->GetCasterGUID().IsPlayer())
            return;

        uint32 const id = aura->GetId();
        if (id == SPELL_GARROTE_SILENCE || id == SPELL_IMPROVED_KICK_SILENCE)
            LengthenSilence(unit, aura);
        else if (IsDeadlyPoisonAura(aura->GetSpellInfo()))
            RaiseDoses(unit, aura);
    }

    // Favor of the Venom Master: a poison that is still on a target that dies passes on.
    void OnAuraRemove(Unit* unit, AuraApplication* aurApp, AuraRemoveMode mode) override
    {
        if (!unit || !aurApp || mode != AURA_REMOVE_BY_DEATH)
            return;

        Aura const* aura = aurApp->GetBase();
        if (!aura || aura->GetType() != UNIT_AURA_TYPE || !aura->GetCasterGUID().IsPlayer() || !IsPoisonAura(aura->GetSpellInfo()))
            return;

        Player* rogue = ObjectAccessor::GetPlayer(*unit, aura->GetCasterGUID());
        if (!rogue || !rogue->IsAlive() || !rogue->IsInWorld() || rogue == unit || aura->GetDuration() < MIN_SPREAD_DURATION_MS)
            return;

        AuraEffect const* favor = rogue->GetAuraEffect(SPELL_FAVOR_VENOM_MASTER, EFFECT_2);
        if (!favor || favor->GetAmount() <= 0)
            return;

        SpreadPoison(rogue, unit, aura, float(favor->GetAmount()));
    }
};

void AddMoonlitBlessingsAssassinationScripts()
{
    RegisterSpellScript(spell_blessings_assassination_venom_surge);
    RegisterSpellScript(spell_blessings_assassination_garrote_artist);
    RegisterSpellScript(spell_blessings_assassination_venom_master);
    new MoonlitAssassinationUnitScript();
}
