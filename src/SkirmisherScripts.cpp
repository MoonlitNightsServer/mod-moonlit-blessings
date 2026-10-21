// Moonlit Blessings: C++ spell scripts for the Skirmisher Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/skirmisher.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_skirmisher_hurl              Kit: bound to Hurl. A thrown weapon counts as a 2.8 sec weapon for spells (Player::GetThrownSpellWeaponFactor); Hurl is thrown at the
//                                                weapon's real speed, so its hit is divided by that factor again (damage per second of the weapon instead of 2.8 / speed times as much)
//   spell_blessings_skirmisher_shattering_throw  Kit: bound to the Shattering Throw clone. Takes off the shield-type invulnerabilities (what the stock script of 64380 does)
//   spell_blessings_skirmisher_thunderous_hurl   Boon: a critical Hurl shakes the ground around its target (a share of the damage and a slow)
//   spell_blessings_skirmisher_whirling_daggers  Boon: every sixth Hurl that hits whirls daggers into the enemies around the target
//   spell_blessings_skirmisher_broad_shatter     Boon: a Shattering Throw that hits is thrown again, free, at the enemies around the target
//   spell_blessings_skirmisher_storm_of_javelins Boon: a critical Hurl, Heroic Throw or Ricochet calls a javelin down on a random enemy near the warrior
//   spell_blessings_skirmisher_perfect_throw     Boon: the fifth Hurl in a row is a sure critical strike and readies Heroic Throw
//   spell_blessings_skirmisher_javelin_storm     Favor of the Javelin Storm: every fourth Hurl that hits sends a volley at the enemies around the target
//   spell_blessings_skirmisher_pinning_throws    Favor of the Pinning Throws: slows from Hurl and Heroic Throw, and a root when both landed within the slow
//   MoonlitSkirmisherUnitScript                  UnitScript: Pinned Down (thrown weapons) and Iron Cage (all damage) hit harder against an enemy the warrior has slowed or rooted
//   MoonlitSkirmisherSpellScript                 AllSpellScript: Iron Cage, an enemy the warrior has slowed or rooted cannot start a charge, leap or jump spell
//
// Spell ids: Hurl 2,040,020 and Ricochet 2,040,030 are the hand-written kit spells (gen_blessings_spells.py), everything else comes from the Skirmisher registry block 2,040,100 (the units are
// listed at the top of tools/talents/recipes/skirmisher.py, which asserts the ids named below). The amounts, ranges and helper spells of the Boons and Favors are read from the effects of their own
// passives, so the spell data stays the one place a number is written. A script on a passive only runs while the Boon or Favor is there; the two global scripts first look for the Boon's own aura.
// ~Moonlit Nights Team

#include "AllSpellScript.h"
#include "Cell.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
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

#include <algorithm>
#include <list>
#include <vector>

namespace
{
    enum SkirmisherSpells : uint32
    {
        SPELL_HURL             = 2040020,   // kit: the auto-repeat thrown attack
        SPELL_RICOCHET         = 2040030,   // kit: the chain throw
        SPELL_HEROIC_THROW     = 2040100,   // kit: Skirmisher recipe unit 0
        SPELL_SHATTERING_THROW = 2040110,   // kit: Skirmisher recipe unit 1
        SPELL_PINNED_DOWN      = 2040250,   // effect 0: amount = percent more damage from the thrown weapons against an enemy slowed or rooted by the warrior
        SPELL_IRON_CAGE        = 2040280    // effect 0: amount = percent more damage from the warrior against an enemy slowed or rooted by her; the same enemy cannot charge or leap
    };

    constexpr uint32 MAX_EXTRA_TARGETS = 10;     // the most enemies one whirl, volley or ground shake hits besides the target
    constexpr uint8 WHIRL_FULL_STACKS = 5;       // Whirling Daggers: the counter holds five hits when the sixth Hurl lands
    constexpr uint8 VOLLEY_FULL_STACKS = 3;      // Favor of the Javelin Storm: three hits are counted when the fourth Hurl lands
    constexpr uint8 PERFECT_FULL_STACKS = 4;     // Perfect Throw: four hits are counted when the fifth Hurl lands

