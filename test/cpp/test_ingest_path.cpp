/* Copyright (C) Event Registry d.o.o. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 */

// Tests for the group-5 (ingest cluster) changes (2026-08-31 deep analysis):
//   - TVec::Merge rewritten in place (sorted fast path, two-pointer unique)
//   - gix AddItem/AddItemV/DelItem/DelItemV/DelItemsBelow resolve the key with a
//     single dictionary lookup, and the flush blocks dropped their redundant
//     O(children) RecalcTotalCnt; ProcessDeletes hands the rebuilt buffer over
//     with a move
//   - JSON: the lexer no longer maintains the uppercase shadow string in
//     case-sensitive mode (values must come out byte-exact), and the defaulted
//     GetObj* reads use a single hash probe
//   - AddRec: the primary map is fed from the already-parsed JSON value, the
//     system inserted_at field is only added for stores that have it, and
//     compressed string fields avoid the per-value copies/allocs
//
// Everything here checks EXTERNAL behavior, so the tests double as regression
// proof that the optimizations changed nothing observable.

#include <qminer.h>
#include <qminer_storage.h>
#include "gtest/gtest.h"

using namespace TQm;

namespace {

void FreshDir(const TStr& FPath)
{
    if (TDir::Exists(FPath)) { TDir::DelNonEmptyDir(FPath); }
    TDir::GenDir(FPath);
}

void EnsureQmEnv()
{
    if (!TQm::TEnv::IsInit()) { TQm::TEnv::Init(); }
    if (!TUnicodeDef::IsDef()) { TUnicodeDef::Load("./src/glib/bin/UnicodeDef.Bin"); }
}

// reference merge: the exact behavior of the historical implementation
template <class TVal>
void CheckMerge(const TVec<TVal>& Src)
{
    TVec<TVal> Expect(Src); Expect.Sort();
    TVec<TVal> ExpectUnique;
    for (int ValN = 0; ValN < Expect.Len(); ValN++) {
        if (ValN == 0 || Expect[ValN - 1] != Expect[ValN]) { ExpectUnique.Add(Expect[ValN]); }
    }
    TVec<TVal> Got(Src);
    Got.Merge();
    ASSERT_EQ(ExpectUnique.Len(), Got.Len());
    for (int ValN = 0; ValN < Got.Len(); ValN++) {
        ASSERT_TRUE(ExpectUnique[ValN] == Got[ValN]) << "mismatch at " << ValN;
    }
    EXPECT_GE(Got.Reserved(), Got.Len());
}

} // namespace

// the in-place Merge must be behavior-identical to the historical
// copy-sort-rebuild implementation on every input shape
TEST(IngestPathTests, MergeInPlace)
{
    // already sorted and unique (the deletes-only flush case - now one linear scan)
    {
        TVec<TUInt> V(1000, 0);
        for (int ValN = 0; ValN < 1000; ValN++) { V.Add(TUInt(uint(ValN))); }
        CheckMerge(V);
    }
    // sorted with duplicates
    {
        TVec<TUInt> V(1000, 0);
        for (int ValN = 0; ValN < 1000; ValN++) { V.Add(TUInt(uint(ValN / 3))); }
        CheckMerge(V);
    }
    // unsorted with duplicates
    {
        TRnd Rnd(1);
        TVec<TInt> V(2000, 0);
        for (int ValN = 0; ValN < 2000; ValN++) { V.Add(Rnd.GetUniDevInt(0, 500)); }
        CheckMerge(V);
    }
    // edge shapes
    {
        TVec<TInt> Empty; Empty.Merge(); EXPECT_EQ(0, Empty.Len());
        TVec<TInt> One; One.Add(7); One.Merge();
        ASSERT_EQ(1, One.Len()); EXPECT_EQ(7, (int)One[0]);
        TVec<TInt> AllEqual(100, 0);
        for (int ValN = 0; ValN < 100; ValN++) { AllEqual.Add(42); }
        AllEqual.Merge();
        ASSERT_EQ(1, AllEqual.Len()); EXPECT_EQ(42, (int)AllEqual[0]);
    }
    // TKeyDat: == and < compare the Key only, so equal-key items merge to one
    {
        TVec<TKeyDat<TUInt64, TInt> > V;
        V.Add(TKeyDat<TUInt64, TInt>(5, 1));
        V.Add(TKeyDat<TUInt64, TInt>(3, 2));
        V.Add(TKeyDat<TUInt64, TInt>(5, 3));
        V.Add(TKeyDat<TUInt64, TInt>(1, 4));
        V.Merge();
        ASSERT_EQ(3, V.Len());
        EXPECT_EQ(uint64(1), (uint64)V[0].Key);
        EXPECT_EQ(uint64(3), (uint64)V[1].Key);
        EXPECT_EQ(uint64(5), (uint64)V[2].Key);
    }
    // non-flat element type (owning strings) keeps working
    {
        TVec<TStr> V;
        V.Add("pear"); V.Add("apple"); V.Add("pear"); V.Add("fig"); V.Add("apple");
        CheckMerge(V);
    }
}

