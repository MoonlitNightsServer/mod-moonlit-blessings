// Moonlit Blessings: C++ spell scripts for the Destruction Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/destruction.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_destruction_shadowrain          Boon: every Rain of Fire hit also does shadow damage equal to a share of the fire damage
//   spell_blessings_destruction_brimstone_curtain   Boon: 20% less damage taken while a Rain of Fire or Hellfire channel runs (checked twice a second)
//   spell_blessings_destruction_chaos_unbound       Keystone: counts Chaos Bolt casts, every fourth one is free of mana
//   spell_blessings_destruction_cinder_lord         Keystone: Conflagrate keeps half of the Immolate it would consume and adds time to it
//   spell_blessings_destruction_hellrain            Keystone: watches the Rain of Fire ground and casts a Shadowfury at its centre when it runs its full course
//   spell_blessings_destruction_chaos_caller        Favor: a critical hit of a Destruction spell shortens the Chaos Bolt cooldown
//   MoonlitDestructionSpreadScript                  Favor of the Immolator: an Immolate passes on when its carrier dies
//
// Spell ids come from the Destruction registry block 1,930,000 (tools/talents/recipes/destruction.py asserts the units and the stock ids that are named here).
// The amounts of the Boons are read from the effects of their own passives, so the spell data stays the one place a number is written.
// An AuraScript exists only on the aura of its own Boon or Favor, so none of the handlers needs to look for the Boon again.
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
#include "UnitScript.h"

#include <algorithm>
#include <chrono>
#include <list>

namespace
{
    enum DestructionSpells : uint32
    {
        SPELL_SHADOWRAIN            = 1930020,   // passive: effect 0 = percent of the fire damage added as shadow damage, its trigger spell = the shadow hit
        SPELL_BRIMSTONE_CURTAIN     = 1930030,   // passive: effect 1 is the twice-a-second check
        SPELL_BRIMSTONE_BUFF        = 1930031,   // buff: 20% less damage taken (2 sec)
        SPELL_CHAOS_UNBOUND         = 1930050,   // passive: effect 0 = every how many Chaos Bolts, its trigger spell = the free-cast buff
        SPELL_CINDER_LORD           = 1930060,   // passive: effect 1 = percent of the Immolate kept, effect 2 = milliseconds added to it
        SPELL_HELLRAIN              = 1930070,   // passive: effect 1 = internal cooldown of the closing Shadowfury in seconds
        SPELL_FAVOR_CHAOS_CALLER    = 1930080,   // passive: effect 1 = milliseconds taken off the Chaos Bolt cooldown
        SPELL_FAVOR_IMMOLATOR       = 1930090,   // passive: effect 2 = range in yards of the death spread (misc value)

        SPELL_BACKDRAFT_BUFF        = 54277,     // asserted by the recipe (not named here: the Backdraft grant is a plain proc trigger)
        SPELL_GLYPH_OF_CONFLAGRATE  = 56235      // while the caster has this aura the core's Conflagrate does not remove the Immolate (Spell::EffectSchoolDMG); asserted by the recipe
    };

    constexpr uint32 SHADOWFURY_RANKS[] = { 47847, 47846, 30414, 30413, 30283 };      // highest first; asserted by the recipe
    constexpr uint32 CHAOS_BOLT_RANKS[] = { 50796, 59170, 59171, 59172 };             // asserted by the recipe
    constexpr uint32 SHADOWFURY_DEFAULT = 30413;                                      // cast when the warlock knows no rank (level 60 rank)
    constexpr uint32 FAMILY_RAIN_OF_FIRE = 0x20;                                      // SpellFamilyFlags[0] of the Rain of Fire cast spells
    constexpr uint32 FAMILY_HELLFIRE = 0x40;                                          // SpellFamilyFlags[0] of the Hellfire cast spells
    constexpr uint32 FAMILY_IMMOLATE = 0x4;                                           // SpellFamilyFlags[0] of Immolate
    constexpr uint32 CONFLAGRATE_SETTLE_MS = 400;                                     // Conflagrate is instant: its effects are done long before this
    constexpr uint32 RAIN_WATCH_MS = 500;                                             // how often the Hellrain script looks at the ground
    constexpr uint32 RAIN_WATCH_START_TRIES = 6;                                      // the ground has this many looks (3 sec) to appear after the cast
    constexpr int32 RAIN_END_WINDOW_MS = 700;                                         // a ground that vanishes with less than this left ran its full course
    constexpr int32 MIN_COPY_DURATION_MS = 1500;                                      // an Immolate with less time left than this is not worth passing on

