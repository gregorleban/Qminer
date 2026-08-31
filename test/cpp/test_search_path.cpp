/* Copyright (C) Event Registry d.o.o. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 */

// Tests for the group-4 search-path changes (2026-08-31 deep analysis):
//   - TVec bitwise-movable fast paths: AddV / copy ctor / operator= / Resize now
//     move flat elements with one memcpy - contents must stay identical for both
//     movable and non-movable element types, appends must stay amortized O(1)
//   - merger Union/Intrs reserve + move-back, Eval/_Search MoveFrom hand-backs
//   - AND short-circuit (empty non-negated prefix skips the remaining operands)
//     and smallest-first key ordering in SearchGixAnd - pure optimizations, so
//     query RESULTS must be exactly what the old evaluator returned
//
// The search tests build a small schema base and check exact result sets for
// value/AND/OR/NOT query shapes against counts derived from the data.

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
}

// chunked-append reference check for any element type
template <class TVal>
void CheckAddVChunks(const TVec<TVal>& Src, const int& Chunks)
{
    TVec<TVal> Expect;
    TVec<TVal> Got;
    const int ChunkLen = Src.Len() / Chunks;
    for (int ChunkN = 0; ChunkN < Chunks; ChunkN++) {
        const int From = ChunkN * ChunkLen;
        const int To = (ChunkN == Chunks - 1) ? Src.Len() - 1 : (ChunkN + 1) * ChunkLen - 1;
        TVec<TVal> Chunk; Src.GetSubValV(From, To, Chunk);
        Got.AddV(Chunk);
        for (int ValN = From; ValN <= To; ValN++) { Expect.Add(Src[ValN]); }
    }
    ASSERT_EQ(Expect.Len(), Got.Len());
    for (int ValN = 0; ValN < Expect.Len(); ValN++) {
        ASSERT_TRUE(Expect[ValN] == Got[ValN]) << "mismatch at " << ValN;
    }
}

} // namespace

// the memcpy AddV must produce identical contents for flat (movable) elements,
// keep the loop path working for owning elements, and handle self-append
TEST(SearchPathVecTests, AddVBulkAppend)
{
    typedef TKeyDat<TUInt64, TInt> TItem;
    TRnd Rnd(3);
    // movable element type, several chunkings incl. non-divisible lengths
    {
        TVec<TItem> Src;
        for (int ItemN = 0; ItemN < 5231; ItemN++) {
            Src.Add(TItem(uint64(Rnd.GetUniDevInt()), ItemN % 13));
        }
        CheckAddVChunks(Src, 1);
        CheckAddVChunks(Src, 7);
        CheckAddVChunks(Src, 50);
    }
    // empty appends are no-ops
    {
        TVec<TItem> Vec; Vec.Add(TItem(1, 1));
        TVec<TItem> Empty;
        Vec.AddV(Empty);
        ASSERT_EQ(1, Vec.Len());
        Empty.AddV(Vec);
        ASSERT_EQ(1, Empty.Len());
        EXPECT_TRUE(Empty[0] == TItem(1, 1));
    }
    // self-append: append a vector to itself (buffer moves under the source)
    {
        TVec<TInt> Vec;
        for (int ValN = 0; ValN < 17; ValN++) { Vec.Add(ValN); }
        Vec.AddV(Vec);
        ASSERT_EQ(34, Vec.Len());
        for (int ValN = 0; ValN < 17; ValN++) {
            ASSERT_EQ(ValN, (int)Vec[ValN]);
            ASSERT_EQ(ValN, (int)Vec[17 + ValN]);
        }
    }
    // non-movable element type stays on the element loop
    {
        TVec<TStr> Src;
        for (int ValN = 0; ValN < 101; ValN++) { Src.Add(TStr::Fmt("string-value-%d", ValN)); }
        CheckAddVChunks(Src, 4);
    }
}