// full add/delete round trip through the single-lookup gix write paths: new-key
// creation, existing-key adds, work-buffer flushes, batch deletes, delete of
// missing keys, delete-to-empty, DelItemsBelow, and persistence across reopen
TEST(IngestPathTests, GixSingleLookupRoundTrip)
{
    const TStr FPath = "./ingest_gix/";
    FreshDir(FPath);
    typedef TIntUInt64Pr TGKey;
    typedef TUInt TGItem;
    TGixDefItemHandler<TGKey, TGItem> ItemHandler;
    const TGKey KeyA(1, 1); const TGKey KeyB(2, 1); const TGKey KeyGone(3, 1);
    {
        TPt<TGix<TGKey, TGItem> > Gix = TGix<TGKey, TGItem>::New("IngestGix", FPath,
            faCreate, &ItemHandler, 10000000, 100, true, 50, 200);
        // new-key path, then existing-key adds far past the split length (forces
        // the flush blocks that no longer call RecalcTotalCnt)
        for (int ItemN = 0; ItemN < 750; ItemN++) { Gix->AddItem(KeyA, TGItem(uint(ItemN))); }
        // AddItemV on a new key and on an existing key
        TVec<TGItem> BulkV;
        for (int ItemN = 0; ItemN < 300; ItemN++) { BulkV.Add(TGItem(uint(ItemN * 2))); }
        Gix->AddItemV(KeyB, BulkV);
        TVec<TGItem> MoreV;
        for (int ItemN = 750; ItemN < 900; ItemN++) { MoreV.Add(TGItem(uint(ItemN))); }
        Gix->AddItemV(KeyA, MoreV);
        // deletes: batch (exercises the moved ProcessDeletes rebuild), single,
        // and deletes on a missing key (must be a clean no-op)
        TVec<TGItem> DelV;
        for (int ItemN = 100; ItemN < 200; ItemN++) { DelV.Add(TGItem(uint(ItemN))); }
        Gix->DelItemV(KeyA, DelV);
        Gix->DelItem(KeyA, TGItem(0u));
        Gix->DelItem(KeyGone, TGItem(1u));
        TVec<TGItem> GoneV; GoneV.Add(TGItem(2u));
        Gix->DelItemV(KeyGone, GoneV);
        EXPECT_EQ(uint64(0), Gix->DelItemsBelow(KeyGone, TGItem(10u)));
        // verify content: KeyA = {1..99, 200..899}, KeyB = evens 0..598
        TVec<TGItem> ItemV; Gix->GetItemV(KeyA, ItemV);
        ASSERT_EQ(99 + 700, ItemV.Len());
        EXPECT_EQ(uint(1), (uint)ItemV[0].Val);
        EXPECT_EQ(uint(99), (uint)ItemV[98].Val);
        EXPECT_EQ(uint(200), (uint)ItemV[99].Val);
        EXPECT_EQ(uint(899), (uint)ItemV.Last().Val);
        TVec<TGItem> ItemVB; Gix->GetItemV(KeyB, ItemVB);
        ASSERT_EQ(300, ItemVB.Len());
        EXPECT_EQ(uint(598), (uint)ItemVB.Last().Val);
        // DelItemsBelow drops the prefix
        EXPECT_EQ(uint64(99), Gix->DelItemsBelow(KeyA, TGItem(200u)));
        TVec<TGItem> AfterBelowV; Gix->GetItemV(KeyA, AfterBelowV);
        ASSERT_EQ(700, AfterBelowV.Len());
        EXPECT_EQ(uint(200), (uint)AfterBelowV[0].Val);
        // delete every item of KeyB: the postings are gone immediately; the key
        // itself is removed when the emptied itemset is stored (at close below)
        Gix->DelItemV(KeyB, ItemVB);
        TVec<TGItem> EmptyV; Gix->GetItemV(KeyB, EmptyV);
        EXPECT_EQ(0, EmptyV.Len());
        EXPECT_TRUE(Gix->IsKey(KeyA));
    }
    // persistence across reopen
    {
        TPt<TGix<TGKey, TGItem> > Gix = TGix<TGKey, TGItem>::New("IngestGix", FPath,
            faRdOnly, &ItemHandler, 10000000);
        EXPECT_TRUE(Gix->IsKey(KeyA));
        EXPECT_FALSE(Gix->IsKey(KeyB));
        TVec<TGItem> ItemV; Gix->GetItemV(KeyA, ItemV);
        ASSERT_EQ(700, ItemV.Len());
        EXPECT_EQ(uint(200), (uint)ItemV[0].Val);
        EXPECT_EQ(uint(899), (uint)ItemV.Last().Val);
    }
    TDir::DelNonEmptyDir(FPath);
}