    // Hurl, Heroic Throw, Shattering Throw and Ricochet: what "your thrown weapons" means for Pinned Down.
    bool IsThrownAbility(uint32 spellId)
    {
        return spellId == SPELL_HURL || spellId == SPELL_HEROIC_THROW || spellId == SPELL_SHATTERING_THROW || spellId == SPELL_RICOCHET;
    }

    // Every attackable enemy of `seeker` within `range` yards of `center` that `center` can see (`center` itself included when it qualifies).
    std::list<Unit*> EnemiesAround(Unit* seeker, Unit* center, float range)
    {
        std::list<Unit*> targets;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(center, seeker, range);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(center, targets, check);
        Cell::VisitObjects(center, searcher, range);
        targets.remove_if([seeker, center](Unit* unit) { return !seeker->IsValidAttackTarget(unit) || !center->IsWithinLOSInMap(unit); });
        return targets;
    }

    // Hit `target` as `caster` with one of the flat-hit helper spells (instant, no spell power, cannot miss or critically hit): the damage is exactly `amount` before armour.
    void CastFlat(Unit* caster, Unit* target, uint32 spellId, int32 amount)
    {
        if (!caster || !target || amount <= 0 || !target->IsAlive())
            return;

        caster->CastCustomSpell(spellId, SPELLVALUE_BASE_POINT0, amount - 1, target, true);     // the spell adds 1 to its base points
    }

    // `percent` percent of `damage` as a flat hit on every enemy within `range` yards of `center` except `center` (up to `limit` of them).
    void HitAround(Unit* caster, Unit* center, uint32 damage, int32 percent, float range, uint32 hitSpell, uint32 limit)
    {
        int32 const amount = int32(std::min<uint64>(uint64(damage) * uint64(std::max(percent, 0)) / 100, 2000000000));
        if (amount <= 0)
            return;

        uint32 count = 0;
        for (Unit* unit : EnemiesAround(caster, center, range))
        {
            if (unit == center || !unit->IsAlive())
                continue;

            CastFlat(caster, unit, hitSpell, amount);
            if (++count >= limit)
                break;
        }
    }

    // The counting Boons and the Favor of the Javelin Storm keep their count in a stacking buff on the warrior. When the buff already holds `fullStacks` stacks this throw is the counted one: the
    // buff is spent and true comes back. Otherwise one stack is added (casting the buff again adds a stack and renews it).
    bool CountThrow(Unit* caster, uint32 counterSpell, uint8 fullStacks)
    {
        Aura const* count = caster->GetAura(counterSpell);
        if (count && count->GetStackAmount() >= fullStacks)
        {
            caster->RemoveAurasDueToSpell(counterSpell);
            return true;
        }

        caster->CastSpell(caster, counterSpell, true);
        return false;
    }

    // Does `target` carry a slowing or immobilizing aura cast by `by`? (Movement speed decreases and roots: Piercing Howl, Shaken Ground, Pinned, Hobbling Hurl ...)
    bool IsSlowedOrRootedBy(Unit const* target, ObjectGuid by)
    {
        static AuraType const types[] = { SPELL_AURA_MOD_DECREASE_SPEED, SPELL_AURA_MOD_ROOT };
        for (AuraType type : types)
            for (AuraEffect const* aurEff : target->GetAuraEffectsByType(type))
                if (aurEff->GetCasterGUID() == by)
                    return true;

        return false;
    }
}

// Hurl. The core scales the weapon roll of a ranged weapon-damage spell by 2.8 / speed for a thrown weapon (Player::GetThrownSpellWeaponFactor) and adds the attack power of a 2.8 sec weapon. That is
// right for a spell fired now and then, but Hurl repeats at the weapon's own speed, which would make it hit 2.8 / speed times harder per second than the weapon's damage per second. Dividing the hit
// by the same factor gives exactly the damage of an ordinary swing (the roll once, the attack power of the weapon's real speed). Remove the binding of this script to get the unscaled hit.
class spell_blessings_skirmisher_hurl : public SpellScript
{
    PrepareSpellScript(spell_blessings_skirmisher_hurl);

