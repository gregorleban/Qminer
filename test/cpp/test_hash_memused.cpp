/* Copyright (C) Event Registry d.o.o. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 */

// Tests for the group-3 perf changes (2026-08-31 deep analysis):
//   S2 - is_shallow<TKeyDat> trait: deep GetMemUsed over posting-list vectors is
//        O(1) by construction and numerically identical to the element walk
//   S4 - fused single-pass DJB string hash: values bit-identical to the historical
//        two-pass (strlen + hash) implementation, secondary == primary, and the
//        THash/TStrHash probe paths (which now reuse the primary hash) behave
//        identically, including across serialization (stored HashCds must match)

#include <base.h>
#include "gtest/gtest.h"

namespace {

// the historical DJB implementation: explicit strlen prepass + length-bounded loop.
// The fused implementation must produce bit-identical values to this
int ReferenceDJBPrimHashCd(const char* p)
{
    const char* r = p; while (*r) { r++; }
    const size_t Len = (size_t)(r - p);
    unsigned int hash = 5381;
    for (size_t i = 0; i < Len; p++, i++) {
        hash = ((hash << 5) + hash) + (*p);
    }
    return (int)hash & 0x7fffffff;
}

TStr MakeRandomKey(TRnd& Rnd, const int& MxLen)
{
    const int Len = 1 + Rnd.GetUniDevInt(0, MxLen - 1);
    TChA ChA;
    for (int ChN = 0; ChN < Len; ChN++) {
        // any non-NUL byte, including high-bit (negative signed char) values -
        // those exercise the sign-extension behavior the hash relies on
        ChA += char(Rnd.GetUniDevInt(1, 255));
    }
    return TStr(ChA);
}

} // namespace

// S2: the trait must classify packed key-dat pairs of shallow types as shallow,
// and keep anything owning heap memory on the deep path
TEST(ShallowTraitTests, KeyDatTraitOptIns)
{
    EXPECT_TRUE((gtraits::is_shallow<TKeyDat<TUInt64, TInt> >::value));  // TQmGixItemFull
    EXPECT_TRUE((gtraits::is_shallow<TKeyDat<TUInt, TSInt> >::value));   // TQmGixItemSmall
    EXPECT_TRUE((gtraits::is_shallow<TKeyDat<TInt, TFlt> >::value));
    EXPECT_FALSE((gtraits::is_shallow<TKeyDat<TStr, TInt> >::value));    // TStr owns memory
    // sanity: the pre-existing specializations still hold
    EXPECT_TRUE((gtraits::is_shallow<TPair<TInt, TInt> >::value));
    EXPECT_TRUE((gtraits::is_shallow<TInt>::value));
    EXPECT_FALSE((gtraits::is_shallow<TStr>::value));
}

// S2: for the gix posting item types, the deep element walk and the shallow O(1)
// formula are numerically identical - the trait must not change any reported size,
// on flat vectors and on the nested ChildV shape the gix cache accounting walks
TEST(ShallowTraitTests, DeepMemUsedEqualsShallowForKeyDatVectors)
{
    typedef TKeyDat<TUInt64, TInt> TItem;
    TVec<TItem> Vec(1000, 0);
    for (int ItemN = 0; ItemN < 1000; ItemN++) { Vec.Add(TItem(uint64(ItemN), ItemN % 7)); }
    // with the trait, GetMemUsed(true) dispatches to the shallow O(1) path - the
    // reported size must be identical to the (historical) deep element walk
    EXPECT_EQ(Vec.GetMemUsed(false), Vec.GetMemUsed(true));
    EXPECT_EQ(uint64(sizeof(TVec<TItem>)) + uint64(Vec.Reserved()) * sizeof(TItem), Vec.GetMemUsed(true));

    // nested vector-of-vectors (the TGixItemSet::ChildV shape)
    TVec<TVec<TItem> > ChildV(5, 0);
    uint64 ExpectInner = 0;
    for (int ChildN = 0; ChildN < 5; ChildN++) {
        TVec<TItem> Child(100 * (ChildN + 1), 0);
        for (int ItemN = 0; ItemN < Child.Reserved(); ItemN++) { Child.Add(TItem(uint64(ItemN), 1)); }
        ExpectInner += Child.GetMemUsed(false);
        ChildV.Add(TVec<TItem>());
        ChildV.Last().MoveFrom(Child);
    }
    // deep walk of the outer vector = outer shell + shallow sizes of the children
    const uint64 OuterShell = sizeof(TVec<TVec<TItem> >) +
        TMemUtils::GetExtraMemberSize(TInt(0)) + TMemUtils::GetExtraMemberSize(TInt(0));
    EXPECT_EQ(OuterShell + ExpectInner, TMemUtils::GetMemUsed(ChildV));
}

