/*
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 2 or any later version.
 */

#ifndef MOD_WATCH_CAMERA_H
#define MOD_WATCH_CAMERA_H

#include "SpellInfo.h"
#include <memory>

namespace Watch
{
inline std::unique_ptr<SpellInfo> CreateCameraSpell(SpellEntry const* entry, SpellInfo const& original)
{
    // Construct owned effect/condition data rather than shallow-copying SpellInfo.
    // Preserve the core's corrected attributes: bind sight is passive (no visible
    // aura slot) and no longer channeled. Only our private spell survives death.
    auto spell = std::make_unique<SpellInfo>(entry);
    spell->Attributes = original.Attributes;
    spell->AttributesEx = original.AttributesEx;
    spell->AttributesEx2 = original.AttributesEx2 | SPELL_ATTR2_ALLOW_DEAD_TARGET;
    spell->AttributesEx3 = original.AttributesEx3 | SPELL_ATTR3_ALLOW_AURA_WHILE_DEAD;
    spell->AttributesEx4 = original.AttributesEx4;
    spell->AttributesEx5 = original.AttributesEx5;
    spell->AttributesEx6 = original.AttributesEx6;
    spell->AttributesEx7 = original.AttributesEx7 & ~SPELL_ATTR7_DISABLE_AURA_WHILE_DEAD;
    spell->AttributesCu = original.AttributesCu | SPELL_ATTR0_CU_AURA_CANNOT_BE_SAVED;
    spell->_auraState = spell->LoadAuraState();
    spell->_spellSpecific = spell->LoadSpellSpecific();
    spell->_diminishInfoNonTriggered = original._diminishInfoNonTriggered;
    spell->_diminishInfoTriggered = original._diminishInfoTriggered;
    spell->_InitializeExplicitTargetMask();
    spell->_LoadImmunityInfo();
    return spell;
}
}

#endif