    void HandleHit()
    {
        Unit* caster = GetCaster();
        Player* player = caster ? caster->ToPlayer() : nullptr;
        if (!player)
            return;

        float const factor = player->GetThrownSpellWeaponFactor();
        if (factor > 1.0f)
            SetHitDamage(int32(float(GetHitDamage()) / factor));
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_blessings_skirmisher_hurl::HandleHit);
    }
};

// Shattering Throw (the clone). Effect 0 is the script effect: it takes off the shield-type invulnerabilities (mechanic 29: Divine Shield, Ice Block ...) before the damage is dealt, exactly what the
// stock script of 64380 does. The spell itself pierces immunities (the attributes of 64380).
class spell_blessings_skirmisher_shattering_throw : public SpellScript
{
    PrepareSpellScript(spell_blessings_skirmisher_shattering_throw);

    void HandleScript(SpellEffIndex effIndex)
    {
        PreventHitDefaultEffect(effIndex);
        if (Unit* target = GetHitUnit())
            target->RemoveAurasWithMechanic(1ULL << MECHANIC_IMMUNE_SHIELD, AURA_REMOVE_BY_ENEMY_SPELL);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_blessings_skirmisher_shattering_throw::HandleScript, EFFECT_0, SPELL_EFFECT_SCRIPT_EFFECT);
    }
};

// Thunderous Hurl. Effect 0: amount = percent of the critical hit dealt again to every other enemy near the target, misc = yards, trigger spell = the flat hit. Effect 1: trigger spell = the slow.
// The target is slowed too.
class spell_blessings_skirmisher_thunderous_hurl : public AuraScript
{
    PrepareAuraScript(spell_blessings_skirmisher_thunderous_hurl);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        if (!victim || !damageInfo || !victim->IsAlive())
            return;

        SpellInfo const* info = GetSpellInfo();
        float const range = float(info->Effects[EFFECT_0].MiscValue);
        uint32 const hitSpell = info->Effects[EFFECT_0].TriggerSpell;
        uint32 const slowSpell = info->Effects[EFFECT_1].TriggerSpell;
        int32 const amount = int32(std::min<uint64>(uint64(damageInfo->GetDamage()) * uint64(std::max(aurEff->GetAmount(), 0)) / 100, 2000000000));

        uint32 count = 0;
        for (Unit* unit : EnemiesAround(caster, victim, range))
        {
            if (!unit->IsAlive())
                continue;

            if (unit != victim)
                CastFlat(caster, unit, hitSpell, amount);

            caster->CastSpell(unit, slowSpell, true);
            if (++count > MAX_EXTRA_TARGETS)       // the target and up to MAX_EXTRA_TARGETS others
                break;
        }
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_skirmisher_thunderous_hurl::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Whirling Daggers. Effect 0: amount = percent of the Hurl's damage, misc = yards, trigger spell = the counter (five stacks). Effect 1: trigger spell = the flat hit. The sixth Hurl that hits finds the
// counter full and spends it.
class spell_blessings_skirmisher_whirling_daggers : public AuraScript
{
    PrepareAuraScript(spell_blessings_skirmisher_whirling_daggers);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        SpellInfo const* info = GetSpellInfo();
        if (!CountThrow(caster, info->Effects[EFFECT_0].TriggerSpell, WHIRL_FULL_STACKS))
            return;

        Unit* victim = eventInfo.GetActionTarget();
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        if (!victim || !damageInfo || !victim->IsAlive())
            return;

