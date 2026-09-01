/* Copyright (C) Event Registry d.o.o. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 */

// Regression tests for bug batch 2 + the batch-4 optimizations (2026-09-01,
// remaining-work doc):
//   B7  JSON surrogate pairs decode to real UTF-8, unpaired halves -> U+FFFD
//   B11 ASCII case tables (no CRT tolower/toupper on signed chars)
//   B13 TStr::Fmt heap retry past 10KB; GetStr(Str,Fmt) no stack smash
//   B14 exact JSON integers above 2^53 (parse, getters, printing)
//   B15 TVec::FindAll/FindAllSatisfy return indexes
//   B18 TPair move constructor actually moves
//   B24 TQQueue::Back on a wrapped circular buffer
//   B25 THash sized ctor grows past its estimate
//   B26 self-referencing TVec::Add/Ins
//   B27 numeric Tm primary keys accepted by the paged store
//   B10 gix DelItemsBelow works with pending delete markers
//   A2  paged-store field-read memo stays correct across writes
//   A3  range-limited index-join filtering
//   A5  blob free-block reuse across segments (hint bookkeeping)
//   A6  JSON escaping byte-compatibility (incl. emoji round trip)
//   B-13 Base64 round trips; B-14 Ins/AddSorted fast paths

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

} // namespace

// B7 + A6: surrogate pairs parse into real UTF-8 and serialize back to the same
// surrogate escapes; unpaired halves become U+FFFD
TEST(BugfixBatch2, JsonSurrogatePairs)
{
    // U+1F600 (emoji) = \ud83d\ude00 = UTF-8 F0 9F 98 80
    {
        PJsonVal Val = TJsonVal::GetValFromStr("{ \"s\": \"\\ud83d\\ude00\" }");
        ASSERT_TRUE(Val->IsDef());
        const TStr S = Val->GetObjStr("s");
        ASSERT_EQ(4, S.Len());
        EXPECT_EQ(TStr("\xF0\x9F\x98\x80"), S);
        // serializing escapes it back as the same surrogate pair
        EXPECT_EQ(TStr("{\"s\":\"\\ud83d\\ude00\"}"), Val->SaveStr());
    }
    // unpaired high surrogate before a plain char -> U+FFFD + the char
    {
        PJsonVal Val = TJsonVal::GetValFromStr("{ \"s\": \"\\ud83dX\" }");
        ASSERT_TRUE(Val->IsDef());
        EXPECT_EQ(TStr("\xEF\xBF\xBDX"), Val->GetObjStr("s"));
    }
    // unpaired high surrogate before another (non-u) escape -> U+FFFD + escape
    {
        PJsonVal Val = TJsonVal::GetValFromStr("{ \"s\": \"\\ud83d\\nY\" }");
        ASSERT_TRUE(Val->IsDef());
        EXPECT_EQ(TStr("\xEF\xBF\xBD\nY"), Val->GetObjStr("s"));
    }
    // lone low surrogate -> U+FFFD
    {
        PJsonVal Val = TJsonVal::GetValFromStr("{ \"s\": \"\\ude00Z\" }");
        ASSERT_TRUE(Val->IsDef());
        EXPECT_EQ(TStr("\xEF\xBF\xBDZ"), Val->GetObjStr("s"));
    }
    // BMP escapes still work
    {
        PJsonVal Val = TJsonVal::GetValFromStr("{ \"s\": \"\\u0041\\u017d\" }");
        EXPECT_EQ(TStr("A\xC5\xBD"), Val->GetObjStr("s"));
    }
}

