#pragma once
#include "PackedCell.hpp"

namespace AtomicCScompact {

// States (8-bit)
static constexpr tag8_t ST_IDLE        = 0x00;
static constexpr tag8_t ST_PUBLISHED   = 0x01;
static constexpr tag8_t ST_PENDING     = 0x02;
static constexpr tag8_t ST_CLAIMED     = 0x03;
static constexpr tag8_t ST_PROCESSING  = 0x04;
static constexpr tag8_t ST_COMPLETE    = 0x05;
static constexpr tag8_t ST_RETIRED     = 0x06;
static constexpr tag8_t ST_EPOCH_BUMP  = 0x07;
static constexpr tag8_t ST_LOCKED      = 0x08;
// Reserve 0xF0..0xFF for user extensions

// Relation bit masks (8-bit)
static constexpr tag8_t REL_NONE      = 0x00;
static constexpr tag8_t REL_NODE0     = 0x01;
static constexpr tag8_t REL_NODE1     = 0x02;
static constexpr tag8_t REL_PAGE      = 0x04;
static constexpr tag8_t REL_PATTERN   = 0x08;
static constexpr tag8_t REL_SELF      = 0x10;
static constexpr tag8_t REL_BROADCAST = 0xFF; // convenience

static inline strl16_t MakeSTREL(tag8_t st, tag8_t rel) noexcept
{
    return static_cast<strl16_t>((static_cast<strl16_t>(st) << 8) | static_cast<strl16_t>(rel));
}
static inline bool RelationMatches(tag8_t slot_rel, tag8_t rel_mask) noexcept
{
    return ((static_cast<strl16_t>(slot_rel) & static_cast<uint8_t>(rel_mask)) != 0);
}


}