    bool IsRainOrHellfire(SpellInfo const* info)
    {
        return info && info->SpellFamilyName == SPELLFAMILY_WARLOCK && (info->SpellFamilyFlags[0] & (FAMILY_RAIN_OF_FIRE | FAMILY_HELLFIRE));
    }

    // Is the unit channeling Rain of Fire or Hellfire right now?
    bool IsChannelingRainOrHellfire(Unit const* unit)
    {
        Spell const* spell = unit->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        return spell && spell->getState() == SPELL_STATE_CASTING && IsRainOrHellfire(spell->GetSpellInfo());
    }

    // The Immolate of `caster` on `target` (null when there is none).
    AuraEffect* FindImmolate(Unit* caster, Unit* target)
    {
        return target->GetAuraEffect(SPELL_AURA_PERIODIC_DAMAGE, SPELLFAMILY_WARLOCK, FAMILY_IMMOLATE, 0, 0, caster->GetGUID());
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

    // What the Hellrain script remembers about one Rain of Fire. `ground` is the dynamic object it is bound to (so that a second rain cast over the first is not mistaken for it).
    struct RainWatch
    {
        uint32 rainId = 0;              // the Rain of Fire cast spell
        ObjectGuid ground;              // empty until the ground has been seen
        Position centre;                // where the ground was when last seen
        int32 left = 0;                 // milliseconds the ground had left when last seen
        uint32 tries = 0;               // looks spent waiting for the ground to appear
    };

    void StepRainWatch(Player* player, RainWatch watch);

    void ScheduleRainWatch(Player* player, RainWatch const& watch)
    {
        player->m_Events.AddEventAtOffset([player, watch]() { StepRainWatch(player, watch); }, std::chrono::milliseconds(RAIN_WATCH_MS));
    }

    // The Shadowfury the rain ends with: the highest rank the warlock knows, cast at the centre of the ground, free and without a cooldown.
    void CastClosingShadowfury(Player* player, Position const& centre)
    {
        uint32 spellId = SHADOWFURY_DEFAULT;
        for (uint32 const rank : SHADOWFURY_RANKS)
        {
            if (player->HasSpell(rank))
            {
                spellId = rank;
                break;
            }
        }

        player->CastSpell(centre.GetPositionX(), centre.GetPositionY(), centre.GetPositionZ(), spellId, true);
    }

    // One look at the ground: remember where it is and how long it has left; when it has gone, decide whether it ran its full course (the last look found it with almost no time left)
    // or was cancelled (a channel that is broken early takes its ground with it, and then there is no Shadowfury).
    void StepRainWatch(Player* player, RainWatch watch)
    {
        if (!player->IsInWorld() || !player->IsAlive())
            return;

        DynamicObject* ground = player->GetDynObject(watch.rainId);
        if (ground && (!watch.ground || ground->GetGUID() == watch.ground))
        {
            watch.ground = ground->GetGUID();
            watch.centre = ground->GetPosition();
            watch.left = ground->GetDuration();
        }
        else if (watch.ground)
        {
            // gone: ran out (a Shadowfury breaks), or cancelled early (nothing happens)
            AuraEffect const* passive = player->GetAuraEffect(SPELL_HELLRAIN, EFFECT_1);
            if (watch.left <= RAIN_END_WINDOW_MS && passive && !player->HasSpellCooldown(SPELL_HELLRAIN))
            {
                CastClosingShadowfury(player, watch.centre);
                player->AddSpellCooldown(SPELL_HELLRAIN, 0, uint32(std::max(passive->GetAmount(), 0)) * IN_MILLISECONDS);
            }

            return;
        }
        else if (++watch.tries > RAIN_WATCH_START_TRIES)
            return;     // the ground never appeared (the cast failed)

        ScheduleRainWatch(player, watch);
    }

    // After Conflagrate: the Immolate that the core was told to leave alone gets half of its time left plus the Boon's bonus, never more than its full length.
    void SettleImmolate(Player* player, ObjectGuid targetGuid, uint32 immolateId, int32 keepPercent, int32 addMs, bool removeGlyph)
    {
        if (removeGlyph)
            player->RemoveAurasDueToSpell(SPELL_GLYPH_OF_CONFLAGRATE);

        if (!player->IsInWorld())
            return;

        Unit* target = ObjectAccessor::GetUnit(*player, targetGuid);
        Aura* immolate = target ? target->GetAura(immolateId, player->GetGUID()) : nullptr;
        if (!immolate)
            return;

        int32 const duration = int32(CalculatePct(std::max(immolate->GetDuration(), 0), keepPercent)) + addMs;
        immolate->SetDuration(std::min(duration, immolate->GetMaxDuration()));
    }

    // Copy the Immolate `source` (an aura of `caster` on the dying `from`) to the nearest other enemy within `range` yards that does not carry the caster's Immolate yet: same damage per
    // tick, the time that was left.
    void PassImmolate(Player* caster, Unit* from, Aura const* source, float range)
    {
        SpellInfo const* info = source->GetSpellInfo();
        ObjectGuid const casterGuid = caster->GetGUID();

        Unit* nearest = nullptr;
        float nearestDistance = range + 1.0f;
        for (Unit* other : EnemiesAround(caster, from, range))
        {
            if (other == from || !other->IsAlive() || other->GetAura(info->Id, casterGuid) || FindImmolate(caster, other))
                continue;

            float const distance = from->GetDistance(other);
            if (distance < nearestDistance)
            {
                nearest = other;
                nearestDistance = distance;
            }
        }

        if (!nearest)
            return;

        uint8 effectMask = 0;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (source->HasEffect(i))
                effectMask |= uint8(1 << i);

        Aura* copy = caster->AddAura(info, effectMask, nearest);
        if (!copy)
            return;

        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            AuraEffect* effect = copy->GetEffect(i);
            AuraEffect const* original = source->GetEffect(i);
            if (effect && original && info->Effects[i].ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE)
                effect->SetAmount(original->GetAmount());   // SetAmount also stops the aura from being recalculated back to the base value
        }

        copy->SetDuration(source->GetDuration());
    }
}

