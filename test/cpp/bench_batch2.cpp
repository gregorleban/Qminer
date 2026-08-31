/* Copyright (C) Event Registry d.o.o. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 */

// Micro-benchmarks for the group-4 (search path) changes: TVec bulk-op memcpy
// paths, merger reserve+move, gix AND short-circuit and smallest-first key
// ordering. Only stable public APIs, so the same file builds before and after
// the changes. Skipped unless QM_BENCH is set:
//   set QM_BENCH=1 && tests.exe --gtest_filter=PerfBench2.*

#include <qminer.h>
#include <qminer_storage.h>
#include "gtest/gtest.h"

using namespace TQm;

namespace {

bool BenchEnabled() { return getenv("QM_BENCH") != NULL; }

class TBenchTimer {
private:
    uint64 StartMSec;
public:
    TBenchTimer() : StartMSec(TTm::GetCurUniMSecs()) {}
    double GetSec() const { return double(TTm::GetCurUniMSecs() - StartMSec) / 1000.0; }
};

void ResetBenchDir(const TStr& FPath)
{
    if (TDir::Exists(FPath)) { TDir::DelNonEmptyDir(FPath); }
    TDir::GenDir(FPath);
}

} // namespace

// bulk vector operations on the gix full-item type: AddV (posting-list
// materialization), copy assignment (merger result hand-back), copy construction
TEST(PerfBench2, VecBulkOps)
{
    if (!BenchEnabled()) { GTEST_SKIP() << "set QM_BENCH=1 to run"; }
    typedef TKeyDat<TUInt64, TInt> TItem;
    const int Items = 2000000;
    const int Rounds = 60;

    TVec<TItem> Src(Items, 0);
    for (int ItemN = 0; ItemN < Items; ItemN++) { Src.Add(TItem(uint64(ItemN), 1)); }
    // split into 20 chunks to model per-child appends in GetItemV
    TVec<TVec<TItem> > ChunkV;
    const int ChunkLen = Items / 20;
    for (int ChunkN = 0; ChunkN < 20; ChunkN++) {
        TVec<TItem> Chunk;
        Src.GetSubValV(ChunkN * ChunkLen, (ChunkN + 1) * ChunkLen - 1, Chunk);
        ChunkV.Add(TVec<TItem>());
        ChunkV.Last().MoveFrom(Chunk);
    }

    {
        TBenchTimer Timer;
        uint64 Total = 0;
        for (int RoundN = 0; RoundN < Rounds; RoundN++) {
            TVec<TItem> Dst; Dst.Gen(Items, 0);
            for (int ChunkN = 0; ChunkN < 20; ChunkN++) { Dst.AddV(ChunkV[ChunkN]); }
            Total += (uint64)Dst.Len();
        }
        printf("[bench] VecBulkOps AddV: %d rounds x %d items in %.3f s (total %llu)\n",
            Rounds, Items, Timer.GetSec(), (unsigned long long)Total);
    }
    {
        TBenchTimer Timer;
        uint64 Total = 0;
        TVec<TItem> Dst;
        for (int RoundN = 0; RoundN < Rounds; RoundN++) {
            Dst = Src;
            Total += (uint64)Dst.Len();
        }
        printf("[bench] VecBulkOps assign: %d rounds x %d items in %.3f s (total %llu)\n",
            Rounds, Items, Timer.GetSec(), (unsigned long long)Total);
    }
    {
        TBenchTimer Timer;
        uint64 Total = 0;
        for (int RoundN = 0; RoundN < Rounds; RoundN++) {
            TVec<TItem> Dst(Src);
            Total += (uint64)Dst.Len();
        }
        printf("[bench] VecBulkOps copy-ctor: %d rounds x %d items in %.3f s (total %llu)\n",
            Rounds, Items, Timer.GetSec(), (unsigned long long)Total);
    }
    SUCCEED();
}

