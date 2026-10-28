// Moonlit Blessings: C++ spell scripts for the Shadow Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/shadow.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_shadow_wand_reset      Voidspark and the Void Lasher's wand: a wand hit resets Mind Blast and makes the next one instant (free with Voidspark)
//   spell_blessings_shadow_damage_heal     Vampiric Touch Grace: a share of each Vampiric Touch tick heals the most injured ally
//   spell_blessings_shadow_mind_sovereign  Shadow Word: Death resets its own cooldown when it kills
//   spell_blessings_shadow_voidtouched     casting Shadow Word: Pain or Vampiric Touch spreads it once to the nearest other enemy
//   spell_blessings_shadow_bound_fiend     Favor of the Bound Fiend: keeps one permanent Shadowfiend beside the priest
//   spell_blessings_shadow_torment         Favor of Torment: every Mind Flay tick refreshes Shadow Word: Pain, Vampiric Touch and Devouring Plague
//   spell_blessings_shadow_void_lasher     Favor of the Void Lasher: spell haste becomes attack speed, tentacles lash for every running damage-over-time effect
//
// Spell ids come from the Shadow registry block 1,630,000 (units are listed at the top of tools/talents/recipes/shadow.py: the enum below must match them).
// ~Moonlit Nights Team

#include "Pet.h"   // BlessingsHelpers.h needs the full Pet type (it is not included there)
#include "BlessingsHelpers.h"
#include "Cell.h"
#include "CellImpl.h"
#include "Creature.h"
#include "GridNotifiers.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "TemporarySummon.h"
#include <algorithm>
#include <cmath>
#include <list>

namespace
{
    enum ShadowSpells : uint32
    {
        SPELL_VOIDSPARK          = 1630000,   // the Boon (its presence makes the wand's instant Mind Blast free)
        SPELL_VOIDSPARK_INSTANT  = 1630001,   // buff: the next Mind Blast is instant
        SPELL_VOIDSPARK_FOCUS    = 1630002,   // buff: the next Mind Blast costs no mana
        SPELL_BOUND_FIEND_BUFF   = 1630051,   // on the fiend: damage done +30%
        SPELL_BOUND_FIEND_SUMMON = 1630052,   // Shadowfiend clone that never expires
        SPELL_TENTACLE_LASH      = 1630071,   // the area lash; its damage is passed in by the script

        // Glyph of the Bound Fiend: the talent rewrites (first rank of each; ids run on by one per rank, see recipes/shadow.py)
        SPELL_FIEND_DARKNESS     = 1630120,   // 5 ranks: the fiend deals 2% more damage per rank
        SPELL_FIEND_TWISTED      = 1630130,   // 5 ranks: the fiend hits 1% harder per rank against targets with the priest's Shadow Word: Pain
        SPELL_FIEND_VEILED       = 1630140,   // 2 ranks: the fiend is called again 2 sec sooner per rank
        SPELL_FIEND_SPIRIT_TAP   = 1630150,   // 3 ranks: a kill by the fiend counts as the priest's own
        SPELL_FIEND_AFFINITY     = 1630160,   // 3 ranks: the fiend generates 20% more threat per rank
        SPELL_SPIRIT_TAP_TRIGGER = 15271,     // the stock Spirit Tap buff

        SPELL_MIND_BLAST_RANK_1  = 8092,
        SPELL_SHADOWFIEND_LEECH  = 28305      // the pet aura that hands mana back to the priest
    };

    constexpr uint32 NPC_TENTACLE = 1630090;                  // creature_template entry of the Tentacle of the Old Gods (still to be written, see the report)

    constexpr uint32 FLAG_MIND_BLAST = 0x2000;                // SpellFamilyFlags[0]
    constexpr uint32 FLAG_SHADOW_WORD_PAIN = 0x8000;          // [0]
    constexpr uint32 FLAG_DEVOURING_PLAGUE = 0x2000000;       // [0]
    constexpr uint32 FLAG_VAMPIRIC_TOUCH = 0x400;             // [1]