        HitAround(caster, victim, damageInfo->GetDamage(), aurEff->GetAmount(), float(info->Effects[EFFECT_0].MiscValue), info->Effects[EFFECT_1].TriggerSpell, MAX_EXTRA_TARGETS);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_skirmisher_whirling_daggers::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Broad Shatter. Effect 0: misc = yards. The proc is a Shattering Throw that hit; the same spell is cast, triggered (no cost, no cooldown, and a triggered spell cannot feed this proc again), at
// every other enemy near the target.
class spell_blessings_skirmisher_broad_shatter : public AuraScript
{
    PrepareAuraScript(spell_blessings_skirmisher_broad_shatter);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        SpellInfo const* thrown = eventInfo.GetSpellInfo();
        if (!victim || !thrown || thrown->Id != SPELL_SHATTERING_THROW)
            return;

        float const range = float(GetSpellInfo()->Effects[EFFECT_0].MiscValue);
        uint32 count = 0;
        for (Unit* unit : EnemiesAround(caster, victim, range))
        {
            if (unit == victim || !unit->IsAlive())
                continue;

            caster->CastSpell(unit, SPELL_SHATTERING_THROW, true);
            if (++count >= MAX_EXTRA_TARGETS)
                break;
        }
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_skirmisher_broad_shatter::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Storm of Javelins. Effect 0: amount = percent of the critical hit, misc = yards (around the warrior), trigger spell = the flat hit. One random living enemy in range takes the javelin.
class spell_blessings_skirmisher_storm_of_javelins : public AuraScript
{
    PrepareAuraScript(spell_blessings_skirmisher_storm_of_javelins);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        if (!damageInfo)
            return;

        SpellInfo const* info = GetSpellInfo();
        std::vector<Unit*> candidates;
        for (Unit* unit : EnemiesAround(caster, caster, float(info->Effects[EFFECT_0].MiscValue)))
            if (unit->IsAlive())
                candidates.push_back(unit);

        if (candidates.empty())
            return;

        int32 const amount = int32(std::min<uint64>(uint64(damageInfo->GetDamage()) * uint64(std::max(aurEff->GetAmount(), 0)) / 100, 2000000000));
        CastFlat(caster, candidates[urand(0, uint32(candidates.size() - 1))], info->Effects[EFFECT_0].TriggerSpell, amount);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_skirmisher_storm_of_javelins::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Perfect Throw. Effect 0: trigger spell = the counter (four stacks). Effect 1: trigger spell = the sure critical strike (one charge, spent by the next Hurl). Each Hurl that hits adds a stack; the
// hit that brings the count to four arms the sure critical strike for the next Hurl; the fifth Hurl that hits finds the counter full, spends it and readies Heroic Throw.
class spell_blessings_skirmisher_perfect_throw : public AuraScript
{
    PrepareAuraScript(spell_blessings_skirmisher_perfect_throw);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        SpellInfo const* info = GetSpellInfo();
        uint32 const counterSpell = info->Effects[EFFECT_0].TriggerSpell;
        Aura const* count = caster->GetAura(counterSpell);
        uint8 const stacks = count ? count->GetStackAmount() : 0;
        if (stacks >= PERFECT_FULL_STACKS)
        {
            caster->RemoveAurasDueToSpell(counterSpell);
            if (Player* player = caster->ToPlayer())
                player->RemoveSpellCooldown(SPELL_HEROIC_THROW, true);

            return;
        }