// JSON parsing must stay byte-exact in case-sensitive mode (the lexer no longer
// maintains its uppercase shadow there), and the single-probe GetObj* reads must
// behave identically for hits and defaults
TEST(IngestPathTests, JsonParseAndObjReads)
{
    const TStr DocStr =
        "{ \"MixedCaseKey\": \"MixedCaseValue with UPPER and lower\","
        "  \"escapes\": \"line1\\nline2\\ttabbed \\\"quoted\\\" back\\\\slash\","
        "  \"unicode\": \"\\u0041\\u017d\","
        "  \"num\": 42.5, \"int\": -17, \"big\": 1234567890123, \"flag\": true,"
        "  \"arr\": [1, 2, 3] }";
    PJsonVal Val = TJsonVal::GetValFromStr(DocStr);
    ASSERT_TRUE(Val->IsObj());
    // values byte-exact, case preserved
    EXPECT_EQ(TStr("MixedCaseValue with UPPER and lower"), Val->GetObjStr("MixedCaseKey"));
    EXPECT_EQ(TStr("line1\nline2\ttabbed \"quoted\" back\\slash"), Val->GetObjStr("escapes"));
    EXPECT_EQ(TStr("A\xC5\xBD"), Val->GetObjStr("unicode")); // U+0041 'A' + U+017D 'Z-caron' in UTF-8
    // keys are case-sensitive: a different casing is a miss
    EXPECT_FALSE(Val->IsObjKey("mixedcasekey"));
    // single-probe defaulted reads: hits and misses of every type
    EXPECT_EQ(TStr("MixedCaseValue with UPPER and lower"), Val->GetObjStr("MixedCaseKey", "def"));
    EXPECT_EQ(TStr("def"), Val->GetObjStr("missing", "def"));
    EXPECT_DOUBLE_EQ(42.5, Val->GetObjNum("num", -1.0));
    EXPECT_DOUBLE_EQ(-1.0, Val->GetObjNum("missing", -1.0));
    EXPECT_EQ(-17, Val->GetObjInt("int", 0));
    EXPECT_EQ(99, Val->GetObjInt("missing", 99));
    EXPECT_EQ(int64(1234567890123LL), Val->GetObjInt64("big", 0));
    EXPECT_EQ(int64(-5), Val->GetObjInt64("missing", -5));
    EXPECT_EQ(uint64(1234567890123ULL), Val->GetObjUInt64("big", 0));
    EXPECT_EQ(uint64(7), Val->GetObjUInt64("missing", 7));
    EXPECT_TRUE(Val->GetObjBool("flag", false));
    EXPECT_FALSE(Val->GetObjBool("missing", false));
    // array access unchanged
    TIntV IntV; Val->GetObjIntV("arr", IntV);
    ASSERT_EQ(3, IntV.Len());
    EXPECT_EQ(2, (int)IntV[1]);
}