    constexpr uint8 DOT_KINDS = 3;                            // The effects of a spell that are auras (what a copy of the spell applied with AddAura should carry).
    uint8 AuraEffectMask(SpellInfo const* info)
    {
        uint8 mask = 0;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (info->Effects[i].IsAura())
                mask |= uint8(1 << i);

        return mask;
    }

    // 0 Shadow Word: Pain, 1 Vampiric Touch, 2 Devouring Plague
    constexpr float LASH_RANGE = 12.0f;                       // how far from its tentacle an enemy may be to be lashed
    constexpr float LASH_PER_LEVEL = 1.6f;                    // base damage of one lash per player level (about 60% of a Mind Sear tick; tune here)
    constexpr float TENTACLE_DISTANCE = 3.0f;                 // yards from the dot target
    constexpr uint32 TENTACLE_LIFETIME_MS = 30000;            // a tentacle that is still needed is called again when this runs out

    // The rank (0 = none) of a rewritten talent the player has: the variant spells run on by one per rank.
    uint32 VariantRank(Unit const* owner, uint32 firstRank, uint32 ranks)
    {
        for (uint32 rank = ranks; rank > 0; --rank)
            if (owner->HasAura(firstRank + rank - 1))
                return rank;

        return 0;
    }

    bool IsBoundFiend(Unit const* unit)
    {
        return unit && unit->GetEntry() == NPC_SHADOWFIEND && unit->GetUInt32Value(UNIT_CREATED_BY_SPELL) == SPELL_BOUND_FIEND_SUMMON;
    }

    Unit* FindFiend(Unit* owner)
    {
        for (Unit* controlled : owner->m_Controlled)
            if (controlled->GetEntry() == NPC_SHADOWFIEND && controlled->GetUInt32Value(UNIT_CREATED_BY_SPELL) == SPELL_BOUND_FIEND_SUMMON)
                return controlled;

        return nullptr;
    }

    void EnsureFiend(Unit* owner)
    {
        if (!owner || !owner->IsInWorld() || !owner->IsAlive())
            return;

        Unit* fiend = FindFiend(owner);
        if (!fiend)
        {
            owner->CastSpell(owner, SPELL_BOUND_FIEND_SUMMON, true);
            fiend = FindFiend(owner);
        }

        if (!fiend || !fiend->IsAlive())
            return;

        if (!fiend->HasAura(SPELL_BOUND_FIEND_BUFF))
            fiend->CastSpell(fiend, SPELL_BOUND_FIEND_BUFF, true);

        // the Glyph of the Bound Fiend's talent rewrites feed the fiend: Darkness adds damage, Shadow Affinity adds threat
        if (Aura* buff = fiend->GetAura(SPELL_BOUND_FIEND_BUFF))
        {
            if (AuraEffect* damage = buff->GetEffect(EFFECT_0))
                damage->ChangeAmount(30 + 2 * VariantRank(owner, SPELL_FIEND_DARKNESS, 5));
            if (AuraEffect* threat = buff->GetEffect(EFFECT_1))
                threat->ChangeAmount(20 * VariantRank(owner, SPELL_FIEND_AFFINITY, 3));
        }

        // a fiend that never leaves would hand back mana without end: the permanent one does not
        if (fiend->HasAura(SPELL_SHADOWFIEND_LEECH))
            fiend->RemoveAurasDueToSpell(SPELL_SHADOWFIEND_LEECH);
    }


