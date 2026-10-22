// Synthesized PMGR tables for t8101 (A14), beyond the t8030 ones it reuses.
// The kernel only sanity-checks their shape; these are made-up values, not
// hardware data.

// BRIDGE SETTINGS
// REG_OFF, REG_VALUE
// d53g lists 18 PMGR bridges and marks none of 0-17 optional, so ApplePMGR
// wants settings for each one. t8030's synthesized tables cover all but
// 9-11; these fill the gap with the same kind of made-up values.
static const uint32_t t8101_bridge_settings9[][2] = {
    {0x0, 0xF},
    {0x10, 0x8F04},
};

static const uint32_t t8101_bridge_settings10[][2] = {
    {0x0, 0xF},
    {0x10, 0x8F04},
};

static const uint32_t t8101_bridge_settings11[][2] = {
    {0x0, 0xF},
    {0x10, 0x8F04},
};
