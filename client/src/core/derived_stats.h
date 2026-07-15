#pragma once

#include <cstdint>

// WARNING: This file MUST stay in sync with server/internal/world/derived_stats.go.
// Specifically: ComputeDerivedStats function and all coefficient constants.
// If you change formulas in derived_stats.go, update this file IDENTICALLY.
// TECH_DEBT: long-term, server should send formulas/coefficients to client at login.

namespace rco::stats {

struct PrimaryStats {
    int32_t STR = 0;
    int32_t DEX = 0;
    int32_t INT = 0;
    int32_t WIS = 0;
    int32_t PER = 0;
};

struct DerivedStats {
    int32_t HealthMax = 0;
    float   HealthRegen = 0.0f;
    int32_t EnergyMax = 0;
    float   EnergyRegen = 0.0f;

    int32_t MeleeDefenseValue = 0;
    int32_t RangedDefenseValue = 0;
    int32_t MagicDefenseValue = 0;

    int32_t MeleeEvasionValue = 0;
    int32_t RangedEvasionValue = 0;
    int32_t MagicEvasionValue = 0;

    int32_t MeleeHitValue = 0;
    int32_t RangedHitValue = 0;
    int32_t MagicHitValue = 0;

    int32_t MeleeCritValue = 0;
    int32_t RangedCritValue = 0;
    int32_t MagicCritValue = 0;

    int32_t MeleeDmgMin = 0;
    int32_t MeleeDmgMax = 0;
    int32_t RangedDmgMin = 0;
    int32_t RangedDmgMax = 0;
    int32_t MagicDmgMin = 0;
    int32_t MagicDmgMax = 0;

    float   CritDamageMult = 0.0f;

    float   AttackSpeedMult = 0.0f;
    float   MovementSpeedMult = 0.0f;
    float   CooldownSpeedPct = 0.0f;

    float   SkillDamageBoostPct = 0.0f;
    float   BuffDurationPct = 0.0f;
    float   DebuffDurationPct = 0.0f;

    float   RangeBonusPct = 0.0f;
    int32_t BonusDamageFlat = 0;

    int32_t CCChanceValue = 0;
    int32_t CCResistanceValue = 0;

    int32_t DamageReductionFlat = 0;