    // Remove every cooldown of the spell's category (and the spell itself) so that the spell is ready again.
    void ResetCooldownOf(Player* player, uint32 spellId)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info)
            return;

        if (info->GetCategory())
            player->RemoveCategoryCooldown(info->GetCategory());

        player->RemoveSpellCooldown(info->Id, true);
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

    // 0 Shadow Word: Pain, 1 Vampiric Touch, 2 Devouring Plague, -1 anything else.
    int8 KindOfDot(SpellInfo const* info)
    {
        if (info->SpellFamilyName != SPELLFAMILY_PRIEST)
            return -1;

        if (info->SpellFamilyFlags[0] & FLAG_SHADOW_WORD_PAIN)
            return 0;

        if (info->SpellFamilyFlags[1] & FLAG_VAMPIRIC_TOUCH)
            return 1;

        if (info->SpellFamilyFlags[0] & FLAG_DEVOURING_PLAGUE)
            return 2;

        return -1;
    }

    // The periodic effect of the priest's own damage-over-time on `target`, or null when it is not running.
    AuraEffect* FindOwnDot(Unit* target, Unit* owner, uint8 kind)
    {
        ObjectGuid const casterGuid = owner->GetGUID();
        switch (kind)
        {
            case 0:
                return target->GetAuraEffect(SPELL_AURA_PERIODIC_DAMAGE, SPELLFAMILY_PRIEST, FLAG_SHADOW_WORD_PAIN, 0, 0, casterGuid);
            case 1:
                return target->GetAuraEffect(SPELL_AURA_PERIODIC_DAMAGE, SPELLFAMILY_PRIEST, 0, FLAG_VAMPIRIC_TOUCH, 0, casterGuid);
            default:
                return target->GetAuraEffect(SPELL_AURA_PERIODIC_LEECH, SPELLFAMILY_PRIEST, FLAG_DEVOURING_PLAGUE, 0, 0, casterGuid);
        }
    }

    // Wand hit: Mind Blast is ready again and the next one is instant (and free with Voidspark).
    void GrantVoidMindBlast(Player* player)
    {
        ResetCooldownOf(player, SPELL_MIND_BLAST_RANK_1);
        player->CastSpell(player, SPELL_VOIDSPARK_INSTANT, true);
        if (player->HasAura(SPELL_VOIDSPARK))
            player->CastSpell(player, SPELL_VOIDSPARK_FOCUS, true);
    }
}

// Voidspark (Boon) and the Void Lasher's wand: bound to a passive with a dummy first effect that procs on wand hits.
class spell_blessings_shadow_wand_reset : public AuraScript
{
    PrepareAuraScript(spell_blessings_shadow_wand_reset);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        if (Player* player = GetTarget()->ToPlayer())
            GrantVoidMindBlast(player);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_shadow_wand_reset::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Vampiric Touch Grace: the dummy effect holds the percent (amount) and the range in yards (misc value).
class spell_blessings_shadow_damage_heal : public AuraScript
{
    PrepareAuraScript(spell_blessings_shadow_damage_heal);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        DamageInfo const* damageInfo = eventInfo.GetDamageInfo();
        if (!damageInfo || !damageInfo->GetDamage())
            return;

        Unit* caster = GetTarget();
        float const range = float(GetSpellInfo()->Effects[EFFECT_0].MiscValue);
        if (Unit* ally = BlessingsHelpers::MostInjuredFriend(caster, caster, range))
            BlessingsHelpers::HealUnit(caster, ally, CalculatePct(damageInfo->GetDamage(), aurEff->GetAmount()), GetSpellInfo()->Id);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_shadow_damage_heal::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Mind Sovereign: when Shadow Word: Death kills its target it is ready again. The proc is in the second effect (the first one is the damage modifier).
class spell_blessings_shadow_mind_sovereign : public AuraScript
{
    PrepareAuraScript(spell_blessings_shadow_mind_sovereign);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Player* player = GetTarget()->ToPlayer();
        Unit* victim = eventInfo.GetActionTarget();
        SpellInfo const* info = eventInfo.GetSpellInfo();
        if (!player || !victim || !info || victim->IsAlive())
            return;

        ResetCooldownOf(player, info->Id);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_shadow_mind_sovereign::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

// Voidtouched: the second effect's misc value is the spread range in yards. Only the priest's own casts spread; the spread copy is applied with AddAura, so it triggers nothing.
class spell_blessings_shadow_voidtouched : public AuraScript
{
    PrepareAuraScript(spell_blessings_shadow_voidtouched);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        SpellInfo const* info = eventInfo.GetSpellInfo();
        if (!victim || !info)
            return;

        if (Spell const* spell = eventInfo.GetProcSpell())
            if (spell->IsTriggered())
                return;

        float const range = float(GetSpellInfo()->Effects[EFFECT_1].MiscValue);
        Unit* best = nullptr;
        float bestDistance = range + 1.0f;
        for (Unit* unit : EnemiesAround(caster, victim, range))
        {
            if (unit == victim || unit->GetAura(info->Id, caster->GetGUID()))
                continue;

            float const distance = victim->GetDistance(unit);
            if (distance < bestDistance)
            {
                best = unit;
                bestDistance = distance;
            }
        }