// Shadowrain: effect 0 holds the percent (amount) and the shadow hit (trigger spell). The proc row selects the Rain of Fire damage spell, which the ground casts as a triggered spell.
class spell_blessings_destruction_shadowrain : public AuraScript
{
    PrepareAuraScript(spell_blessings_destruction_shadowrain);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return ValidateSpellInfo({ spellInfo->Effects[EFFECT_0].TriggerSpell });
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        Unit* caster = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        DamageInfo const* damageInfo = eventInfo.GetDamageInfo();
        if (!victim || !victim->IsAlive() || !damageInfo || !damageInfo->GetDamage())
            return;

        int32 const amount = int32(CalculatePct(damageInfo->GetDamage(), aurEff->GetAmount()));
        if (amount > 0)
            caster->CastCustomSpell(GetSpellInfo()->Effects[EFFECT_0].TriggerSpell, SPELLVALUE_BASE_POINT0, amount - 1, victim, true);     // the spell adds 1 to its base points
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_destruction_shadowrain::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Brimstone Curtain: twice a second, while the warlock channels Rain of Fire or Hellfire, the 20% damage reduction (2 sec buff) is renewed; when no such channel runs it is taken off.
// The pushback part is a spell modifier in the data (effect 0).
class spell_blessings_destruction_brimstone_curtain : public AuraScript
{
    PrepareAuraScript(spell_blessings_destruction_brimstone_curtain);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* caster = GetTarget();
        if (IsChannelingRainOrHellfire(caster))
        {
            if (Aura* buff = caster->GetAura(SPELL_BRIMSTONE_BUFF))
                buff->RefreshDuration();
            else
                caster->CastSpell(caster, SPELL_BRIMSTONE_BUFF, true);
        }
        else
            caster->RemoveAurasDueToSpell(SPELL_BRIMSTONE_BUFF);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_destruction_brimstone_curtain::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Chaos Unbound: the proc row selects the Chaos Bolt cast (cast phase, so a cast that fails to go off is not counted). Effect 0: amount = N (every Nth Chaos Bolt is free), trigger
// spell = the free-cast buff. After the (N-1)th cast the buff is cast on the warlock; the Nth cast uses it up and the count starts again. The count lives in this aura.
class spell_blessings_destruction_chaos_unbound : public AuraScript
{
    PrepareAuraScript(spell_blessings_destruction_chaos_unbound);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return ValidateSpellInfo({ spellInfo->Effects[EFFECT_0].TriggerSpell });
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();