        caster->CastSpell(caster, counterSpell, true);
        if (stacks + 1 == PERFECT_FULL_STACKS)
            caster->CastSpell(caster, info->Effects[EFFECT_1].TriggerSpell, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_skirmisher_perfect_throw::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Favor of the Javelin Storm. Effect 0: amount = percent of the Hurl's damage per javelin, misc = yards, trigger spell = the counter (three stacks). Effect 1: amount = most enemies hit, trigger
// spell = the flat hit. The fourth Hurl that hits finds the counter full, spends it and sends the volley at the other enemies near the target.
class spell_blessings_skirmisher_javelin_storm : public AuraScript
{
    PrepareAuraScript(spell_blessings_skirmisher_javelin_storm);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        SpellInfo const* info = GetSpellInfo();
        if (!CountThrow(caster, info->Effects[EFFECT_0].TriggerSpell, VOLLEY_FULL_STACKS))
            return;

        Unit* victim = eventInfo.GetActionTarget();
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        AuraEffect const* limitEffect = GetEffect(EFFECT_1);
        if (!victim || !damageInfo || !victim->IsAlive() || !limitEffect)
            return;

        HitAround(caster, victim, damageInfo->GetDamage(), aurEff->GetAmount(), float(info->Effects[EFFECT_0].MiscValue), info->Effects[EFFECT_1].TriggerSpell,
                  uint32(std::max(limitEffect->GetAmount(), 0)));
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_skirmisher_javelin_storm::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Favor of the Pinning Throws. Effect 0: trigger spell = the slow of a Hurl, misc = the per-target rest mark; effect 1: trigger spell = the slow of a Heroic Throw; effect 2: trigger spell = the root.
// The two slows double as the marks "struck by Hurl" and "struck by Heroic Throw" (each 6 sec, cast by the warrior). A hit that finds the other mark on the target, and no rest mark, roots the target
// and sets the rest mark (15 sec): so a target is rooted at most once every 15 sec, and only when both throws landed within the slow.
class spell_blessings_skirmisher_pinning_throws : public AuraScript
{
    PrepareAuraScript(spell_blessings_skirmisher_pinning_throws);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        SpellInfo const* thrown = eventInfo.GetSpellInfo();
        if (!victim || !thrown || victim == caster || !victim->IsAlive())
            return;

        SpellInfo const* info = GetSpellInfo();
        uint32 const hurlSlow = info->Effects[EFFECT_0].TriggerSpell;
        uint32 const heroicSlow = info->Effects[EFFECT_1].TriggerSpell;
        uint32 const rootSpell = info->Effects[EFFECT_2].TriggerSpell;
        uint32 const restMark = uint32(info->Effects[EFFECT_0].MiscValue);

        uint32 mine;
        uint32 other;
        if (thrown->Id == SPELL_HURL)
        {
            mine = hurlSlow;
            other = heroicSlow;
        }
        else if (thrown->Id == SPELL_HEROIC_THROW)
        {
            mine = heroicSlow;
            other = hurlSlow;
        }
        else
            return;

        caster->CastSpell(victim, mine, true);
        if (victim->HasAura(other, caster->GetGUID()) && !victim->HasAura(restMark, caster->GetGUID()))
        {
            caster->CastSpell(victim, rootSpell, true);
            caster->CastSpell(victim, restMark, true);
        }
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_skirmisher_pinning_throws::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Pinned Down and Iron Cage: more damage from the warrior against an enemy that carries a slow or a root cast by her. Pinned Down covers her four thrown abilities, Iron Cage everything she deals
// (melee, spells and damage over time). Both amounts are the first effect of the Boon's own aura, so a warrior without the Boon is never touched.
class MoonlitSkirmisherUnitScript : public UnitScript
{
public:
    MoonlitSkirmisherUnitScript() : UnitScript("MoonlitSkirmisherUnitScript", true, { UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN, UNITHOOK_MODIFY_MELEE_DAMAGE, UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK }) { }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo) override
    {
        if (damage <= 0 || !spellInfo)
            return;

        int32 const percent = BonusPercent(target, attacker, IsThrownAbility(spellInfo->Id));
        if (percent > 0)
            damage += CalculatePct(damage, percent);
    }

    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        if (!damage)
            return;

        int32 const percent = BonusPercent(target, attacker, false);
        if (percent > 0)
            damage += CalculatePct(damage, percent);
    }

    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* spellInfo) override
    {
        if (!damage || !spellInfo)
            return;

        int32 const percent = BonusPercent(target, attacker, false);
        if (percent > 0)
            damage += CalculatePct(damage, percent);
    }

private:
    // The percent more damage that `attacker` (a Skirmisher with the Boons) deals to `target`; 0 when it has neither Boon, `target` is not an enemy, or is not slowed or rooted by her.
    static int32 BonusPercent(Unit const* target, Unit const* attacker, bool thrownAbility)
    {
        if (!target || !attacker || target == attacker || !attacker->IsPlayer())
            return 0;

        AuraEffect const* cage = attacker->GetAuraEffect(SPELL_IRON_CAGE, EFFECT_0);
        AuraEffect const* pinned = thrownAbility ? attacker->GetAuraEffect(SPELL_PINNED_DOWN, EFFECT_0) : nullptr;
        if (!cage && !pinned)
            return 0;

        if (!attacker->IsHostileTo(target) || !IsSlowedOrRootedBy(target, attacker->GetGUID()))
            return 0;

        return (cage ? cage->GetAmount() : 0) + (pinned ? pinned->GetAmount() : 0);
    }
};

// Iron Cage, second half: an enemy that a Skirmisher with Iron Cage has slowed or rooted cannot start a spell with a charge, leap or jump effect. Checked when the spell is cast, so only spells are
// stopped: a movement a creature's own script starts is not.
class MoonlitSkirmisherSpellScript : public AllSpellScript
{
public:
    MoonlitSkirmisherSpellScript() : AllSpellScript("MoonlitSkirmisherSpellScript", { ALLSPELLHOOK_ON_SPELL_CHECK_CAST }) { }