        if (best)
            caster->AddAura(info, AuraEffectMask(info), best);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_shadow_voidtouched::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

// Favor of the Bound Fiend: effect 0 is a dummy (apply and remove), effect 1 a periodic dummy that calls the fiend again if it died or was dismissed.
class spell_blessings_shadow_bound_fiend : public AuraScript
{
    PrepareAuraScript(spell_blessings_shadow_bound_fiend);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        EnsureFiend(GetTarget());
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* owner = GetTarget();
        std::list<Unit*> fiends;
        for (Unit* controlled : owner->m_Controlled)
            if (controlled->GetEntry() == NPC_SHADOWFIEND && controlled->GetUInt32Value(UNIT_CREATED_BY_SPELL) == SPELL_BOUND_FIEND_SUMMON)
                fiends.push_back(controlled);

        for (Unit* fiend : fiends)
            if (TempSummon* summon = fiend->ToTempSummon())
                summon->UnSummon();
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        PreventDefaultAction();
        EnsureFiend(GetTarget());
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_shadow_bound_fiend::HandleApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_shadow_bound_fiend::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_shadow_bound_fiend::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Favor of Torment: every Mind Flay tick refreshes the priest's Shadow Word: Pain, Vampiric Touch and Devouring Plague on the target (the way Pain and Suffering refreshes the first).
class spell_blessings_shadow_torment : public AuraScript
{
    PrepareAuraScript(spell_blessings_shadow_torment);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        if (!victim)
            return;

        for (uint8 kind = 0; kind < DOT_KINDS; ++kind)
            if (AuraEffect* effect = FindOwnDot(victim, caster, kind))
            {
                effect->GetBase()->RefreshTimersWithMods();
                effect->ChangeAmount(effect->CalculateAmount(effect->GetCaster()), false);
            }
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_shadow_torment::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

// Favor of the Void Lasher. The aura has three effects:
//   0  spell haste (a slowdown equal to the priest's whole spell haste, so casting speed is normal)
//   1  melee and ranged haste (the same haste, less what haste rating already gives the wand)
//   2  a periodic dummy, once a second: keeps both haste effects in step, keeps one tentacle per running damage-over-time effect and makes every tentacle lash
// The proc (Shadow Word: Pain, Vampiric Touch, Devouring Plague and Mind Blast casts) calls tentacles and, on Mind Blast, makes every tentacle lash once more.
class spell_blessings_shadow_void_lasher : public AuraScript
{
    PrepareAuraScript(spell_blessings_shadow_void_lasher);

    ObjectGuid _tentacle[DOT_KINDS];
    ObjectGuid _dotTarget[DOT_KINDS];

    // Casting speed is multiplied by 100 / (100 + haste) for every haste source and by (100 + slow) / 100 for our own slowdown. Dividing our own slowdown out of the current
    // casting speed gives the whole spell haste; the slowdown is then set to exactly that and the attack haste to the same haste (less the wand's own haste rating).
    void ConvertHaste(Player* player)
    {
        AuraEffect* slowdown = GetAura()->GetEffect(EFFECT_0);
        AuraEffect* attackHaste = GetAura()->GetEffect(EFFECT_1);
        float const castSpeed = player->GetFloatValue(UNIT_MOD_CAST_SPEED);
        if (!slowdown || !attackHaste || castSpeed <= 0.0f)
            return;

        float const ownFactor = (100.0f - float(slowdown->GetAmount())) / 100.0f;
        float const haste = ownFactor / castSpeed - 1.0f;
        float const rating = player->GetRatingBonusValue(CR_HASTE_RANGED) / 100.0f;
        int32 newSlowdown = 0;
        int32 newAttackHaste = 0;
        if (haste > 0.0f)
        {
            newSlowdown = -int32(std::lround(haste * 100.0f));
            newAttackHaste = std::max<int32>(0, int32(std::lround(((1.0f + haste) / (1.0f + rating) - 1.0f) * 100.0f)));
        }

        if (slowdown->GetAmount() != newSlowdown)
            slowdown->ChangeAmount(newSlowdown);

        if (attackHaste->GetAmount() != newAttackHaste)
            attackHaste->ChangeAmount(newAttackHaste);
    }