// appending many small chunks must stay amortized O(1) per element (geometric
// growth), not reallocate to the exact size on every chunk
TEST(SearchPathVecTests, AddVGrowsGeometrically)
{
    typedef TKeyDat<TUInt64, TInt> TItem;
    TVec<TItem> Chunk(64, 0);
    for (int ItemN = 0; ItemN < 64; ItemN++) { Chunk.Add(TItem(uint64(ItemN), 1)); }
    TVec<TItem> Dst;
    int Reallocs = 0; int LastReserved = Dst.Reserved();
    for (int ChunkN = 0; ChunkN < 4096; ChunkN++) {
        Dst.AddV(Chunk);
        if (Dst.Reserved() != LastReserved) { Reallocs++; LastReserved = Dst.Reserved(); }
    }
    ASSERT_EQ(64 * 4096, Dst.Len());
    // 262144 elements from 64-element chunks: geometric growth needs ~20
    // reallocations; exact-size growth would need 4096
    EXPECT_LE(Reallocs, 40) << "AddV reallocates per chunk - growth is not geometric";
}

// copy construction and copy assignment through the memcpy path, including
// assignment into a vector with enough capacity (the buffer-reuse branch)
TEST(SearchPathVecTests, CopyCtorAndAssignBulk)
{
    typedef TKeyDat<TUInt64, TInt> TItem;
    TRnd Rnd(5);
    TVec<TItem> Src;
    for (int ItemN = 0; ItemN < 3001; ItemN++) {
        Src.Add(TItem(uint64(Rnd.GetUniDevInt()), ItemN));
    }
    // copy ctor
    TVec<TItem> Copy(Src);
    ASSERT_EQ(Src.Len(), Copy.Len());
    for (int ItemN = 0; ItemN < Src.Len(); ItemN++) { ASSERT_TRUE(Src[ItemN] == Copy[ItemN]); }
    // assignment into an empty vector
    TVec<TItem> Assigned;
    Assigned = Src;
    ASSERT_EQ(Src.Len(), Assigned.Len());
    for (int ItemN = 0; ItemN < Src.Len(); ItemN++) { ASSERT_TRUE(Src[ItemN] == Assigned[ItemN]); }
    // assignment into a vector with larger capacity (reuses the buffer)
    TVec<TItem> Preallocated(10000, 0);
    for (int ItemN = 0; ItemN < 10000; ItemN++) { Preallocated.Add(TItem(0, 0)); }
    Preallocated = Src;
    ASSERT_EQ(Src.Len(), Preallocated.Len());
    for (int ItemN = 0; ItemN < Src.Len(); ItemN++) { ASSERT_TRUE(Src[ItemN] == Preallocated[ItemN]); }
    // non-movable type keeps working
    TVec<TStr> StrSrc;
    for (int ValN = 0; ValN < 55; ValN++) { StrSrc.Add(TStr::Fmt("value-%d", ValN)); }
    TVec<TStr> StrCopy(StrSrc);
    TVec<TStr> StrAssigned; StrAssigned = StrSrc;
    for (int ValN = 0; ValN < 55; ValN++) {
        ASSERT_EQ(StrSrc[ValN], StrCopy[ValN]);
        ASSERT_EQ(StrSrc[ValN], StrAssigned[ValN]);
    }
}

// growth across many single Adds exercises the Resize memcpy carry-over
TEST(SearchPathVecTests, ResizeCarryOver)
{
    typedef TKeyDat<TUInt64, TInt> TItem;
    TVec<TItem> Vec;
    for (int ItemN = 0; ItemN < 100000; ItemN++) {
        Vec.Add(TItem(uint64(ItemN) * 3, ItemN % 7));
    }
    for (int ItemN = 0; ItemN < 100000; ItemN++) {
        ASSERT_EQ(uint64(ItemN) * 3, (uint64)Vec[ItemN].Key);
        ASSERT_EQ(ItemN % 7, (int)Vec[ItemN].Dat);
    }
}