// A6: the escaping fast path is byte-compatible with the historical output
TEST(BugfixBatch2, JsonEscapingByteCompatible)
{
    PJsonVal Val = TJsonVal::NewObj();
    Val->AddToObj("a", TStr("plain ASCII 123"));
    Val->AddToObj("b", TStr("quote\" slash/ back\\ nl\n tab\t bell\x07"));
    Val->AddToObj("c", TStr("\xC5\xBD\xD0\x96"));          // 2-byte UTF-8 (Z-caron, Cyrillic Zhe)
    Val->AddToObj("d", TStr("\xF0\x9F\x98\x80"));          // 4-byte UTF-8 (emoji)
    const TStr Out = Val->SaveStr();
    EXPECT_TRUE(Out.SearchStr("plain ASCII 123") != -1);
    EXPECT_TRUE(Out.SearchStr("quote\\\" slash\\/ back\\\\ nl\\n tab\\t bell\\u0007") != -1);
    EXPECT_TRUE(Out.SearchStr("\\u017d\\u0416") != -1);
    EXPECT_TRUE(Out.SearchStr("\\ud83d\\ude00") != -1);
    // and the output parses back to the identical strings
    PJsonVal Back = TJsonVal::GetValFromStr(Out);
    EXPECT_EQ(Val->GetObjStr("b"), Back->GetObjStr("b"));
    EXPECT_EQ(Val->GetObjStr("c"), Back->GetObjStr("c"));
    EXPECT_EQ(Val->GetObjStr("d"), Back->GetObjStr("d"));
}

// B14: integers above 2^53 survive parse, typed getters and printing exactly
TEST(BugfixBatch2, JsonExactIntegers)
{
    PJsonVal Val = TJsonVal::GetValFromStr(
        "{ \"big\": 9007199254740993, \"neg\": -9007199254740993,"
        "  \"small\": 42, \"flt\": 42.5, \"exp\": 1e3 }");
    ASSERT_TRUE(Val->IsDef());
    // 2^53 + 1 is not representable in a double
    EXPECT_EQ(int64(9007199254740993LL), Val->GetObjKey("big")->GetInt64());
    EXPECT_EQ(uint64(9007199254740993ULL), Val->GetObjKey("big")->GetUInt64());
    EXPECT_EQ(int64(-9007199254740993LL), Val->GetObjKey("neg")->GetInt64());
    EXPECT_EQ(int64(42), Val->GetObjKey("small")->GetInt64());
    EXPECT_DOUBLE_EQ(42.5, Val->GetObjKey("flt")->GetNum());
    EXPECT_FALSE(Val->GetObjKey("flt")->IsExactInt());
    EXPECT_FALSE(Val->GetObjKey("exp")->IsExactInt()); // exponent form stays a double
    // printing round-trips the exact value
    EXPECT_TRUE(Val->SaveStr().SearchStr("9007199254740993") != -1);
    // GetNum still returns the (rounded) double
    EXPECT_DOUBLE_EQ(9007199254740992.0, Val->GetObjKey("big")->GetNum());
}

// B11: case mapping is fixed ASCII (high-bit bytes pass through untouched)
TEST(BugfixBatch2, AsciiCaseMapping)
{
    TStr Mixed("aBc XyZ 123 \xC5\xBD\xE8");
    EXPECT_EQ(TStr("abc xyz 123 \xC5\xBD\xE8"), Mixed.GetLc());
    EXPECT_EQ(TStr("ABC XYZ 123 \xC5\xBD\xE8"), Mixed.GetUc());
    TChA ChA("MiXeD\xC5\xBD");
    ChA.ToLc();
    EXPECT_EQ(TStr("mixed\xC5\xBD"), TStr(ChA));
    EXPECT_EQ(0, TStr("HeLLo\xC5").CmpI("hEllO\xC5"));
    EXPECT_NE(0, TStr("HeLLo").CmpI("hEllX"));
}

// B13: Fmt output longer than the 10KB stack buffer is complete, not truncated;
// GetStr with a long string no longer overflows a fixed buffer
TEST(BugfixBatch2, FmtHeapRetry)
{
    TChA LongChA;
    for (int ChN = 0; ChN < 20000; ChN++) { LongChA += char('a' + (ChN % 26)); }
    const TStr LongStr(LongChA);
    const TStr Fmted = TStr::Fmt("pre[%s]post", LongStr.CStr());
    ASSERT_EQ(20000 + 9, Fmted.Len());
    EXPECT_TRUE(Fmted.StartsWith("pre[abcde"));
    EXPECT_TRUE(Fmted.EndsWith("]post"));
    const TStr Got = TStr::GetStr(LongStr, "x%sy");
    ASSERT_EQ(20002, Got.Len());
    EXPECT_TRUE(Got.StartsWith("xabc"));
    EXPECT_TRUE(Got.EndsWith("y"));
}