    void OnSpellCheckCast(Spell* spell, bool /*strict*/, SpellCastResult& res) override
    {
        if (!spell || res != SPELL_CAST_OK)
            return;

        SpellInfo const* info = spell->GetSpellInfo();
        if (!info || !(info->HasEffect(SPELL_EFFECT_CHARGE) || info->HasEffect(SPELL_EFFECT_CHARGE_DEST) || info->HasEffect(SPELL_EFFECT_LEAP) || info->HasEffect(SPELL_EFFECT_LEAP_BACK) ||
                       info->HasEffect(SPELL_EFFECT_JUMP) || info->HasEffect(SPELL_EFFECT_JUMP_DEST)))
            return;

        Unit* caster = spell->GetCaster() ? spell->GetCaster()->ToUnit() : nullptr;
        if (!caster || !IsCaged(caster))
            return;

        res = SPELL_FAILED_ROOTED;
    }

private:
    // Is `unit` slowed or rooted by a Skirmisher who has Iron Cage and is its enemy?
    static bool IsCaged(Unit* unit)
    {
        static AuraType const types[] = { SPELL_AURA_MOD_DECREASE_SPEED, SPELL_AURA_MOD_ROOT };
        for (AuraType type : types)
            for (AuraEffect const* aurEff : unit->GetAuraEffectsByType(type))
            {
                Unit* warrior = aurEff->GetCaster();
                if (warrior && warrior != unit && warrior->IsPlayer() && warrior->HasAura(SPELL_IRON_CAGE) && warrior->IsHostileTo(unit))
                    return true;
            }

        return false;
    }
};

void AddMoonlitBlessingsSkirmisherScripts()
{
    RegisterSpellScript(spell_blessings_skirmisher_hurl);
    RegisterSpellScript(spell_blessings_skirmisher_shattering_throw);
    RegisterSpellScript(spell_blessings_skirmisher_thunderous_hurl);
    RegisterSpellScript(spell_blessings_skirmisher_whirling_daggers);
    RegisterSpellScript(spell_blessings_skirmisher_broad_shatter);
    RegisterSpellScript(spell_blessings_skirmisher_storm_of_javelins);
    RegisterSpellScript(spell_blessings_skirmisher_perfect_throw);
    RegisterSpellScript(spell_blessings_skirmisher_javelin_storm);
    RegisterSpellScript(spell_blessings_skirmisher_pinning_throws);
    new MoonlitSkirmisherUnitScript();
    new MoonlitSkirmisherSpellScript();
}
