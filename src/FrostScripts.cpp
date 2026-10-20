// Moonlit Blessings: C++ spell scripts for the Frost Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/frost.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   MoonlitFrostUnitScript                      UnitScript with four hooks:
//                                                 Absolute Zero   a freeze you applied that is broken (damage or dispel) bursts for Frost damage
//                                                 Glacial Shatter a frozen enemy killed by you or your pet bursts; remembers which hits found the target frozen
//                                                 Shatterer       frozen targets take more damage from your spells (direct damage and damage over time)
//   spell_blessings_frost_glacial_shatter       Boon: a critical hit that found the target frozen deals extra Frost damage
//   spell_blessings_frost_elemental_pact        Boon: keeps the cost reduction on the mage and the shared spell power on the Water Elemental
//   spell_blessings_frost_winter_warden         Favor: buffs the Water Elemental and casts its Frost Nova
//   spell_blessings_frost_shatter_burst         the two burst spells: leaves out the enemy at the centre of the burst
//
// Spell ids come from the Frost registry block 1,430,000 (tools/talents/recipes/frost.py asserts the passives that are named here). Helper spells are read from the
// TriggerSpell column of the passives' dummy effects, so they are not hard-coded.
// ~Moonlit Nights Team

#include "Creature.h"
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
#include <map>
#include <mutex>
#include <utility>

namespace
{
    enum FrostSpells : uint32
    {
        SPELL_FROST_ABSOLUTE_ZERO   = 1430000,   // effect 2: percent of a Frost Nova hit the burst deals (TriggerSpell = the burst)
        SPELL_FROST_GLACIAL_SHATTER = 1430020,   // effect 1: percent of a critical hit dealt again (TriggerSpell = extra damage), effect 2: percent of a Frost Nova hit (TriggerSpell = kill burst)
        SPELL_FROST_SHATTERER       = 1430040    // effect 0: percent more damage to frozen targets
    };

    constexpr uint32 NPC_FROST_WATER_ELEMENTAL_TEMP = 510;       // Summon Water Elemental (31687 through 70907)
    constexpr uint32 NPC_FROST_WATER_ELEMENTAL_PERM = 37994;     // with the Glyph of Eternal Water (70908)

    constexpr uint32 FROST_BREAK_BURST_COOLDOWN_MS = 1000;       // one freeze-break burst per second per mage
    constexpr uint32 FROST_MARK_LIFETIME_MS = 1000;              // a "this hit found the target frozen" mark waits at most this long for its critical-hit proc
    constexpr float FROST_NOVA_RANGE = 8.0f;                     // the Winter Warden elemental casts its Frost Nova when its target is this close

    // A Frost Nova rank 1 hits for about 10 at level 1 and rank 6 for about 365 at level 80: a straight line between them stands in for "a Frost Nova hit" at any level.
    int32 FrostNovaDamage(Unit const* unit, int32 percent)
    {
        float const level = float(std::max<uint8>(unit->GetLevel(), 1));
        float const nova = 10.0f + (level - 1.0f) * (365.0f - 10.0f) / 79.0f;
        return std::max<int32>(1, int32(nova * float(percent) / 100.0f));
    }

    // The mage's Water Elemental (the temporary one, a guardian, or the permanent one of the Glyph of Eternal Water, a real Pet: the lookup that finds both), or null.
    Creature* FindWaterElemental(Unit* owner)
    {
        Creature* pet = ObjectAccessor::GetCreatureOrPetOrVehicle(*owner, owner->GetPetGUID());
        if (pet && pet->IsAlive() && (pet->GetEntry() == NPC_FROST_WATER_ELEMENTAL_TEMP || pet->GetEntry() == NPC_FROST_WATER_ELEMENTAL_PERM))
            return pet;

        return nullptr;
    }

    // Glacial Shatter's critical-hit bonus: the damage hook (before the hit lands, while the target still counts as frozen) leaves a mark for the pair (mage, target) and the
    // proc of the critical hit (after the hit, when a broken root is already gone) takes it. Maps update on several threads, so the little table has a lock.
    std::mutex sMarkLock;
    std::map<std::pair<ObjectGuid, ObjectGuid>, uint32> sMarks;

    void SetFrozenMark(ObjectGuid mage, ObjectGuid target, bool frozen)
    {
        std::lock_guard<std::mutex> lock(sMarkLock);
        uint32 const now = getMSTime();
        if (sMarks.size() > 16)
        {
            for (auto it = sMarks.begin(); it != sMarks.end();)
            {
                if (getMSTimeDiff(it->second, now) > FROST_MARK_LIFETIME_MS)
                    it = sMarks.erase(it);
                else
                    ++it;
            }
        }

        if (frozen)
            sMarks[{ mage, target }] = now;
        else
            sMarks.erase({ mage, target });
    }