// B15: FindAll/FindAllSatisfy return INDEXES
TEST(BugfixBatch2, FindAllReturnsIndexes)
{
    TIntV V;
    V.Add(7); V.Add(3); V.Add(7); V.Add(1); V.Add(7);
    TIntV IdxV;
    V.FindAll(7, IdxV);
    ASSERT_EQ(3, IdxV.Len());
    EXPECT_EQ(0, (int)IdxV[0]); EXPECT_EQ(2, (int)IdxV[1]); EXPECT_EQ(4, (int)IdxV[2]);
    TIntV SatIdxV;
    V.FindAllSatisfy([](const TInt& Val) { return Val < 5; }, SatIdxV);
    ASSERT_EQ(2, SatIdxV.Len());
    EXPECT_EQ(1, (int)SatIdxV[0]); EXPECT_EQ(3, (int)SatIdxV[1]);
}

// B18: TPair's move constructor moves (the const&& version silently copied)
TEST(BugfixBatch2, PairMoveMoves)
{
    TPair<TStr, TInt> Src(TStr("a-heap-allocated-string-value"), 7);
    TPair<TStr, TInt> Dst(std::move(Src));
    EXPECT_EQ(TStr("a-heap-allocated-string-value"), Dst.Val1);
    EXPECT_EQ(7, (int)Dst.Val2);
    // TStr's move leaves the source empty - proof the move ctor engaged
    EXPECT_TRUE(Src.Val1.Empty());
}

// B24: Back() on a wrapped circular queue
TEST(BugfixBatch2, QueueBackAfterWrap)
{
    TQQueue<TInt> Queue(2, -1);
    for (int ValN = 0; ValN < 100; ValN++) {
        Queue.Push(ValN);
        EXPECT_EQ(ValN, (int)Queue.Back()) << "at push " << ValN;
        if (Queue.Len() > 3) { Queue.Pop(); }
    }
    EXPECT_EQ(99, (int)Queue.Back());
}

// B25: a hash constructed with a size estimate keeps working when it outgrows it
TEST(BugfixBatch2, SizedHashGrowsPastEstimate)
{
    THash<TInt, TInt> H(16);
    for (int KeyN = 0; KeyN < 20000; KeyN++) { H.AddDat(KeyN, KeyN * 3); }
    ASSERT_EQ(20000, H.Len());
    for (int KeyN = 0; KeyN < 20000; KeyN++) {
        ASSERT_EQ(KeyN * 3, (int)H.GetDat(KeyN));
    }
}