// end-to-end boolean search correctness on a schema base - the short-circuit,
// smallest-first ordering and all the move/reserve changes must not change any
// result. 60 records: Cat = even/odd, Mod3 = m0/m1/m2, Rare = unique, and a
// text field with a shared and a unique token per record
TEST(SearchPathQueryTests, BooleanQueryResultsUnchanged)
{
    const TStr FPath = "./searchpath_query/";
    FreshDir(FPath);
    EnsureQmEnv();
    const TStr SchemaStr =
        "[{ \"name\": \"QStore\","
        "   \"fields\": [ { \"name\": \"Name\", \"type\": \"string\", \"primary\": true },"
        "                 { \"name\": \"Cat\", \"type\": \"string\" },"
        "                 { \"name\": \"Mod3\", \"type\": \"string\" },"
        "                 { \"name\": \"Rare\", \"type\": \"string\" },"
        "                 { \"name\": \"Txt\", \"type\": \"string\" } ],"
        "   \"keys\": [ { \"field\": \"Cat\", \"type\": \"value\" },"
        "               { \"field\": \"Mod3\", \"type\": \"value\" },"
        "               { \"field\": \"Rare\", \"type\": \"value\" },"
        "               { \"field\": \"Txt\", \"type\": \"text\" } ]"
        "}]";
    const int Recs = 60;
    {
        PBase Base = TStorage::NewBase(FPath, TJsonVal::GetValFromStr(SchemaStr), 10000000, 10000000, true);
        TWPt<TStore> Store = Base->GetStoreByStoreNm("QStore");
        for (int RecN = 0; RecN < Recs; RecN++) {
            Store->AddRec(TJsonVal::GetValFromStr(TStr::Fmt(
                "{ \"Name\": \"r%d\", \"Cat\": \"%s\", \"Mod3\": \"m%d\", \"Rare\": \"u%d\","
                "  \"Txt\": \"shared token%d\" }",
                RecN, (RecN % 2 == 0) ? "even" : "odd", RecN % 3, RecN, RecN)));
        }

        // single-value queries
        EXPECT_EQ(30, Base->Search("{ \"$from\": \"QStore\", \"Cat\": \"even\" }")->GetRecs());
        EXPECT_EQ(20, Base->Search("{ \"$from\": \"QStore\", \"Mod3\": \"m1\" }")->GetRecs());
        EXPECT_EQ(1, Base->Search("{ \"$from\": \"QStore\", \"Rare\": \"u17\" }")->GetRecs());
        EXPECT_EQ(0, Base->Search("{ \"$from\": \"QStore\", \"Rare\": \"nonexistent\" }")->GetRecs());

        // implicit AND: both orders, incl. an empty first operand (short-circuit path)
        EXPECT_EQ(10, Base->Search("{ \"$from\": \"QStore\", \"Cat\": \"even\", \"Mod3\": \"m0\" }")->GetRecs());
        EXPECT_EQ(10, Base->Search("{ \"$from\": \"QStore\", \"Mod3\": \"m0\", \"Cat\": \"even\" }")->GetRecs());
        EXPECT_EQ(0, Base->Search("{ \"$from\": \"QStore\", \"Rare\": \"nonexistent\", \"Cat\": \"even\" }")->GetRecs());
        EXPECT_EQ(0, Base->Search("{ \"$from\": \"QStore\", \"Cat\": \"even\", \"Rare\": \"nonexistent\" }")->GetRecs());
        {
            // AND result contents, not just the count
            PRecSet RecSet = Base->Search("{ \"$from\": \"QStore\", \"Cat\": \"odd\", \"Mod3\": \"m2\", \"Rare\": \"u5\" }");
            ASSERT_EQ(1, RecSet->GetRecs());
            EXPECT_EQ(TStr("r5"), RecSet->GetRec(0).GetFieldStr(Store->GetFieldId("Name")));
        }

        // $or, incl. overlapping operands (frequencies summed, records unique)
        EXPECT_EQ(40, Base->Search(
            "{ \"$from\": \"QStore\", \"$or\": [ { \"Mod3\": \"m0\" }, { \"Mod3\": \"m1\" } ] }")->GetRecs());
        EXPECT_EQ(30, Base->Search(
            "{ \"$from\": \"QStore\", \"$or\": [ { \"Cat\": \"even\" }, { \"Cat\": \"even\" } ] }")->GetRecs());
        EXPECT_EQ(31, Base->Search(
            "{ \"$from\": \"QStore\", \"$or\": [ { \"Cat\": \"even\" }, { \"Rare\": \"u1\" } ] }")->GetRecs());

        // $not as an AND operand (Minus branches)
        EXPECT_EQ(20, Base->Search(
            "{ \"$from\": \"QStore\", \"Cat\": \"even\", \"$not\": { \"Mod3\": \"m0\" } }")->GetRecs());
        EXPECT_EQ(30, Base->Search(
            "{ \"$from\": \"QStore\", \"Cat\": \"even\", \"$not\": { \"Rare\": \"nonexistent\" } }")->GetRecs());

        // text key: multi-token AND through SearchGixAnd with the smallest-first
        // ordering ("shared" matches all 60, "tokenN" exactly one)
        EXPECT_EQ(60, Base->Search("{ \"$from\": \"QStore\", \"Txt\": \"shared\" }")->GetRecs());
        {
            PRecSet RecSet = Base->Search("{ \"$from\": \"QStore\", \"Txt\": \"shared token7\" }");
            ASSERT_EQ(1, RecSet->GetRecs());
            EXPECT_EQ(TStr("r7"), RecSet->GetRec(0).GetFieldStr(Store->GetFieldId("Name")));
        }
        {
            PRecSet RecSet = Base->Search("{ \"$from\": \"QStore\", \"Txt\": \"token7 shared\" }");
            ASSERT_EQ(1, RecSet->GetRecs());
        }
        // unknown token in the AND makes it empty, whatever its position
        EXPECT_EQ(0, Base->Search("{ \"$from\": \"QStore\", \"Txt\": \"zzzunknown shared\" }")->GetRecs());
        EXPECT_EQ(0, Base->Search("{ \"$from\": \"QStore\", \"Txt\": \"shared zzzunknown\" }")->GetRecs());

        TStorage::SaveBase(Base);
    }
    // everything still correct after close/reopen (results served from disk)
    {
        PBase Base = TStorage::LoadBase(FPath, faRdOnly, 10000000, 10000000);
        EXPECT_EQ(10, Base->Search("{ \"$from\": \"QStore\", \"Cat\": \"even\", \"Mod3\": \"m0\" }")->GetRecs());
        EXPECT_EQ(0, Base->Search("{ \"$from\": \"QStore\", \"Rare\": \"nonexistent\", \"Cat\": \"even\" }")->GetRecs());
        EXPECT_EQ(1, Base->Search("{ \"$from\": \"QStore\", \"Txt\": \"shared token7\" }")->GetRecs());
    }
    TDir::DelNonEmptyDir(FPath);
}