    bool TakeFrozenMark(ObjectGuid mage, ObjectGuid target)
    {
        std::lock_guard<std::mutex> lock(sMarkLock);
        auto it = sMarks.find({ mage, target });
        if (it == sMarks.end())
            return false;

        bool const fresh = getMSTimeDiff(it->second, getMSTime()) <= FROST_MARK_LIFETIME_MS;
        sMarks.erase(it);
        return fresh;
    }

    // Cast a burst spell at a point as the player (the target selection of the burst spells is "enemies around the destination").
    void CastBurst(Player* player, uint32 spellId, int32 amount, Position const& pos)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!player || !info || !player->IsInWorld() || !player->IsAlive())
            return;

        SpellCastTargets targets;
        targets.SetDst(pos);
        CustomSpellValues values;
        values.AddSpellMod(SPELLVALUE_BASE_POINT0, amount);
        player->CastSpell(targets, info, &values, TRIGGERED_FULL_MASK);
    }

    // The burst waits a moment (1 ms of game time) so that it never runs inside the damage or aura code that decided on it.
    void ScheduleBurst(Unit* where, Player* player, uint32 spellId, int32 amount)
    {
        ObjectGuid const playerGuid = player->GetGUID();
        Position const pos = where->GetPosition();
        where->m_Events.AddEventAtOffset([where, playerGuid, spellId, amount, pos]()
        {
            CastBurst(ObjectAccessor::GetPlayer(*where, playerGuid), spellId, amount, pos);
        }, std::chrono::milliseconds(1));
    }
}

class MoonlitFrostUnitScript : public UnitScript
{
public:
    MoonlitFrostUnitScript() : UnitScript("MoonlitFrostUnitScript", true,
        { UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN, UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK, UNITHOOK_ON_DAMAGE, UNITHOOK_ON_AURA_REMOVE }) { }

    // Direct spell damage, before the hit lands: the target still counts as frozen here (a broken root is gone a moment later).
    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo) override
    {
        if (!attacker || !attacker->IsPlayer() || !target || !spellInfo || damage <= 0)
            return;

        Player* player = attacker->ToPlayer();
        AuraEffect const* shatterer = player->GetAuraEffect(SPELL_FROST_SHATTERER, EFFECT_0);
        bool const glacial = player->HasAura(SPELL_FROST_GLACIAL_SHATTER);
        if (!shatterer && !glacial)
            return;

        bool const frozen = target != player && !target->IsFriendlyTo(player) && target->HasAuraState(AURA_STATE_FROZEN, spellInfo, player);
        if (glacial)
            SetFrozenMark(player->GetGUID(), target->GetGUID(), frozen);

        if (frozen && shatterer)
            damage += CalculatePct(damage, shatterer->GetAmount());
    }

    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* spellInfo) override
    {
        if (!attacker || !attacker->IsPlayer() || !target || !spellInfo || !damage)
            return;

        Player* player = attacker->ToPlayer();
        AuraEffect const* shatterer = player->GetAuraEffect(SPELL_FROST_SHATTERER, EFFECT_0);
        if (!shatterer || target == player || target->IsFriendlyTo(player))      // this hook also runs for heals over time
            return;

        if (target->HasAuraState(AURA_STATE_FROZEN, spellInfo, player))
            damage += CalculatePct(damage, shatterer->GetAmount());
    }

    // Glacial Shatter, the kill: this runs inside Unit::DealDamage after absorbs and before the freeze can be broken, so a lethal hit on a frozen target is seen here
    // (OnUnitDeath comes after the auras are gone). The burst waits a moment and only goes off if the target really died.
    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (!attacker || !victim || !damage || damage < victim->GetHealth())
            return;

        Unit* owner = attacker->GetCharmerOrOwnerOrSelf();
        Player* player = owner ? owner->ToPlayer() : nullptr;
        if (!player || victim == player)
            return;

        AuraEffect const* glacial = player->GetAuraEffect(SPELL_FROST_GLACIAL_SHATTER, EFFECT_2);
        if (!glacial || victim->IsFriendlyTo(player) || !victim->HasAuraState(AURA_STATE_FROZEN))
            return;

        uint32 const burst = glacial->GetSpellInfo()->Effects[EFFECT_2].TriggerSpell;
        Unit* where = victim;
        ObjectGuid const playerGuid = player->GetGUID();
        int32 const amount = FrostNovaDamage(player, glacial->GetAmount());
        Position const pos = victim->GetPosition();
        where->m_Events.AddEventAtOffset([where, playerGuid, burst, amount, pos]()
        {
            if (!where->IsAlive())
                CastBurst(ObjectAccessor::GetPlayer(*where, playerGuid), burst, amount, pos);
        }, std::chrono::milliseconds(1));
    }

    // Absolute Zero: a freeze (a Frost root or stun) applied by the mage or the mage's pet that is removed by damage or by a dispel, not by running out or by death.
    void OnAuraRemove(Unit* unit, AuraApplication* aurApp, AuraRemoveMode mode) override
    {
        if (mode != AURA_REMOVE_BY_DEFAULT && mode != AURA_REMOVE_BY_ENEMY_SPELL)
            return;

        Aura* aura = aurApp->GetBase();
        if (!aura->GetCasterGUID().IsPlayer() && !aura->GetCasterGUID().IsAnyTypeCreature())
            return;

        if (aura->GetSpellInfo()->GetAuraState() != AURA_STATE_FROZEN)
            return;

        Unit* caster = aura->GetCaster();
        Unit* owner = caster ? caster->GetCharmerOrOwnerOrSelf() : nullptr;
        Player* player = owner ? owner->ToPlayer() : nullptr;
        if (!player || unit == player || !unit->IsAlive() || unit->IsFriendlyTo(player))
            return;

        AuraEffect const* zero = player->GetAuraEffect(SPELL_FROST_ABSOLUTE_ZERO, EFFECT_2);
        if (!zero)
            return;

        uint32 const burst = zero->GetSpellInfo()->Effects[EFFECT_2].TriggerSpell;
        if (player->HasSpellCooldown(burst))
            return;

        player->AddSpellCooldown(burst, 0, FROST_BREAK_BURST_COOLDOWN_MS);
        ScheduleBurst(unit, player, burst, FrostNovaDamage(player, zero->GetAmount()));
    }
};

