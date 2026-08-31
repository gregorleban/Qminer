/* Copyright (C) Event Registry d.o.o. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 */

// Micro-benchmarks for the group-5 (ingest cluster) changes: gix key-lookup
// dedup, TVec::Merge in-place fast path, JSON lexer/parser dead-work removal,
// AddRec fixes (primary-field hoist, inserted_at guard, field-name copies).
// Only stable public APIs - the same file builds before and after. Skipped
// unless QM_BENCH is set:
//   set QM_BENCH=1 && tests.exe --gtest_filter=PerfBench3.*

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

// per-article gix fan-out: AddItem over many existing keys (the ingest hot
// path pays the key-dictionary lookups here), then batch deletes
TEST(PerfBench3, GixIngestChurn)
{
    if (!BenchEnabled()) { GTEST_SKIP() << "set QM_BENCH=1 to run"; }
    const TStr FPath = "./bench_gixingest/";
    ResetBenchDir(FPath);
    typedef TIntUInt64Pr TBKey;
    typedef TUInt TBItem;
    const int Keys = 2000;      // shared vocabulary of keys
    const int Articles = 20000;
    const int KeysPerArticle = 50;

    TGixDefItemHandler<TBKey, TBItem> ItemHandler;
    try {
        TPt<TGix<TBKey, TBItem> > Gix = TGix<TBKey, TBItem>::New("BenchGix", FPath,
            faCreate, &ItemHandler, 256 * 1024 * 1024, 1000, true, 500, 2000);
        TBenchTimer Timer;
        TRnd Rnd(1);
        for (int ArticleN = 0; ArticleN < Articles; ArticleN++) {
            const int Base = Rnd.GetUniDevInt(0, Keys - 1);
            for (int KeyN = 0; KeyN < KeysPerArticle; KeyN++) {
                Gix->AddItem(TBKey((Base + KeyN) % Keys, 1), TBItem((uint)ArticleN));
            }
        }
        const double AddSec = Timer.GetSec();
        // batch-delete every 10th article from its keys
        TBenchTimer DelTimer;
        TRnd DelRnd(1);
        for (int ArticleN = 0; ArticleN < Articles; ArticleN += 10) {
            const int Base = DelRnd.GetUniDevInt(0, Keys - 1);
            for (int KeyN = 0; KeyN < KeysPerArticle; KeyN++) {
                Gix->DelItem(TBKey((Base + KeyN) % Keys, 1), TBItem((uint)ArticleN));
            }
            // keep the random sequence aligned with the add loop
            for (int SkipN = 1; SkipN < 10 && ArticleN + SkipN < Articles; SkipN++) { DelRnd.GetUniDevInt(0, Keys - 1); }
        }
        printf("[bench] GixIngestChurn: %d adds in %.3f s, %d dels in %.3f s\n",
            Articles * KeysPerArticle, AddSec, (Articles / 10) * KeysPerArticle, DelTimer.GetSec());
    } catch (PExcept Except) {
        FAIL() << Except->GetMsgStr().CStr();
    }
    TDir::DelNonEmptyDir(FPath);
    SUCCEED();
}

// TVec::Merge on the shapes Def() feeds it: already-sorted unique (deletes-only
// flush), sorted with duplicates, and unsorted with duplicates
TEST(PerfBench3, VecMerge)
{
    if (!BenchEnabled()) { GTEST_SKIP() << "set QM_BENCH=1 to run"; }
    const int Items = 100000;
    const int Rounds = 300;
    TRnd Rnd(2);

    TVec<TUInt> SortedUniqueV(Items, 0);
    for (int ItemN = 0; ItemN < Items; ItemN++) { SortedUniqueV.Add(TUInt(uint(ItemN))); }
    TVec<TUInt> SortedDupV(Items, 0);
    for (int ItemN = 0; ItemN < Items; ItemN++) { SortedDupV.Add(TUInt(uint(ItemN / 2))); }
    TVec<TUInt> UnsortedV(Items, 0);
    for (int ItemN = 0; ItemN < Items; ItemN++) { UnsortedV.Add(TUInt(uint(Rnd.GetUniDevInt()))); }

    {
        TBenchTimer Timer;
        uint64 Total = 0;
        for (int RoundN = 0; RoundN < Rounds; RoundN++) {
            TVec<TUInt> WorkV = SortedUniqueV;
            WorkV.Merge();
            Total += (uint64)WorkV.Len();
        }
        printf("[bench] VecMerge sorted-unique: %d rounds x %d items in %.3f s (total %llu)\n",
            Rounds, Items, Timer.GetSec(), (unsigned long long)Total);
    }
    {
        TBenchTimer Timer;
        uint64 Total = 0;
        for (int RoundN = 0; RoundN < Rounds; RoundN++) {
            TVec<TUInt> WorkV = SortedDupV;
            WorkV.Merge();
            Total += (uint64)WorkV.Len();
        }
        printf("[bench] VecMerge sorted-dups: %d rounds x %d items in %.3f s (total %llu)\n",
            Rounds, Items, Timer.GetSec(), (unsigned long long)Total);
    }
    {
        TBenchTimer Timer;
        uint64 Total = 0;
        for (int RoundN = 0; RoundN < Rounds; RoundN++) {
            TVec<TUInt> WorkV = UnsortedV;
            WorkV.Merge();
            Total += (uint64)WorkV.Len();
        }
        printf("[bench] VecMerge unsorted: %d rounds x %d items in %.3f s (total %llu)\n",
            Rounds, Items, Timer.GetSec(), (unsigned long long)Total);
    }
    SUCCEED();
}