        uint32 const every = uint32(std::max(aurEff->GetAmount(), 2));
        if (++_casts == every - 1)
        {
            Unit* caster = GetTarget();
            caster->CastSpell(caster, GetSpellInfo()->Effects[aurEff->GetEffIndex()].TriggerSpell, true);
        }
        else if (_casts >= every)
            _casts = 0;
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_destruction_chaos_unbound::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }

private:
    uint32 _casts = 0;
};

// Cinder Lord. The proc row selects the Conflagrate cast (cast phase: the core has not yet looked at the Immolate; its cast-phase event has no target of its own, so the target is read
// from the spell). The core removes the Immolate during the cast unless the caster has Glyph of Conflagrate, so for this one cast the aura is lent to the warlock (unless he has
// the real glyph) and taken away again a moment later; at the same moment the Immolate that stayed gets half of its time left plus the bonus. Conflagrate still reads the full
// Immolate for its damage, as before. Only the warlock's own Immolate is touched; Shadowflame (which the core also lets Conflagrate consume) is left to the core.
// Effect 1: amount = percent of the remaining time kept. Effect 2: amount = milliseconds added.
class spell_blessings_destruction_cinder_lord : public AuraScript
{
    PrepareAuraScript(spell_blessings_destruction_cinder_lord);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        Player* player = GetTarget()->ToPlayer();
        Spell const* spell = eventInfo.GetProcSpell();
        Unit* target = spell ? spell->m_targets.GetUnitTarget() : nullptr;
        AuraEffect const* bonus = GetEffect(EFFECT_2);
        if (!player || !target || !target->IsAlive() || !bonus)
            return;

        AuraEffect const* immolate = FindImmolate(player, target);
        if (!immolate)
            return;

        bool lentGlyph = false;
        if (!player->HasAura(SPELL_GLYPH_OF_CONFLAGRATE))
            lentGlyph = player->AddAura(SPELL_GLYPH_OF_CONFLAGRATE, player) != nullptr;

        ObjectGuid const targetGuid = target->GetGUID();
        uint32 const immolateId = immolate->GetId();
        int32 const keepPercent = std::max(aurEff->GetAmount(), 0);
        int32 const addMs = std::max(bonus->GetAmount(), 0);
        player->m_Events.AddEventAtOffset([player, targetGuid, immolateId, keepPercent, addMs, lentGlyph]()
        {
            SettleImmolate(player, targetGuid, immolateId, keepPercent, addMs, lentGlyph);
        }, std::chrono::milliseconds(CONFLAGRATE_SETTLE_MS));
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_destruction_cinder_lord::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

// Hellrain. The proc row selects the Rain of Fire cast (cast phase: the ground does not exist yet). The script starts a watch that looks at the ground twice a second (one lookup
// of the warlock's own dynamic object, nothing is scanned) and casts the Shadowfury when the ground ends by running out. Effect 1: amount = the internal cooldown in seconds.
class spell_blessings_destruction_hellrain : public AuraScript
{
    PrepareAuraScript(spell_blessings_destruction_hellrain);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        Player* player = GetTarget()->ToPlayer();
        SpellInfo const* rain = eventInfo.GetSpellInfo();
        if (!player || !rain || !IsRainOrHellfire(rain) || !(rain->SpellFamilyFlags[0] & FAMILY_RAIN_OF_FIRE) || player->HasSpellCooldown(GetId()))
            return;