// Glacial Shatter, the critical hit: effect 0 is the Deep Freeze modifier, effect 1 the extra damage (percent in the value, spell in TriggerSpell), effect 2 the kill burst.
class spell_blessings_frost_glacial_shatter : public AuraScript
{
    PrepareAuraScript(spell_blessings_frost_glacial_shatter);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return ValidateSpellInfo({ spellInfo->Effects[EFFECT_1].TriggerSpell, spellInfo->Effects[EFFECT_2].TriggerSpell });
    }

    // only a hit that the damage hook saw landing on a frozen target counts; the proc row cannot know that
    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        Unit* victim = eventInfo.GetActionTarget();
        if (!spellInfo || !victim || spellInfo->SpellFamilyName != SPELLFAMILY_MAGE)
            return false;

        return TakeFrozenMark(GetTarget()->GetGUID(), victim->GetGUID());
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        Unit* victim = eventInfo.GetActionTarget();
        if (!damageInfo || !victim || !victim->IsAlive())
            return;

        int32 const extra = CalculatePct(int32(damageInfo->GetDamage()), aurEff->GetAmount());
        if (extra > 0)
            GetTarget()->CastCustomSpell(aurEff->GetSpellInfo()->Effects[aurEff->GetEffIndex()].TriggerSpell, SPELLVALUE_BASE_POINT0, extra, victim, true, nullptr, aurEff);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_blessings_frost_glacial_shatter::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_blessings_frost_glacial_shatter::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