// S4: the fused single-pass DJB must produce bit-identical values to the
// historical two-pass implementation, for both overloads, and sec == prim
TEST(HashFuncTests, FusedDJBMatchesReferenceTwoPass)
{
    const char* FixedV[] = { "", "a", "ab", "quick brown fox", "hello",
        "\x01\x7f", "\xe8\xa1\xac", "with spaces and, punctuation!" };
    for (int StrN = 0; StrN < 8; StrN++) {
        EXPECT_EQ(ReferenceDJBPrimHashCd(FixedV[StrN]), TStrHashF_DJB::GetPrimHashCd(FixedV[StrN]))
            << "mismatch for '" << FixedV[StrN] << "'";
        EXPECT_EQ(TStrHashF_DJB::GetPrimHashCd(FixedV[StrN]), TStrHashF_DJB::GetSecHashCd(FixedV[StrN]));
    }
    TRnd Rnd(11);
    for (int RunN = 0; RunN < 2000; RunN++) {
        const TStr Key = MakeRandomKey(Rnd, 200);
        EXPECT_EQ(ReferenceDJBPrimHashCd(Key.CStr()), TStrHashF_DJB::GetPrimHashCd(Key.CStr()));
        EXPECT_EQ(TStrHashF_DJB::GetPrimHashCd(Key), TStrHashF_DJB::GetSecHashCd(Key));
        // TStr's own hash methods (the TDefaultHashFunc<TStr> path) forward to DJB
        EXPECT_EQ(TStrHashF_DJB::GetPrimHashCd(Key.CStr()), Key.GetPrimHashCd());
        EXPECT_EQ(Key.GetPrimHashCd(), Key.GetSecHashCd());
    }
}

// S4: THash<TStr,...> probes (which now reuse the primary hash as HashCd) must
// behave identically through add/lookup/delete and across save/load - the stored
// HashCd values are compared on every probe, so a load of a table saved with the
// old code must keep finding every key
TEST(HashFuncTests, THashStrProbesAndSerializationRoundTrip)
{
    const int Keys = 20000;
    TRnd Rnd(13);
    TStrV KeyV(Keys, 0);
    THash<TStr, TInt> H;
    for (int KeyN = 0; KeyN < Keys; KeyN++) {
        TStr Key;
        do { Key = MakeRandomKey(Rnd, 40); } while (H.IsKey(Key));
        KeyV.Add(Key);
        H.AddDat(Key, KeyN);
    }
    ASSERT_EQ(Keys, H.Len());
    for (int KeyN = 0; KeyN < Keys; KeyN++) {
        ASSERT_TRUE(H.IsKey(KeyV[KeyN]));
        ASSERT_EQ(KeyN, (int)H.GetDat(KeyV[KeyN]));
    }
    // delete every third key and re-verify
    for (int KeyN = 0; KeyN < Keys; KeyN += 3) { H.DelKey(KeyV[KeyN]); }
    for (int KeyN = 0; KeyN < Keys; KeyN++) {
        if (KeyN % 3 == 0) { ASSERT_FALSE(H.IsKey(KeyV[KeyN])); }
        else { ASSERT_EQ(KeyN, (int)H.GetDat(KeyV[KeyN])); }
    }
    // serialization round trip: the stored HashCds must keep matching the probes
    TMOut MOut; H.Save(MOut);
    TMIn MIn(MOut.GetBfAddr(), MOut.Len(), false);
    THash<TStr, TInt> Loaded(MIn);
    ASSERT_EQ(H.Len(), Loaded.Len());
    for (int KeyN = 0; KeyN < Keys; KeyN++) {
        if (KeyN % 3 == 0) { ASSERT_FALSE(Loaded.IsKey(KeyV[KeyN])); }
        else { ASSERT_EQ(KeyN, (int)Loaded.GetDat(KeyV[KeyN])); }
    }
    // misses stay misses
    for (int RunN = 0; RunN < 1000; RunN++) {
        TStr Miss = MakeRandomKey(Rnd, 40);
        if (!H.IsKey(Miss)) { EXPECT_FALSE(Loaded.IsKey(Miss)); }
    }
}

// S4: same contract for the vocabulary-style pooled TStrHash
TEST(HashFuncTests, TStrHashProbesAndSerializationRoundTrip)
{
    const int Keys = 20000;
    TRnd Rnd(17);
    TStrV KeyV(Keys, 0);
    TStrHash<TInt> SH;
    for (int KeyN = 0; KeyN < Keys; KeyN++) {
        TStr Key;
        do { Key = MakeRandomKey(Rnd, 40); } while (SH.IsKey(Key.CStr()));
        KeyV.Add(Key);
        SH.AddDat(Key.CStr(), KeyN);
    }
    ASSERT_EQ(Keys, SH.Len());
    for (int KeyN = 0; KeyN < Keys; KeyN++) {
        ASSERT_TRUE(SH.IsKey(KeyV[KeyN].CStr()));
        ASSERT_EQ(KeyN, (int)SH.GetDat(KeyV[KeyN].CStr()));
    }
    TMOut MOut; SH.Save(MOut);
    TMIn MIn(MOut.GetBfAddr(), MOut.Len(), false);
    TStrHash<TInt> Loaded(MIn);
    ASSERT_EQ(SH.Len(), Loaded.Len());
    for (int KeyN = 0; KeyN < Keys; KeyN++) {
        ASSERT_TRUE(Loaded.IsKey(KeyV[KeyN].CStr()));
        ASSERT_EQ(KeyN, (int)Loaded.GetDat(KeyV[KeyN].CStr()));
    }
}