// B26 + B-14: self-referencing Add/Ins are safe, and the Ins/AddSorted fast
// paths match the historical behavior
TEST(BugfixBatch2, SelfRefAddInsAndSortedInsert)
{
    // Add of own element at full capacity
    {
        TIntV V(4, 0);
        V.Add(10); V.Add(20); V.Add(30); V.Add(40); // now full
        V.Add(V[0]);
        ASSERT_EQ(5, V.Len());
        EXPECT_EQ(10, (int)V[4]);
    }
    // Ins of own last element at position 0 (the shift moves the source)
    {
        TIntV V;
        for (int ValN = 0; ValN < 10; ValN++) { V.Add(ValN); }
        V.Ins(0, V.Last());
        ASSERT_EQ(11, V.Len());
        EXPECT_EQ(9, (int)V[0]);
        EXPECT_EQ(0, (int)V[1]);
        EXPECT_EQ(9, (int)V[10]);
    }
    // Ins correctness at every position (memmove path), movable + owning types
    {
        TIntV V; V.Add(1); V.Add(3); V.Add(5);
        V.Ins(0, 0); V.Ins(2, 2); V.Ins(4, 4); V.Ins(6, 6);
        for (int ValN = 0; ValN < 7; ValN++) { ASSERT_EQ(ValN, (int)V[ValN]); }
        TStrV S; S.Add("b"); S.Add("d");
        S.Ins(0, "a"); S.Ins(2, "c"); S.Ins(4, "e");
        EXPECT_EQ(TStr("a"), S[0]); EXPECT_EQ(TStr("c"), S[2]); EXPECT_EQ(TStr("e"), S[4]);
    }
    // AddSorted (binary+memmove path) matches a reference sort, asc and desc
    {
        TRnd Rnd(3);
        TIntV Sorted; TIntV Ref;
        for (int ValN = 0; ValN < 500; ValN++) {
            const int Val = Rnd.GetUniDevInt(0, 50);
            Sorted.AddSorted(Val, true);
            Ref.Add(Val);
        }
        Ref.Sort();
        ASSERT_EQ(Ref.Len(), Sorted.Len());
        for (int ValN = 0; ValN < Ref.Len(); ValN++) { ASSERT_EQ((int)Ref[ValN], (int)Sorted[ValN]); }
        TIntV Desc;
        for (int ValN = 0; ValN < 200; ValN++) { Desc.AddSorted(Rnd.GetUniDevInt(0, 50), false); }
        EXPECT_TRUE(Desc.IsSorted(false));
        // trimmed top-k keeps the k best
        TIntV TopV;
        for (int ValN = 0; ValN < 100; ValN++) { TopV.AddSorted(Rnd.GetUniDevInt(0, 1000), true, 10); }
        EXPECT_EQ(10, TopV.Len());
        EXPECT_TRUE(TopV.IsSorted(true));
    }
}

// B-13: Base64 round trips across padding cases and larger buffers
TEST(BugfixBatch2, Base64RoundTrip)
{
    TRnd Rnd(4);
    for (int Len = 0; Len < 10; Len++) {
        TMem Src(Len > 0 ? Len : 1);
        for (int ChN = 0; ChN < Len; ChN++) { Src += char(Rnd.GetUniDevInt(0, 255) - 128); }
        const TStr Enc = TStr::Base64Encode(Src.GetBf(), Src.Len());
        TMem Dec; TStr::Base64Decode(Enc, Dec);
        ASSERT_EQ(Src.Len(), Dec.Len());
        if (Len > 0) { EXPECT_EQ(0, memcmp(Src.GetBf(), Dec.GetBf(), Len)); }
    }
    TMem Big(100000);
    for (int ChN = 0; ChN < 100000; ChN++) { Big += char(Rnd.GetUniDevInt(0, 255) - 128); }
    const TStr Enc = TStr::Base64Encode(Big.GetBf(), Big.Len());
    EXPECT_EQ(4 * ((100000 + 2) / 3), Enc.Len());
    TMem Dec; TStr::Base64Decode(Enc, Dec);
    ASSERT_EQ(Big.Len(), Dec.Len());
    EXPECT_EQ(0, memcmp(Big.GetBf(), Dec.GetBf(), Big.Len()));
    // TFile::Exists on files and non-files
    EXPECT_TRUE(TFile::Exists("./src/glib/base/dt.cpp"));
    EXPECT_FALSE(TFile::Exists("./no-such-file-here.xyz"));
    EXPECT_FALSE(TFile::Exists("./src"));
}

