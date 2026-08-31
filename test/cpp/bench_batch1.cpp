/* Copyright (C) Event Registry d.o.o. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 */

// Micro-benchmarks for the 2026-08-31 perf batch (see docs/2026-08-31
// qminer-glib-deep-analysis.md in the backend repo):
//   - blob store churn (TGBlobBs put/update/del/relocate) - the S1 write-path fixes
//   - string-hash lookups (THash<TStr>, TStrHash)         - the S4 fused DJB hash
//   - posting-list memory accounting (GetMemUsed deep)    - the S2 is_shallow<TKeyDat>
//
// The benchmarks only use stable public APIs, so the same file builds before and
// after the changes - run it on both builds to compare. They are skipped unless the
// environment variable QM_BENCH is set, so the regular test run stays fast:
//   set QM_BENCH=1 && tests.exe --gtest_filter=PerfBench.*

#include <base.h>
#include "gtest/gtest.h"

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

// Blob-store churn: mimics gix eviction traffic - store mid-size itemset blobs,
// update them in place, grow them past their size class (relocation = del + put),
// and delete them. This is the path the S1 fixes target (padding writes, DelBlob
// zeroing, per-op flush, GetFLen syscalls).
TEST(PerfBench, BlobStoreChurn)
{
    if (!BenchEnabled()) { GTEST_SKIP() << "set QM_BENCH=1 to run"; }
    const TStr FPath = "./bench_blob/";
    ResetBenchDir(FPath);
    const int Blobs = 2000;
    const int Rounds = 5;

    TVec<TMem> PayloadV(3, 0);
    // sizes chosen to sit in the 100K-1M class band (25,000-byte steps -> real padding)
    const int SizeV[3] = { 130000, 260000, 520000 };
    TRnd Rnd(1);
    for (int PayloadN = 0; PayloadN < 3; PayloadN++) {
        TMem Mem(SizeV[PayloadN]);
        for (int ChN = 0; ChN < SizeV[PayloadN]; ChN++) { Mem += char(Rnd.GetUniDevInt(0, 255) - 128); }
        PayloadV.Add(Mem);
    }

    TBenchTimer Timer;
    {
        PBlobBs BlobBs = TMBlobBs::New(FPath + "bench", faCreate);
        TVec<TBlobPt> PtV(Blobs, 0);
        // initial stores (fresh allocations)
        for (int BlobN = 0; BlobN < Blobs; BlobN++) {
            PtV.Add(BlobBs->PutBlob(TMemIn::New(PayloadV[0])));
        }
        // rounds of same-class updates and cross-class relocations
        for (int RoundN = 0; RoundN < Rounds; RoundN++) {
            for (int BlobN = 0; BlobN < Blobs; BlobN++) {
                const TMem& Payload = PayloadV[(RoundN + BlobN) % 3];
                int ReleasedSize;
                PtV[BlobN] = BlobBs->PutBlob(PtV[BlobN], TMemIn::New(Payload), ReleasedSize);
            }
        }
        // delete every blob
        for (int BlobN = 0; BlobN < Blobs; BlobN++) {
            BlobBs->DelBlob(PtV[BlobN]);
        }
    }
    const double Sec = Timer.GetSec();
    printf("[bench] BlobStoreChurn: %d puts + %d updates + %d dels in %.3f s\n",
        Blobs, Blobs * Rounds, Blobs, Sec);
    TDir::DelNonEmptyDir(FPath);
    SUCCEED();
}

// Blob read path: cold gets of the blobs written above (GetBlob trailer/seek costs).
TEST(PerfBench, BlobGetChurn)
{
    if (!BenchEnabled()) { GTEST_SKIP() << "set QM_BENCH=1 to run"; }
    const TStr FPath = "./bench_blob_get/";
    ResetBenchDir(FPath);
    const int Blobs = 5000;
    const int Rounds = 10;
    TMem Payload(50000);
    TRnd Rnd(7);
    for (int ChN = 0; ChN < 50000; ChN++) { Payload += char(Rnd.GetUniDevInt(0, 255) - 128); }

    TVec<TBlobPt> PtV(Blobs, 0);
    {
        PBlobBs BlobBs = TMBlobBs::New(FPath + "bench", faCreate);
        for (int BlobN = 0; BlobN < Blobs; BlobN++) {
            PtV.Add(BlobBs->PutBlob(TMemIn::New(Payload)));
        }
    }
    PBlobBs BlobBs = TMBlobBs::New(FPath + "bench", faRdOnly);
    TBenchTimer Timer;
    uint64 Bytes = 0;
    for (int RoundN = 0; RoundN < Rounds; RoundN++) {
        for (int BlobN = 0; BlobN < Blobs; BlobN++) {
            PSIn SIn = BlobBs->GetBlob(PtV[BlobN]);
            Bytes += (uint64)SIn->Len();
        }
    }
    const double Sec = Timer.GetSec();
    printf("[bench] BlobGetChurn: %d gets (%.1f MB) in %.3f s\n",
        Blobs * Rounds, double(Bytes) / 1e6, Sec);
    BlobBs.Clr();
    TDir::DelNonEmptyDir(FPath);
    SUCCEED();
}