    // The enemy a tentacle lashes: the nearest one in combat with the priest within LASH_RANGE of the tentacle, the damage-over-time target first.
    static Unit* PickLashTarget(Unit* owner, Creature* tentacle, Unit* preferred)
    {
        Unit* best = nullptr;
        float bestDistance = LASH_RANGE + 1.0f;
        for (Unit* unit : EnemiesAround(owner, tentacle, LASH_RANGE))
        {
            if (!owner->IsInCombatWith(unit) && unit != preferred)
                continue;

            float const distance = unit == preferred ? 0.0f : tentacle->GetDistance(unit);
            if (distance < bestDistance)
            {
                best = unit;
                bestDistance = distance;
            }
        }

        return best;
    }

    void Lash(Unit* owner, Creature* tentacle, uint8 kind)
    {
        if (!owner->IsInCombat())
            return;

        Unit* victim = PickLashTarget(owner, tentacle, ObjectAccessor::GetUnit(*owner, _dotTarget[kind]));
        if (!victim)
            return;

        tentacle->SetFacingToObject(victim);
        int32 damage = std::max<int32>(1, int32(float(owner->GetLevel()) * LASH_PER_LEVEL));
        owner->CastCustomSpell(victim, SPELL_TENTACLE_LASH, &damage, nullptr, nullptr, true);
    }

    void EnsureTentacle(Unit* owner, uint8 kind, Unit* target)
    {
        Creature* existing = ObjectAccessor::GetCreature(*owner, _tentacle[kind]);
        if (existing && existing->IsAlive())
            return;

        if (!sObjectMgr->GetCreatureTemplate(NPC_TENTACLE))
        {
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                LOG_ERROR("scripts", "Moonlit Blessings: creature_template {} (Tentacle of the Old Gods) does not exist: the Favor of the Void Lasher cannot call tentacles.", NPC_TENTACLE);
            }

            return;
        }

        Position const pos = target->GetNearPosition(TENTACLE_DISTANCE, float(kind) * 2.1f + 1.0f);
        TempSummon* summon = owner->SummonCreature(NPC_TENTACLE, pos, TEMPSUMMON_TIMED_DESPAWN, TENTACLE_LIFETIME_MS);
        if (!summon)
            return;

        summon->SetFaction(owner->GetFaction());
        summon->SetReactState(REACT_PASSIVE);
        summon->SetImmuneToAll(true);
        summon->SetUnitFlag(UNIT_FLAG_NON_ATTACKABLE);
        _tentacle[kind] = summon->GetGUID();
    }

    void DismissTentacle(Unit* owner, uint8 kind)
    {
        if (Creature* tentacle = ObjectAccessor::GetCreature(*owner, _tentacle[kind]))
            if (TempSummon* summon = tentacle->ToTempSummon())
                summon->UnSummon();

        _tentacle[kind].Clear();
        _dotTarget[kind].Clear();
    }

    void HandleProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* info = eventInfo.GetSpellInfo();
        Unit* owner = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        if (!info || !victim || info->SpellFamilyName != SPELLFAMILY_PRIEST)
            return;

        if (info->SpellFamilyFlags[0] & FLAG_MIND_BLAST)
        {
            for (uint8 kind = 0; kind < DOT_KINDS; ++kind)
                if (Creature* tentacle = ObjectAccessor::GetCreature(*owner, _tentacle[kind]))
                    Lash(owner, tentacle, kind);

            return;
        }

        int8 const kind = KindOfDot(info);
        if (kind < 0)
            return;

        _dotTarget[kind] = victim->GetGUID();
        EnsureTentacle(owner, uint8(kind), victim);
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        PreventDefaultAction();
        Player* player = GetTarget()->ToPlayer();
        if (!player)
            return;

