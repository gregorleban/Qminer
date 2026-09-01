/* Copyright (C) Event Registry d.o.o. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 */

// Micro-benchmarks for the batch-4 changes: per-record field access on paged
// stores (A2), JSON serialization/escaping (A6), TCache probe overhead (A7,
// visible through the gix/store paths), join filtering (A3, via DoJoin).
// Skipped unless QM_BENCH is set:
//   set QM_BENCH=1 && tests.exe --gtest_filter=PerfBench4.*

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

// result-rendering shape: read many fields of the same record, across many
// records - each GetFieldX used to pay a record-map probe + page probe + LRU
// splice on its own
TEST(PerfBench4, PagedStoreFieldReads)
{
    if (!BenchEnabled()) { GTEST_SKIP() << "set QM_BENCH=1 to run"; }
    const TStr FPath = "./bench_fieldread/";
    ResetBenchDir(FPath);
    if (!TQm::TEnv::IsInit()) { TQm::TEnv::Init(); }
    const TStr SchemaStr =
        "[{ \"name\": \"FRStore\","
        "   \"options\": { \"type\": \"paged\" },"
        "   \"fields\": [ { \"name\": \"Name\", \"type\": \"string\", \"primary\": true },"
        "                 { \"name\": \"I1\", \"type\": \"int\" },"
        "                 { \"name\": \"I2\", \"type\": \"int\" },"
        "                 { \"name\": \"F1\", \"type\": \"float\" },"
        "                 { \"name\": \"U1\", \"type\": \"uint64\" },"
        "                 { \"name\": \"S1\", \"type\": \"string\" },"
        "                 { \"name\": \"S2\", \"type\": \"string\" },"
        "                 { \"name\": \"B1\", \"type\": \"bool\" } ]"
        "}]";
    const int Recs = 20000;
    const int Rounds = 15;
    try {
        PBase Base = TStorage::NewBase(FPath, TJsonVal::GetValFromStr(SchemaStr), 128 * 1024 * 1024, 128 * 1024 * 1024, true);
        TWPt<TStore> Store = Base->GetStoreByStoreNm("FRStore");
        for (int RecN = 0; RecN < Recs; RecN++) {
            Store->AddRec(TJsonVal::GetValFromStr(TStr::Fmt(
                "{ \"Name\": \"r%d\", \"I1\": %d, \"I2\": %d, \"F1\": %d.5, \"U1\": %d,"
                "  \"S1\": \"some string value %d\", \"S2\": \"another value\", \"B1\": %s }",
                RecN, RecN, RecN * 2, RecN, RecN * 3, RecN, (RecN % 2 == 0) ? "true" : "false")));
        }
        const int I1 = Store->GetFieldId("I1"), I2 = Store->GetFieldId("I2");
        const int F1 = Store->GetFieldId("F1"), U1 = Store->GetFieldId("U1");
        const int S1 = Store->GetFieldId("S1"), S2 = Store->GetFieldId("S2");
        const int B1 = Store->GetFieldId("B1"), Nm = Store->GetFieldId("Name");
        TBenchTimer Timer;
        uint64 Total = 0;
        for (int RoundN = 0; RoundN < Rounds; RoundN++) {
            for (uint64 RecId = 0; RecId < (uint64)Recs; RecId++) {
                Total += (uint64)Store->GetFieldInt(RecId, I1);
                Total += (uint64)Store->GetFieldInt(RecId, I2);
                Total += (uint64)Store->GetFieldFlt(RecId, F1);
                Total += Store->GetFieldUInt64(RecId, U1);
                Total += (uint64)Store->GetFieldStr(RecId, S1).Len();
                Total += (uint64)Store->GetFieldStr(RecId, S2).Len();
                Total += Store->GetFieldBool(RecId, B1) ? 1 : 0;
                Total += (uint64)Store->GetFieldStr(RecId, Nm).Len();
            }
        }
        printf("[bench] PagedStoreFieldReads: %d reads in %.3f s (total %llu)\n",
            Recs * Rounds * 8, Timer.GetSec(), (unsigned long long)Total);
        TStorage::SaveBase(Base);
    } catch (PExcept Except) {
        FAIL() << Except->GetMsgStr().CStr();
    }
    TDir::DelNonEmptyDir(FPath);
    SUCCEED();
}

// JSON serialization of documents heavy on strings, escapes and non-ASCII -
// the escaping path used to run a TStr::Fmt per non-ASCII character
TEST(PerfBench4, JsonSaveStr)
{
    if (!BenchEnabled()) { GTEST_SKIP() << "set QM_BENCH=1 to run"; }
    // article-shaped object: ASCII body + a non-ASCII (Cyrillic-ish) body + escapes
    PJsonVal Val = TJsonVal::NewObj();
    TChA AsciiChA; TChA UniChA; TChA EscChA;
    TRnd Rnd(5);
    for (int WordN = 0; WordN < 500; WordN++) {
        for (int ChN = 0; ChN < 6; ChN++) { AsciiChA += char('a' + Rnd.GetUniDevInt(0, 25)); }
        AsciiChA += ' ';
        // 2-byte UTF-8 sequences (U+0410..U+042F, Cyrillic capitals)
        for (int ChN = 0; ChN < 4; ChN++) {
            const int Cp = 0x410 + Rnd.GetUniDevInt(0, 31);
            UniChA += char(0xC0 | (Cp >> 6)); UniChA += char(0x80 | (Cp & 0x3F));
        }
        UniChA += ' ';
        EscChA += "line\nwith\t\"quotes\" ";
    }
    Val->AddToObj("ascii", TStr(AsciiChA));
    Val->AddToObj("unicode", TStr(UniChA));
    Val->AddToObj("escapes", TStr(EscChA));
    Val->AddToObj("num", 42.5);

    const int Rounds = 3000;
    TBenchTimer Timer;
    uint64 Total = 0;
    for (int RoundN = 0; RoundN < Rounds; RoundN++) {
        Total += (uint64)Val->SaveStr().Len();
    }
    printf("[bench] JsonSaveStr: %d rounds in %.3f s (out size %llu)\n",
        Rounds, Timer.GetSec(), (unsigned long long)(Total / Rounds));
    SUCCEED();
}