    // NOT computed by ComputeDerivedStats()/formulas below — unlike every
    // other field here, AttackRange isn't derived from primary stats, it's
    // resolved server-side from the equipped weapon (world.ResolveAttackRange,
    // server/internal/net/client.go) and arrives purely over the network as
    // a trailing field on PFullStats (see main.cpp's kPFullStats parse).
    // 0.0f = not yet received (e.g. before the first PFullStats snapshot
    // after login) — callers must treat 0 as "unknown", not "melee range".
    float AttackRange = 0.0f;
};

constexpr int32_t healthBase = 100;
constexpr int32_t healthPerSTR = 8;
constexpr int32_t healthPerLevel = 15;
constexpr float healthRegenPerSTR = 0.3f;
constexpr float healthRegenPctOfMax = 0.01f; // 1%/s of HealthMax

constexpr int32_t energyBase = 50;
constexpr int32_t energyPerWIS = 4;
constexpr int32_t energyPerINT = 4;
constexpr int32_t energyPerLevel = 5;
constexpr float energyRegenPerWIS = 0.2f;
constexpr float energyRegenPctOfMax = 0.02f; // 2%/s of EnergyMax

constexpr int32_t meleeDefSTR = 5;
constexpr int32_t rangedDefSTR = 2;
constexpr int32_t rangedDefDEX = 2;
constexpr int32_t magicDefINT = 2;
constexpr int32_t magicDefWIS = 2;
constexpr int32_t defPerLevel = 2;

constexpr int32_t meleeEvasionDEX = 4;
constexpr int32_t rangedEvasionDEX = 5;
constexpr int32_t magicEvasionDEX = 2;
constexpr int32_t magicEvasionPER = 2;
constexpr int32_t evasionPerLevel = 1;

constexpr int32_t hitPER = 10;
constexpr int32_t meleeHitSTR = 5;
constexpr int32_t rangedHitDEX = 5;
constexpr int32_t magicHitINT = 5;
constexpr int32_t hitPerLevel = 2;

constexpr int32_t meleeCritDEX = 8;
constexpr int32_t rangedCritDEX = 10;
constexpr int32_t magicCritDEX = 5;
constexpr int32_t magicCritINT = 5;
constexpr int32_t critPerLevel = 1;

constexpr float meleeDmgMinFromSTR = 1.5f;
constexpr float meleeDmgMaxFromSTR = 2.0f;
constexpr float meleeDmgMaxFromDEX = 0.5f;

constexpr float rangedDmgMinFromDEX = meleeDmgMinFromSTR;
constexpr float rangedDmgMaxFromDEX = meleeDmgMaxFromSTR;
constexpr float rangedDmgMaxFromPER = meleeDmgMaxFromDEX;

constexpr float magicDmgMinFromINT = 1.5f;
constexpr float magicDmgMaxFromINT = 2.0f;
constexpr float magicDmgMaxFromPER = 0.5f;

constexpr float critDmgBase = 1.5f;
constexpr float critDmgPerDEX = 0.01f;
constexpr float critDmgCap = 3.0f;

constexpr float attackSpeedBase = 1.0f;
constexpr float attackSpeedPerDEX = 0.005f;
constexpr float attackSpeedCap = 1.5f;

constexpr float moveSpeedBase = 1.0f;
constexpr float moveSpeedPerDEX = 0.002f;
constexpr float moveSpeedCap = 1.3f;

constexpr float cooldownSpdPerWIS = 0.002f;
constexpr float cooldownSpdCap = 0.30f;

constexpr float skillDmgBoostPerINT = 0.002f;
constexpr float skillDmgBoostPerWIS = 0.001f;
constexpr float skillDmgBoostCap = 0.50f;

constexpr float buffDurationPerPER = 0.003f;
constexpr float buffDurationCap = 0.50f;

constexpr float debuffDurationPerPER = 0.003f;
constexpr float debuffDurationCap = 0.50f;

constexpr float rangeBonusPerPER = 0.002f;
constexpr float rangeBonusCap = 0.30f;

constexpr int32_t bonusDmgPerLevel = 2;

constexpr int32_t ccChancePER = 5;
constexpr int32_t ccChancePerLevel = 1;

constexpr int32_t ccResistancePER = 7;
constexpr int32_t ccResistancePerLevel = 1;

constexpr int32_t critValueSoftcap = 1500;
constexpr float critValueCap = 0.70f;

constexpr int32_t evasionSoftcap = 2000;
constexpr float evasionCap = 0.60f;

constexpr int32_t defenseSoftcap = 3000;
constexpr float defenseCap = 0.80f;

constexpr int32_t ccValueSoftcap = 1500;
constexpr float ccValueCap = 0.60f;

inline float clampFloat(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

inline void MeleeDamageRange(PrimaryStats primary, int32_t weaponDmg, int32_t& min_out, int32_t& max_out) {
    min_out = weaponDmg + static_cast<int32_t>(static_cast<float>(primary.STR) * meleeDmgMinFromSTR);
    max_out = weaponDmg + static_cast<int32_t>(
        static_cast<float>(primary.STR) * meleeDmgMaxFromSTR +
        static_cast<float>(primary.DEX) * meleeDmgMaxFromDEX);
    if (min_out > max_out) {
        max_out = min_out;
    }
}

inline void RangedDamageRange(PrimaryStats primary, int32_t weaponDmg, int32_t& min_out, int32_t& max_out) {
    min_out = weaponDmg + static_cast<int32_t>(static_cast<float>(primary.DEX) * rangedDmgMinFromDEX);
    max_out = weaponDmg + static_cast<int32_t>(
        static_cast<float>(primary.DEX) * rangedDmgMaxFromDEX +
        static_cast<float>(primary.PER) * rangedDmgMaxFromPER);
    if (min_out > max_out) {
        max_out = min_out;
    }
}

inline void MagicDamageRange(PrimaryStats primary, int32_t weaponDmg, int32_t& min_out, int32_t& max_out) {
    min_out = weaponDmg + static_cast<int32_t>(static_cast<float>(primary.INT) * magicDmgMinFromINT);
    max_out = weaponDmg + static_cast<int32_t>(
        static_cast<float>(primary.INT) * magicDmgMaxFromINT +
        static_cast<float>(primary.PER) * magicDmgMaxFromPER);
    if (min_out > max_out) {
        max_out = min_out;
    }
}

inline float ValueToPercent(int32_t value, float cap, int32_t softcap) {
    if (value <= 0) {
        return 0.0f;
    }
    const float pct = cap * static_cast<float>(value) /
                      static_cast<float>(value + softcap);
    if (pct > cap) {
        return cap;
    }
    return pct;
}

inline DerivedStats ComputeDerivedStats(PrimaryStats primary, int32_t level, int32_t weaponDmg, int32_t armor) {
    DerivedStats d{};

    d.HealthMax = healthBase + primary.STR * healthPerSTR + level * healthPerLevel;
    d.HealthRegen = static_cast<float>(d.HealthMax) * healthRegenPctOfMax + static_cast<float>(primary.STR) * healthRegenPerSTR;
    d.EnergyMax = energyBase + primary.WIS * energyPerWIS + primary.INT * energyPerINT + level * energyPerLevel;
    d.EnergyRegen = static_cast<float>(d.EnergyMax) * energyRegenPctOfMax + static_cast<float>(primary.WIS) * energyRegenPerWIS;

    d.MeleeDefenseValue = primary.STR * meleeDefSTR + level * defPerLevel + armor;
    d.RangedDefenseValue = primary.STR * rangedDefSTR + primary.DEX * rangedDefDEX + level * defPerLevel + armor;
    d.MagicDefenseValue = primary.INT * magicDefINT + primary.WIS * magicDefWIS + level * defPerLevel + armor;

    d.MeleeEvasionValue = primary.DEX * meleeEvasionDEX + level * evasionPerLevel;
    d.RangedEvasionValue = primary.DEX * rangedEvasionDEX + level * evasionPerLevel;
    d.MagicEvasionValue = primary.DEX * magicEvasionDEX + primary.PER * magicEvasionPER + level * evasionPerLevel;

    d.MeleeHitValue = primary.PER * hitPER + primary.STR * meleeHitSTR + level * hitPerLevel;
    d.RangedHitValue = primary.PER * hitPER + primary.DEX * rangedHitDEX + level * hitPerLevel;
    d.MagicHitValue = primary.PER * hitPER + primary.INT * magicHitINT + level * hitPerLevel;

    d.MeleeCritValue = primary.DEX * meleeCritDEX + level * critPerLevel;
    d.RangedCritValue = primary.DEX * rangedCritDEX + level * critPerLevel;
    d.MagicCritValue = primary.DEX * magicCritDEX + primary.INT * magicCritINT + level * critPerLevel;

    MeleeDamageRange(primary, weaponDmg, d.MeleeDmgMin, d.MeleeDmgMax);
    RangedDamageRange(primary, weaponDmg, d.RangedDmgMin, d.RangedDmgMax);
    MagicDamageRange(primary, weaponDmg, d.MagicDmgMin, d.MagicDmgMax);

    d.CritDamageMult = clampFloat(
        critDmgBase + static_cast<float>(primary.DEX) * critDmgPerDEX,
        critDmgBase,
        critDmgCap);

    d.AttackSpeedMult = clampFloat(
        attackSpeedBase + static_cast<float>(primary.DEX) * attackSpeedPerDEX,
        attackSpeedBase,
        attackSpeedCap);
    d.MovementSpeedMult = clampFloat(
        moveSpeedBase + static_cast<float>(primary.DEX) * moveSpeedPerDEX,
        moveSpeedBase,
        moveSpeedCap);
    d.CooldownSpeedPct = clampFloat(
        static_cast<float>(primary.WIS) * cooldownSpdPerWIS,
        0.0f,
        cooldownSpdCap);

    d.SkillDamageBoostPct = clampFloat(
        static_cast<float>(primary.INT) * skillDmgBoostPerINT +
        static_cast<float>(primary.WIS) * skillDmgBoostPerWIS,
        0.0f,
        skillDmgBoostCap);
    d.BuffDurationPct = clampFloat(
        static_cast<float>(primary.PER) * buffDurationPerPER,
        0.0f,
        buffDurationCap);
    d.DebuffDurationPct = clampFloat(
        static_cast<float>(primary.PER) * debuffDurationPerPER,
        0.0f,
        debuffDurationCap);

    d.RangeBonusPct = clampFloat(
        static_cast<float>(primary.PER) * rangeBonusPerPER,
        0.0f,
        rangeBonusCap);
    d.BonusDamageFlat = level * bonusDmgPerLevel;

    d.CCChanceValue = primary.PER * ccChancePER + level * ccChancePerLevel;
    d.CCResistanceValue = primary.PER * ccResistancePER + level * ccResistancePerLevel;

    d.DamageReductionFlat = 0;
    return d;
}

} // namespace rco::stats
