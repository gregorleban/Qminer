/* Copyright (C) Event Registry d.o.o. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 */

#include <base.h>

#include "gtest/gtest.h"

// Tests for the TUniChDb direct BMP codepoint -> keyId lookup (ChInfoKeyIdV,
// built in InitAfterLoad): every codepoint-driven accessor must return exactly
// what the hash probe used to return - for the whole BMP, for astral-plane
// codepoints (which fall back to the hash) and on a db without the table.

namespace {

void BmpInitEnv() {
    if (!TUnicodeDef::IsDef()) { TUnicodeDef::Load("./src/glib/bin/UnicodeDef.Bin"); }
}

} // namespace

// the whole BMP: the table must agree with the hash probe codepoint by codepoint
TEST(UnicodeBmpTests, TableMatchesHashForWholeBmp)
{
    BmpInitEnv();
    ASSERT_TRUE(TUnicodeDef::IsDef());
    const TUniChDb& Db = TUnicodeDef::GetDef()->ucd;
    ASSERT_EQ(Db.ChInfoKeyIdV.Len(), 0x10000);
    int Defined = 0;
    for (int Cp = 0; Cp < 0x10000; Cp++) {
        ASSERT_EQ(Db.h.GetKeyId(Cp), Db.GetChInfoKeyId(Cp)) << "codepoint " << Cp;
        if (Db.GetChInfoKeyId(Cp) >= 0) { Defined++; }
    }
    // sanity: the BMP is far from empty (ASCII alone defines 128 codepoints)
    EXPECT_GT(Defined, 10000);
}

// the derived accessors go through the table now - spot-check them against the
// hash-derived values across scripts, and off the table for astral codepoints
TEST(UnicodeBmpTests, AccessorsMatchHashPath)
{
    BmpInitEnv();
    const TUniChDb& Db = TUnicodeDef::GetDef()->ucd;
    // ascii letter/digit/space, latin-1, cyrillic, arabic, cjk, hangul,
    // unassigned BMP slot, astral plane (emoji, cjk ext-b) - the astral ones
    // exercise the hash fallback branch of GetChInfoKeyId
    const int CpV[] = { 'a', 'Z', '5', ' ', 0xE4, 0x430, 0x627, 0x4E2D, 0xAC00,
        0x0378, 0xFFFF, 0x10000, 0x1F600, 0x20000 };
    for (int CpN = 0; CpN < (int) (sizeof(CpV) / sizeof(CpV[0])); CpN++) {
        const int Cp = CpV[CpN];
        const int HashKeyId = Db.h.GetKeyId(Cp);
        ASSERT_EQ(HashKeyId, Db.GetChInfoKeyId(Cp)) << "codepoint " << Cp;
        // category through the public accessor vs directly through the hash
        const TUniChCategory HashCat = (HashKeyId < 0) ? ucOther : Db.h[HashKeyId].cat;
        ASSERT_EQ(HashCat, Db.GetCat(Cp)) << "codepoint " << Cp;
    }
    // a few semantic anchors so a systematically shifted table cannot pass
    EXPECT_EQ(Db.GetCat('a'), ucLetter);
    EXPECT_EQ(Db.GetCat('5'), ucNumber);
    EXPECT_EQ(Db.GetCat(0x430), ucLetter);     // cyrillic small a
    EXPECT_EQ(Db.GetCat(' '), ucSeparator);
}

// a db whose table was never built (fresh instance before Load) must fall back
// to the hash transparently instead of reading a stale or empty table
TEST(UnicodeBmpTests, EmptyTableFallsBackToHash)
{
    TUniChDb FreshDb;
    ASSERT_EQ(FreshDb.ChInfoKeyIdV.Len(), 0);
    // empty db: both paths agree that nothing is defined
    EXPECT_EQ(FreshDb.GetChInfoKeyId('a'), FreshDb.h.GetKeyId('a'));
    EXPECT_EQ(FreshDb.GetChInfoKeyId('a'), -1);
    EXPECT_EQ(FreshDb.GetCat('a'), ucOther);
}