// AddRec on a paged store: the JSON-fed primary map must behave exactly like the
// read-back one (duplicate detection, lookups, persistence), and windowless
// stores must not have the system inserted_at field injected into the caller's
// JSON any more
TEST(IngestPathTests, StoreIngestPrimaryAndInsertedAt)
{
    const TStr FPath = "./ingest_store/";
    FreshDir(FPath);
    EnsureQmEnv();
    const TStr SchemaStr =
        "[{ \"name\": \"PrimStore\","
        "   \"options\": { \"type\": \"paged\" },"
        "   \"fields\": [ { \"name\": \"Name\", \"type\": \"string\", \"primary\": true },"
        "                 { \"name\": \"Num\", \"type\": \"int\" } ]"
        "},"
        " { \"name\": \"IntPrimStore\","
        "   \"options\": { \"type\": \"paged\" },"
        "   \"fields\": [ { \"name\": \"Code\", \"type\": \"int\", \"primary\": true },"
        "                 { \"name\": \"Tag\", \"type\": \"string\" } ]"
        "}]";
    uint64 Rec0 = 0, Rec1 = 0;
    {
        PBase Base = TStorage::NewBase(FPath, TJsonVal::GetValFromStr(SchemaStr), 10000000, 10000000, true);
        TWPt<TStore> Store = Base->GetStoreByStoreNm("PrimStore");
        PJsonVal RecVal = TJsonVal::GetValFromStr("{ \"Name\": \"alpha\", \"Num\": 1 }");
        Rec0 = Store->AddRec(RecVal);
        // windowless store: the caller's JSON must NOT have been polluted with
        // the system inserted_at field
        EXPECT_FALSE(RecVal->IsObjKey(TStoreWndDesc::SysInsertedAtFieldName));
        Rec1 = Store->AddRec(TJsonVal::GetValFromStr("{ \"Name\": \"beta\", \"Num\": 2 }"));
        ASSERT_NE(Rec0, Rec1);
        // primary duplicate detection resolves through the JSON-fed map
        const uint64 RecDup = Store->AddRec(TJsonVal::GetValFromStr("{ \"Name\": \"alpha\" }"));
        EXPECT_EQ(Rec0, RecDup);
        // primary lookups
        EXPECT_TRUE(Store->IsRecNm("alpha"));
        EXPECT_TRUE(Store->IsRecNm("beta"));
        EXPECT_FALSE(Store->IsRecNm("gamma"));
        EXPECT_EQ(Rec0, Store->GetRecId(TStr("alpha")));
        // int primary goes through the JSON-fed path too
        TWPt<TStore> IntStore = Base->GetStoreByStoreNm("IntPrimStore");
        const uint64 IntRec = IntStore->AddRec(TJsonVal::GetValFromStr("{ \"Code\": 55, \"Tag\": \"x\" }"));
        const uint64 IntDup = IntStore->AddRec(TJsonVal::GetValFromStr("{ \"Code\": 55 }"));
        EXPECT_EQ(IntRec, IntDup);
        EXPECT_EQ(2, Store->GetRecs());
        EXPECT_EQ(1, (int)IntStore->GetRecs());
        TStorage::SaveBase(Base);
    }
    // the primary map built from JSON values persists correctly
    {
        PBase Base = TStorage::LoadBase(FPath, faRdOnly, 10000000, 10000000);
        TWPt<TStore> Store = Base->GetStoreByStoreNm("PrimStore");
        EXPECT_TRUE(Store->IsRecNm("alpha"));
        EXPECT_EQ(Rec0, Store->GetRecId(TStr("alpha")));
        EXPECT_EQ(Rec1, Store->GetRecId(TStr("beta")));
    }
    TDir::DelNonEmptyDir(FPath);
}

// compressed string fields: byte-exact round trips through the zero-copy
// decompress path (in-page compressed bytes -> TStr-owned buffer), across
// value sizes and a close/reopen
TEST(IngestPathTests, CompressedStrRoundTrip)
{
    const TStr FPath = "./ingest_compress/";
    FreshDir(FPath);
    EnsureQmEnv();
    const TStr SchemaStr =
        "[{ \"name\": \"CompStore\","
        "   \"options\": { \"type\": \"paged\" },"
        "   \"fields\": [ { \"name\": \"Name\", \"type\": \"string\", \"primary\": true },"
        "                 { \"name\": \"Body\", \"type\": \"string\", \"compressed\": true } ]"
        "}]";
    // bodies: empty, short, and a long compressible one
    TChA LongChA;
    TRnd Rnd(9);
    for (int WordN = 0; WordN < 5000; WordN++) {
        LongChA += "word"; LongChA += TInt::GetStr(Rnd.GetUniDevInt(0, 50)).CStr(); LongChA += ' ';
    }
    const TStr LongBody(LongChA);
    TVec<TStr> BodyV; BodyV.Add(""); BodyV.Add("short body"); BodyV.Add(LongBody);
    {
        PBase Base = TStorage::NewBase(FPath, TJsonVal::GetValFromStr(SchemaStr), 10000000, 10000000, true);
        TWPt<TStore> Store = Base->GetStoreByStoreNm("CompStore");
        const int BodyFieldId = Store->GetFieldId("Body");
        for (int RecN = 0; RecN < BodyV.Len(); RecN++) {
            PJsonVal RecVal = TJsonVal::NewObj();
            RecVal->AddToObj("Name", TStr::Fmt("rec%d", RecN));
            RecVal->AddToObj("Body", BodyV[RecN]);
            const uint64 RecId = Store->AddRec(RecVal);
            EXPECT_EQ(BodyV[RecN], Store->GetFieldStr(RecId, BodyFieldId));
        }
        TStorage::SaveBase(Base);
    }
    {
        PBase Base = TStorage::LoadBase(FPath, faRdOnly, 10000000, 10000000);
        TWPt<TStore> Store = Base->GetStoreByStoreNm("CompStore");
        const int BodyFieldId = Store->GetFieldId("Body");
        for (int RecN = 0; RecN < BodyV.Len(); RecN++) {
            const uint64 RecId = Store->GetRecId(TStr::Fmt("rec%d", RecN));
            EXPECT_EQ(BodyV[RecN], Store->GetFieldStr(RecId, BodyFieldId));
        }
    }
    TDir::DelNonEmptyDir(FPath);
}