// String-hash probes: THash<TStr,TInt> and the vocabulary-style TStrHash<TInt>,
// mixing hits and misses - the S4 fused-DJB target. Key lengths mimic tokens/uris.
TEST(PerfBench, StringHashLookups)
{
    if (!BenchEnabled()) { GTEST_SKIP() << "set QM_BENCH=1 to run"; }
    const int Keys = 200000;
    const int Rounds = 20;

    TStrV KeyV(Keys, 0);
    TRnd Rnd(42);
    for (int KeyN = 0; KeyN < Keys; KeyN++) {
        const int WordLen = 4 + Rnd.GetUniDevInt(0, 30);
        TChA Word;
        for (int ChN = 0; ChN < WordLen; ChN++) { Word += char('a' + Rnd.GetUniDevInt(0, 25)); }
        Word += TInt::GetStr(KeyN).CStr(); // make keys unique
        KeyV.Add(TStr(Word));
    }

    {
        THash<TStr, TInt> H;
        for (int KeyN = 0; KeyN < Keys; KeyN++) { H.AddDat(KeyV[KeyN], KeyN); }
        TBenchTimer Timer;
        int64 Found = 0;
        for (int RoundN = 0; RoundN < Rounds; RoundN++) {
            for (int KeyN = 0; KeyN < Keys; KeyN++) {
                if (H.IsKey(KeyV[KeyN])) { Found++; }
            }
        }
        printf("[bench] THash<TStr> lookups: %d in %.3f s (found %lld)\n",
            Keys * Rounds, Timer.GetSec(), (long long)Found);
    }
    {
        TStrHash<TInt> SH;
        for (int KeyN = 0; KeyN < Keys; KeyN++) { SH.AddDat(KeyV[KeyN].CStr(), KeyN); }
        TBenchTimer Timer;
        int64 Found = 0;
        for (int RoundN = 0; RoundN < Rounds; RoundN++) {
            for (int KeyN = 0; KeyN < Keys; KeyN++) {
                if (SH.IsKey(KeyV[KeyN].CStr())) { Found++; }
            }
        }
        printf("[bench] TStrHash lookups: %d in %.3f s (found %lld)\n",
            Keys * Rounds, Timer.GetSec(), (long long)Found);
    }
    SUCCEED();
}

// Deep memory accounting over posting-list-shaped data: a TVec<TVec<TKeyDat>>
// mirrors TGixItemSet::ChildV; GetMemUsed(true) via TMemUtils is exactly what the
// gix cache accounting calls. The S2 is_shallow<TKeyDat> trait makes the inner
// walks O(1).
TEST(PerfBench, PostingListMemUsed)
{
    if (!BenchEnabled()) { GTEST_SKIP() << "set QM_BENCH=1 to run"; }
    typedef TKeyDat<TUInt64, TInt> TItem;
    const int Children = 500;
    const int ItemsPerChild = 100000;
    const int Rounds = 50;

    TVec<TVec<TItem> > ChildV(Children, 0);
    for (int ChildN = 0; ChildN < Children; ChildN++) {
        TVec<TItem> Child(ItemsPerChild, 0);
        for (int ItemN = 0; ItemN < ItemsPerChild; ItemN++) {
            Child.Add(TItem(uint64(ChildN) * ItemsPerChild + ItemN, 1));
        }
        ChildV.Add(TVec<TItem>());
        ChildV.Last().MoveFrom(Child);
    }

    TBenchTimer Timer;
    uint64 Total = 0;
    for (int RoundN = 0; RoundN < Rounds; RoundN++) {
        Total += TMemUtils::GetMemUsed(ChildV);
    }
    printf("[bench] PostingListMemUsed: %d deep GetMemUsed over %d x %d items in %.3f s (size %llu)\n",
        Rounds, Children, ItemsPerChild, Timer.GetSec(), (unsigned long long)(Total / Rounds));
    SUCCEED();
}