// B10: DelItemsBelow right after a DelItemV (pending markers) must act, not no-op
TEST(BugfixBatch2, GixDelItemsBelowWithPendingDeletes)
{
    const TStr FPath = "./bugfix2_gix/";
    FreshDir(FPath);
    typedef TIntUInt64Pr TGKey;
    typedef TUInt TGItem;
    TGixDefItemHandler<TGKey, TGItem> ItemHandler;
    const TGKey Key(1, 1);
    {
        TPt<TGix<TGKey, TGItem> > Gix = TGix<TGKey, TGItem>::New("B2Gix", FPath,
            faCreate, &ItemHandler, 10000000, 100, true, 50, 200);
        for (int ItemN = 0; ItemN < 500; ItemN++) { Gix->AddItem(Key, TGItem(uint(ItemN))); }
        // enqueue delete markers WITHOUT triggering a Def
        TVec<TGItem> DelV;
        for (int ItemN = 100; ItemN < 150; ItemN++) { DelV.Add(TGItem(uint(ItemN))); }
        Gix->DelItemV(Key, DelV);
        // this used to return 0 because the itemset was unmerged
        const uint64 Removed = Gix->DelItemsBelow(Key, TGItem(300u));
        EXPECT_EQ(uint64(250), Removed); // 0..299 minus the 50 already deleted
        TVec<TGItem> ItemV; Gix->GetItemV(Key, ItemV);
        ASSERT_EQ(200, ItemV.Len());
        EXPECT_EQ(uint(300), (uint)ItemV[0].Val);
        EXPECT_EQ(uint(499), (uint)ItemV.Last().Val);
    }
    TDir::DelNonEmptyDir(FPath);
}

// A5: a freed block in an EARLIER segment is found and reused by a later
// same-class allocation (the hint hash used to mismatch classes)
TEST(BugfixBatch2, BlobFreeBlockReusedAcrossSegments)
{
    const TStr FPath = "./bugfix2_blob/";
    FreshDir(FPath);
    {
        PBlobBs BlobBs = TMBlobBs::New(FPath + "seg", faCreate, 50000);
        TVec<TBlobPt> PtV;
        TMem Payload(9000);
        TRnd Rnd(5);
        for (int ChN = 0; ChN < 9000; ChN++) { Payload += char(Rnd.GetUniDevInt(0, 255) - 128); }
        for (int BlobN = 0; BlobN < 20; BlobN++) {
            PtV.Add(BlobBs->PutBlob(TMemIn::New(Payload)));
        }
        // multiple segments must exist and the first blob must be in segment 0
        EXPECT_GE((int)PtV.Last().GetSeg(), 3);
        ASSERT_EQ(0, (int)PtV[0].GetSeg());
        // free a block in segment 0, then allocate the same class again - the
        // freed block must be reused (same segment AND address)
        BlobBs->DelBlob(PtV[0]);
        const TBlobPt NewPt = BlobBs->PutBlob(TMemIn::New(Payload));
        EXPECT_EQ(0, (int)NewPt.GetSeg());
        EXPECT_EQ(PtV[0].GetAddr(), NewPt.GetAddr());
        // content still round-trips
        PSIn SIn = BlobBs->GetBlob(NewPt);
        ASSERT_EQ(9000, SIn->Len());
    }
    TDir::DelNonEmptyDir(FPath);
}