// filtering a record set must keep exactly the passing records (FilterBy now
// moves the filtered vector instead of copying it)
TEST(SearchPathQueryTests, RecSetFilterByKeepsRecords)
{
    const TStr FPath = "./searchpath_filter/";
    FreshDir(FPath);
    EnsureQmEnv();
    const TStr SchemaStr =
        "[{ \"name\": \"FStore\","
        "   \"fields\": [ { \"name\": \"Name\", \"type\": \"string\", \"primary\": true },"
        "                 { \"name\": \"Num\", \"type\": \"int\" } ]"
        "}]";
    {
        PBase Base = TStorage::NewBase(FPath, TJsonVal::GetValFromStr(SchemaStr), 10000000, 10000000, true);
        TWPt<TStore> Store = Base->GetStoreByStoreNm("FStore");
        for (int RecN = 0; RecN < 100; RecN++) {
            Store->AddRec(TJsonVal::GetValFromStr(TStr::Fmt("{ \"Name\": \"r%d\", \"Num\": %d }", RecN, RecN)));
        }
        PRecSet RecSet = Store->GetAllRecs();
        ASSERT_EQ(100, RecSet->GetRecs());
        const int NumFieldId = Store->GetFieldId("Num");
        RecSet->FilterByFieldInt(NumFieldId, 10, 29); // keep Num in [10, 29]
        ASSERT_EQ(20, RecSet->GetRecs());
        for (int RecN = 0; RecN < RecSet->GetRecs(); RecN++) {
            const int Num = RecSet->GetRec(RecN).GetFieldInt(NumFieldId);
            EXPECT_TRUE(10 <= Num && Num <= 29);
        }
        TStorage::SaveBase(Base);
    }
    TDir::DelNonEmptyDir(FPath);
}