        RainWatch watch;
        watch.rainId = rain->Id;
        ScheduleRainWatch(player, watch);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_destruction_hellrain::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

// Favor of the Chaos Caller: the proc row selects the critical hits of the single target Destruction spells. Effect 1: amount = milliseconds taken off Chaos Bolt's cooldown. Chaos Bolt
// has a category cooldown that is stored under the rank that was cast, so every rank that is on cooldown is shortened (a rank that is ready is left alone).
class spell_blessings_destruction_chaos_caller : public AuraScript
{
    PrepareAuraScript(spell_blessings_destruction_chaos_caller);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();

        Player* player = GetTarget()->ToPlayer();
        if (!player || aurEff->GetAmount() <= 0)
            return;

        for (uint32 const rank : CHAOS_BOLT_RANKS)
            if (player->HasSpellCooldown(rank))
                player->ModifySpellCooldown(rank, -aurEff->GetAmount());
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_destruction_chaos_caller::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

// Favor of the Immolator, the death spread: one hook on every aura removal; everything is rejected early unless the aura is the Immolate of a player, removed by the death of its
// carrier, and the warlock has the Favor. (A copy that dies passes on again, as long as at least 1.5 sec of it is left.)
class MoonlitDestructionSpreadScript : public UnitScript
{
public:
    MoonlitDestructionSpreadScript() : UnitScript("MoonlitDestructionSpreadScript", true, { UNITHOOK_ON_AURA_REMOVE }) { }

    void OnAuraRemove(Unit* unit, AuraApplication* aurApp, AuraRemoveMode mode) override
    {
        if (!unit || !aurApp || mode != AURA_REMOVE_BY_DEATH)
            return;

        Aura const* aura = aurApp->GetBase();
        if (!aura || aura->GetType() != UNIT_AURA_TYPE || !aura->GetCasterGUID().IsPlayer())
            return;

        SpellInfo const* info = aura->GetSpellInfo();
        if (info->SpellFamilyName != SPELLFAMILY_WARLOCK || !(info->SpellFamilyFlags[0] & FAMILY_IMMOLATE) || !info->HasAura(SPELL_AURA_PERIODIC_DAMAGE) || aura->GetDuration() < MIN_COPY_DURATION_MS)
            return;

        Player* caster = ObjectAccessor::GetPlayer(*unit, aura->GetCasterGUID());
        if (!caster || !caster->IsAlive() || !caster->IsInWorld())
            return;

        AuraEffect const* favor = caster->GetAuraEffect(SPELL_FAVOR_IMMOLATOR, EFFECT_2);
        if (!favor)
            return;

        PassImmolate(caster, unit, aura, float(favor->GetSpellInfo()->Effects[EFFECT_2].MiscValue));
    }
};

void AddMoonlitBlessingsDestructionScripts()
{
    RegisterSpellScript(spell_blessings_destruction_shadowrain);
    RegisterSpellScript(spell_blessings_destruction_brimstone_curtain);
    RegisterSpellScript(spell_blessings_destruction_chaos_unbound);
    RegisterSpellScript(spell_blessings_destruction_cinder_lord);
    RegisterSpellScript(spell_blessings_destruction_hellrain);
    RegisterSpellScript(spell_blessings_destruction_chaos_caller);
    new MoonlitDestructionSpreadScript();
}