// B27 + A2: paged store with a datetime primary accepts numeric msecs, and the
// field-read memo stays correct across interleaved writes
TEST(BugfixBatch2, StoreTmPrimaryAndFieldReadMemo)
{
    const TStr FPath = "./bugfix2_store/";
    FreshDir(FPath);
    EnsureQmEnv();
    const TStr SchemaStr =
        "[{ \"name\": \"TmStore\","
        "   \"options\": { \"type\": \"paged\" },"
        "   \"fields\": [ { \"name\": \"Ts\", \"type\": \"datetime\", \"primary\": true },"
        "                 { \"name\": \"Num\", \"type\": \"int\" } ]"
        "}]";
    {
        PBase Base = TStorage::NewBase(FPath, TJsonVal::GetValFromStr(SchemaStr), 10000000, 10000000, true);
        TWPt<TStore> Store = Base->GetStoreByStoreNm("TmStore");
        // numeric msecs primary used to throw inside AddRec and SILENTLY drop the record
        const uint64 Rec0 = Store->AddRec(TJsonVal::GetValFromStr("{ \"Ts\": 13224268800000, \"Num\": 1 }"));
        ASSERT_NE(TUInt64::Mx, Rec0);
        // duplicate numeric primary resolves to the same record
        const uint64 RecDup = Store->AddRec(TJsonVal::GetValFromStr("{ \"Ts\": 13224268800000 }"));
        EXPECT_EQ(Rec0, RecDup);
        // string timestamps still work
        const uint64 Rec1 = Store->AddRec(TJsonVal::GetValFromStr("{ \"Ts\": \"2020-05-01T10:00:00\", \"Num\": 2 }"));
        ASSERT_NE(TUInt64::Mx, Rec1);
        ASSERT_NE(Rec0, Rec1);
        // field-read memo: read, write through UpdateRec, read again
        const int NumFieldId = Store->GetFieldId("Num");
        EXPECT_EQ(1, Store->GetFieldInt(Rec0, NumFieldId));
        Store->UpdateRec(Rec0, TJsonVal::GetValFromStr("{ \"Num\": 100 }"));
        EXPECT_EQ(100, Store->GetFieldInt(Rec0, NumFieldId));
        EXPECT_EQ(2, Store->GetFieldInt(Rec1, NumFieldId));
        EXPECT_EQ(100, Store->GetFieldInt(Rec0, NumFieldId));
        TStorage::SaveBase(Base);
    }
    TDir::DelNonEmptyDir(FPath);
}

// A3: range-limited index-join filtering returns exactly the records whose join
// targets fall in the range
TEST(BugfixBatch2, IndexJoinRangeFilter)
{
    const TStr FPath = "./bugfix2_join/";
    FreshDir(FPath);
    EnsureQmEnv();
    const TStr SchemaStr =
        "[{ \"name\": \"Parent\","
        "   \"fields\": [ { \"name\": \"Name\", \"type\": \"string\", \"primary\": true } ],"
        "   \"joins\": [ { \"name\": \"child\", \"type\": \"index\", \"store\": \"Child\" } ]"
        " },"
        " { \"name\": \"Child\","
        "   \"fields\": [ { \"name\": \"Name\", \"type\": \"string\", \"primary\": true } ],"
        "   \"joins\": [ { \"name\": \"parent\", \"type\": \"index\", \"store\": \"Parent\", \"inverse\": \"child\" } ]"
        "}]";
    {
        PBase Base = TStorage::NewBase(FPath, TJsonVal::GetValFromStr(SchemaStr), 10000000, 10000000, true);
        TWPt<TStore> Parent = Base->GetStoreByStoreNm("Parent");
        TWPt<TStore> Child = Base->GetStoreByStoreNm("Child");
        // parent p<N> joins to child c<N> (child rec ids are 0..9 in add order)
        for (int RecN = 0; RecN < 10; RecN++) {
            Parent->AddRec(TJsonVal::GetValFromStr(TStr::Fmt(
                "{ \"Name\": \"p%d\", \"child\": [ { \"Name\": \"c%d\" } ] }", RecN, RecN)));
        }
        ASSERT_EQ(10, (int)Child->GetRecs());
        // filter parents whose child rec id lies in [3, 6]
        const int JoinId = Parent->GetJoinId("child");
        PRecSet RecSet = Parent->GetAllRecs();
        ASSERT_EQ(10, RecSet->GetRecs());
        TRecFilterByIndexJoin JoinFilter(Parent, JoinId, 3, 6);
        RecSet->FilterBy(JoinFilter);
        ASSERT_EQ(4, RecSet->GetRecs());
        const int NameFieldId = Parent->GetFieldId("Name");
        TStrV NameV;
        for (int RecN = 0; RecN < RecSet->GetRecs(); RecN++) {
            NameV.Add(RecSet->GetRec(RecN).GetFieldStr(NameFieldId));
        }
        NameV.Sort();
        EXPECT_EQ(TStr("p3"), NameV[0]);
        EXPECT_EQ(TStr("p6"), NameV[3]);
        TStorage::SaveBase(Base);
    }
    TDir::DelNonEmptyDir(FPath);
}