// Elemental Pact: effect 0 is periodic (every 2 sec) and names the cost buff for the mage, effect 1 holds the percent of the mage's Frost spell power the elemental is given and
// names the buff for the elemental. Both buffs are on only while the elemental lives; the amount follows the mage's spell power.
class spell_blessings_frost_elemental_pact : public AuraScript
{
    PrepareAuraScript(spell_blessings_frost_elemental_pact);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return ValidateSpellInfo({ spellInfo->Effects[EFFECT_0].TriggerSpell, spellInfo->Effects[EFFECT_1].TriggerSpell });
    }

    void Sync()
    {
        Unit* owner = GetTarget();
        AuraEffect const* share = GetEffect(EFFECT_1);
        if (!owner || !owner->IsInWorld() || !share)
            return;

        uint32 const costBuff = GetSpellInfo()->Effects[EFFECT_0].TriggerSpell;
        uint32 const petBuff = GetSpellInfo()->Effects[EFFECT_1].TriggerSpell;
        Creature* pet = FindWaterElemental(owner);
        if (!pet)
        {
            owner->RemoveAurasDueToSpell(costBuff);
            return;
        }

        if (!owner->HasAura(costBuff))
            owner->CastSpell(owner, costBuff, true);

        if (!pet->HasAura(petBuff))
            pet->CastSpell(pet, petBuff, true);

        if (AuraEffect* bonus = pet->GetAuraEffect(petBuff, EFFECT_0))
        {
            int32 const wanted = CalculatePct(std::max<int32>(0, owner->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_FROST)), share->GetAmount());
            if (bonus->GetAmount() != wanted)
                bonus->ChangeAmount(wanted);
        }
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Sync();
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* owner = GetTarget();
        owner->RemoveAurasDueToSpell(GetSpellInfo()->Effects[EFFECT_0].TriggerSpell);
        if (Creature* pet = ObjectAccessor::GetCreatureOrPetOrVehicle(*owner, owner->GetPetGUID()))
            pet->RemoveAurasDueToSpell(GetSpellInfo()->Effects[EFFECT_1].TriggerSpell);
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        PreventDefaultAction();
        Sync();
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_frost_elemental_pact::HandleApply, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_frost_elemental_pact::HandleRemove, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_frost_elemental_pact::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Favor of the Winter Warden: effect 0 (dummy) names the buff and holds its percent, effect 1 (periodic, every 2 sec) names the elemental's Frost Nova, effect 2 holds the
// percent of a Frost Nova hit the elemental's nova deals. The elemental is the stock creature.
class spell_blessings_frost_winter_warden : public AuraScript
{
    PrepareAuraScript(spell_blessings_frost_winter_warden);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return ValidateSpellInfo({ spellInfo->Effects[EFFECT_0].TriggerSpell, spellInfo->Effects[EFFECT_1].TriggerSpell });
    }

    void Tick()
    {
        Unit* owner = GetTarget();
        if (!owner || !owner->IsInWorld())
            return;

        Creature* pet = FindWaterElemental(owner);
        if (!pet)
            return;

        uint32 const buff = GetSpellInfo()->Effects[EFFECT_0].TriggerSpell;
        if (!pet->HasAura(buff))
        {
            // more maximum health must not leave the elemental looking wounded
            float const healthPct = pet->GetHealthPct();
            pet->CastSpell(pet, buff, true);
            pet->SetHealth(std::max<uint32>(1, pet->CountPctFromMaxHealth(int32(healthPct))));
        }

        uint32 const nova = GetSpellInfo()->Effects[EFFECT_1].TriggerSpell;
        SpellInfo const* novaInfo = sSpellMgr->GetSpellInfo(nova);
        AuraEffect const* novaShare = GetEffect(EFFECT_2);
        Unit* victim = pet->GetVictim();
        if (!novaInfo || !novaShare || !victim || !pet->IsWithinDist(victim, FROST_NOVA_RANGE) || pet->HasSpellCooldown(nova))
            return;

        pet->CastCustomSpell(nova, SPELLVALUE_BASE_POINT0, FrostNovaDamage(pet, novaShare->GetAmount()), pet, true);
        pet->AddSpellCooldown(nova, 0, novaInfo->RecoveryTime);
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Tick();
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* owner = GetTarget();
        if (Creature* pet = ObjectAccessor::GetCreatureOrPetOrVehicle(*owner, owner->GetPetGUID()))
            pet->RemoveAurasDueToSpell(GetSpellInfo()->Effects[EFFECT_0].TriggerSpell);
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        PreventDefaultAction();
        Tick();
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_frost_winter_warden::HandleApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_frost_winter_warden::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_frost_winter_warden::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// The two burst spells hit the enemies around the point where a frozen enemy broke or died; the enemy at that point (still alive when a freeze breaks) is left out.
class spell_blessings_frost_shatter_burst : public SpellScript
{
    PrepareSpellScript(spell_blessings_frost_shatter_burst);

    void FilterTargets(std::list<WorldObject*>& targets)
    {
        WorldLocation const* dest = GetExplTargetDest();
        if (!dest)
            return;

        targets.remove_if([dest](WorldObject* target)
        {
            return target->GetExactDist(dest) < 0.5f;
        });
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_blessings_frost_shatter_burst::FilterTargets, EFFECT_0, TARGET_UNIT_DEST_AREA_ENEMY);
    }
};

void AddMoonlitBlessingsFrostScripts()
{
    RegisterSpellScript(spell_blessings_frost_glacial_shatter);
    RegisterSpellScript(spell_blessings_frost_elemental_pact);
    RegisterSpellScript(spell_blessings_frost_winter_warden);
    RegisterSpellScript(spell_blessings_frost_shatter_burst);
    new MoonlitFrostUnitScript();
}