// end-to-end boolean queries over a schema base: AND with an empty first
// operand (short-circuit target), AND of two large posting lists, OR fan-out
TEST(PerfBench2, SearchQueries)
{
    if (!BenchEnabled()) { GTEST_SKIP() << "set QM_BENCH=1 to run"; }
    const TStr FPath = "./bench_search/";
    ResetBenchDir(FPath);
    if (!TQm::TEnv::IsInit()) { TQm::TEnv::Init(); }
    const TStr SchemaStr =
        "[{ \"name\": \"BenchStore\","
        "   \"fields\": [ { \"name\": \"Name\", \"type\": \"string\", \"primary\": true },"
        "                 { \"name\": \"Cat\", \"type\": \"string\" },"
        "                 { \"name\": \"Mod3\", \"type\": \"string\" },"
        "                 { \"name\": \"Rare\", \"type\": \"string\" } ],"
        "   \"keys\": [ { \"field\": \"Cat\", \"type\": \"value\" },"
        "               { \"field\": \"Mod3\", \"type\": \"value\" },"
        "               { \"field\": \"Rare\", \"type\": \"value\" } ]"
        "}]";
    const int Recs = 30000;
    PBase Base = TStorage::NewBase(FPath, TJsonVal::GetValFromStr(SchemaStr), 128 * 1024 * 1024, 128 * 1024 * 1024, true);
    TWPt<TStore> Store = Base->GetStoreByStoreNm("BenchStore");
    for (int RecN = 0; RecN < Recs; RecN++) {
        Store->AddRec(TJsonVal::GetValFromStr(TStr::Fmt(
            "{ \"Name\": \"r%d\", \"Cat\": \"%s\", \"Mod3\": \"m%d\", \"Rare\": \"u%d\" }",
            RecN, (RecN % 2 == 0) ? "even" : "odd", RecN % 3, RecN)));
    }

    {
        // AND whose first operand matches nothing: the fix should skip loading the
        // large Cat posting list entirely
        TBenchTimer Timer;
        for (int RoundN = 0; RoundN < 300; RoundN++) {
            PRecSet RecSet = Base->Search(
                "{ \"$from\": \"BenchStore\", \"Rare\": \"nonexistent\", \"Cat\": \"even\" }");
            ASSERT_EQ(0, RecSet->GetRecs());
        }
        printf("[bench] Search AND empty-first: 300 queries in %.3f s\n", Timer.GetSec());
    }
    {
        // AND of two large posting lists (15000 x 10000 -> 5000 hits)
        TBenchTimer Timer;
        for (int RoundN = 0; RoundN < 300; RoundN++) {
            PRecSet RecSet = Base->Search(
                "{ \"$from\": \"BenchStore\", \"Cat\": \"even\", \"Mod3\": \"m0\" }");
            ASSERT_EQ(Recs / 6, RecSet->GetRecs());
        }
        printf("[bench] Search AND big x big: 300 queries in %.3f s\n", Timer.GetSec());
    }
    {
        // OR fan-out over 100 rare values
        TChA QueryChA("{ \"$from\": \"BenchStore\", \"$or\": [");
        for (int ValN = 0; ValN < 100; ValN++) {
            if (ValN > 0) { QueryChA += ", "; }
            QueryChA += TStr::Fmt("{ \"Rare\": \"u%d\" }", ValN * 7);
        }
        QueryChA += " ] }";
        const TStr QueryStr(QueryChA);
        TBenchTimer Timer;
        for (int RoundN = 0; RoundN < 100; RoundN++) {
            PRecSet RecSet = Base->Search(QueryStr);
            ASSERT_EQ(100, RecSet->GetRecs());
        }
        printf("[bench] Search OR 100-way: 100 queries in %.3f s\n", Timer.GetSec());
    }

    TStorage::SaveBase(Base);
    Base.Clr();
    TDir::DelNonEmptyDir(FPath);
    SUCCEED();
}