        ConvertHaste(player);
        for (uint8 kind = 0; kind < DOT_KINDS; ++kind)
        {
            Unit* target = ObjectAccessor::GetUnit(*player, _dotTarget[kind]);
            if (!player->IsAlive() || !target || !target->IsAlive() || !FindOwnDot(target, player, kind))
            {
                DismissTentacle(player, kind);
                continue;
            }

            EnsureTentacle(player, kind, target);
            if (Creature* tentacle = ObjectAccessor::GetCreature(*player, _tentacle[kind]))
                Lash(player, tentacle, kind);
        }
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        for (uint8 kind = 0; kind < DOT_KINDS; ++kind)
            DismissTentacle(GetTarget(), kind);
    }

    void Register() override
    {
        OnProc += AuraProcFn(spell_blessings_shadow_void_lasher::HandleProc);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_shadow_void_lasher::HandlePeriodic, EFFECT_2, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_shadow_void_lasher::HandleRemove, EFFECT_2, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Glyph of the Bound Fiend: the parts of the talent rewrites that live outside the priest's own auras.
//   Twisted Faith  the fiend hits harder against targets that carry the priest's Shadow Word: Pain
//   Veiled Shadows when the fiend dies it is called again sooner
//   Spirit Tap     a kill by the fiend counts as the priest's own (the stock chance and the experience or honor rule)
// Darkness and Shadow Affinity are carried by the fiend's buff (EnsureFiend sets its amounts). The priest's own talent effects are plain spell data.
class MoonlitBlessingsBoundFiendScript : public UnitScript
{
public:
    MoonlitBlessingsBoundFiendScript() : UnitScript("MoonlitBlessingsBoundFiendScript", true, { UNITHOOK_MODIFY_MELEE_DAMAGE, UNITHOOK_ON_UNIT_DEATH }) { }

    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        if (!target || !IsBoundFiend(attacker))
            return;

        Unit* owner = attacker->GetOwner();
        if (!owner)
            return;

        uint32 rank = VariantRank(owner, SPELL_FIEND_TWISTED, 5);
        if (rank && target->GetAuraEffect(SPELL_AURA_PERIODIC_DAMAGE, SPELLFAMILY_PRIEST, FLAG_SHADOW_WORD_PAIN, 0, 0, owner->GetGUID()))
            damage += damage * rank / 100;
    }

    void OnUnitDeath(Unit* unit, Unit* killer) override
    {
        if (IsBoundFiend(unit))
        {
            Unit* owner = unit->GetOwner();
            uint32 rank = owner ? VariantRank(owner, SPELL_FIEND_VEILED, 2) : 0;
            if (rank && owner->IsAlive())
            {
                // without the talent the fiend is called again by the favor's 5 second tick; each rank brings it back 2 sec sooner
                ObjectGuid ownerGuid = owner->GetGUID();
                owner->m_Events.AddEventAtOffset([owner, ownerGuid]()
                {
                    if (owner->GetGUID() == ownerGuid)
                        EnsureFiend(owner);
                }, std::chrono::milliseconds(5000 - 2000 * rank));
            }
            return;
        }

        if (!unit || !IsBoundFiend(killer))
            return;

        Player* owner = killer->GetOwner() ? killer->GetOwner()->ToPlayer() : nullptr;
        uint32 rank = owner ? VariantRank(owner, SPELL_FIEND_SPIRIT_TAP, 3) : 0;
        if (rank && owner->isHonorOrXPTarget(unit) && roll_chance_i(rank < 3 ? 33 * rank : 100))
            owner->CastSpell(owner, SPELL_SPIRIT_TAP_TRIGGER, true);
    }
};

void AddMoonlitBlessingsShadowScripts()

{
    RegisterSpellScript(spell_blessings_shadow_wand_reset);
    RegisterSpellScript(spell_blessings_shadow_damage_heal);
    RegisterSpellScript(spell_blessings_shadow_mind_sovereign);
    RegisterSpellScript(spell_blessings_shadow_voidtouched);
    RegisterSpellScript(spell_blessings_shadow_bound_fiend);
    RegisterSpellScript(spell_blessings_shadow_torment);
    RegisterSpellScript(spell_blessings_shadow_void_lasher);
    new MoonlitBlessingsBoundFiendScript();
}