// JSON parsing of an article-shaped document (strings dominate, some escapes
// and non-ASCII), plus defaulted GetObj* reads
TEST(PerfBench3, JsonParse)
{
    if (!BenchEnabled()) { GTEST_SKIP() << "set QM_BENCH=1 to run"; }
    // build a ~6KB article-like JSON
    TChA DocChA("{ \"uri\": \"article-123456789\", \"lang\": \"eng\", \"date\": \"2026-08-31\","
        " \"title\": \"Some reasonably long article title with \\\"quotes\\\" and more\",");
    DocChA += " \"body\": \"";
    TRnd Rnd(3);
    for (int WordN = 0; WordN < 700; WordN++) {
        for (int ChN = 0; ChN < 3 + Rnd.GetUniDevInt(0, 7); ChN++) { DocChA += char('a' + Rnd.GetUniDevInt(0, 25)); }
        DocChA += (WordN % 47 == 0) ? "\\n" : " ";
    }
    DocChA += "\", \"concepts\": [";
    for (int ConceptN = 0; ConceptN < 30; ConceptN++) {
        if (ConceptN > 0) { DocChA += ","; }
        DocChA += TStr::Fmt("{ \"uri\": \"concept-%d\", \"score\": %d.5 }", ConceptN, ConceptN);
    }
    DocChA += "] }";
    const TStr DocStr(DocChA);
    printf("[bench] JsonParse: document size %d bytes\n", DocStr.Len());

    const int Rounds = 5000;
    {
        TBenchTimer Timer;
        uint64 Total = 0;
        for (int RoundN = 0; RoundN < Rounds; RoundN++) {
            PJsonVal Val = TJsonVal::GetValFromStr(DocStr);
            Total += (uint64)Val->GetObjKeys();
        }
        printf("[bench] JsonParse parse: %d rounds in %.3f s (keys %llu)\n",
            Rounds, Timer.GetSec(), (unsigned long long)Total);
    }
    {
        PJsonVal Val = TJsonVal::GetValFromStr(DocStr);
        TBenchTimer Timer;
        uint64 Total = 0;
        for (int RoundN = 0; RoundN < Rounds * 20; RoundN++) {
            Total += (uint64)Val->GetObjStr("lang", "unk").Len();
            Total += (uint64)(int)Val->GetObjNum("missing", 7.0);
            Total += (uint64)Val->GetObjStr("uri", "").Len();
        }
        printf("[bench] JsonParse GetObj defaults: %d reads in %.3f s (total %llu)\n",
            Rounds * 20 * 3, Timer.GetSec(), (unsigned long long)Total);
    }
    SUCCEED();
}

// full AddRec ingest into a store with a primary string field plus value and
// text keys - covers serialization, primary-map upkeep and index fan-out
TEST(PerfBench3, StoreIngest)
{
    if (!BenchEnabled()) { GTEST_SKIP() << "set QM_BENCH=1 to run"; }
    const TStr FPath = "./bench_ingest/";
    ResetBenchDir(FPath);
    if (!TQm::TEnv::IsInit()) { TQm::TEnv::Init(); }
    // the text-key tokenizer needs the unicode tables (path relative to the qminer
    // repo root, where the suite runs)
    if (!TUnicodeDef::IsDef()) { TUnicodeDef::Load("./src/glib/bin/UnicodeDef.Bin"); }
    const TStr SchemaStr =
        "[{ \"name\": \"IngestStore\","
        "   \"options\": { \"type\": \"paged\" },"
        "   \"fields\": [ { \"name\": \"Name\", \"type\": \"string\", \"primary\": true },"
        "                 { \"name\": \"Cat\", \"type\": \"string\" },"
        "                 { \"name\": \"Txt\", \"type\": \"string\" } ],"
        "   \"keys\": [ { \"field\": \"Cat\", \"type\": \"value\" },"
        "               { \"field\": \"Txt\", \"type\": \"text\" } ]"
        "}]";
    const int Recs = 20000;
    // pre-build the record JSONs so parsing them is outside the timer
    TVec<PJsonVal> RecValV(Recs, 0);
    TRnd Rnd(4);
    for (int RecN = 0; RecN < Recs; RecN++) {
        TChA TxtChA;
        for (int WordN = 0; WordN < 40; WordN++) {
            TxtChA += TStr::Fmt("word%d ", Rnd.GetUniDevInt(0, 5000));
        }
        RecValV.Add(TJsonVal::GetValFromStr(TStr::Fmt(
            "{ \"Name\": \"rec-%d\", \"Cat\": \"cat%d\", \"Txt\": \"%s\" }",
            RecN, RecN % 100, TxtChA.CStr())));
    }
    try {
        PBase Base = TStorage::NewBase(FPath, TJsonVal::GetValFromStr(SchemaStr), 256 * 1024 * 1024, 256 * 1024 * 1024, true);
        TWPt<TStore> Store = Base->GetStoreByStoreNm("IngestStore");
        TBenchTimer Timer;
        for (int RecN = 0; RecN < Recs; RecN++) {
            Store->AddRec(RecValV[RecN]);
        }
        printf("[bench] StoreIngest: %d AddRec in %.3f s\n", Recs, Timer.GetSec());
        TStorage::SaveBase(Base);
    } catch (PExcept Except) {
        FAIL() << Except->GetMsgStr().CStr();
    }
    TDir::DelNonEmptyDir(FPath);
    SUCCEED();
}
