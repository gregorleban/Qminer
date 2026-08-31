/* Copyright (C) Event Registry d.o.o. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 */

// Tests for the TGBlobBs write-path changes (S1/B8/B9 of the 2026-08-31 deep
// analysis): padding is now seeked over instead of written as zeroes, DelBlob no
// longer erases the freed block, the per-op flushes are gone (the close-time
// header write + flush remains), fresh allocations use a cached file length and
// are gated so a block can never extend past the 32-bit ftell range, and
// faRdOnly opens segments actually read-only and refuses to conjure missing or
// empty segment files.
//
// The tests pin the externally observable contract: byte-exact round trips
// across put/update/delete/reuse/relocation, free-list reuse, persistence across
// close/reopen in every access mode, and multi-segment rollover in TMBlobBs.

#include <base.h>
#include "gtest/gtest.h"

namespace {

void ResetDir(const TStr& FPath)
{
    if (TDir::Exists(FPath)) { TDir::DelNonEmptyDir(FPath); }
    TDir::GenDir(FPath);
}

TMem MakePayload(const int& Len, const int& Seed)
{
    TMem Mem(Len);
    TRnd Rnd(Seed);
    for (int ChN = 0; ChN < Len; ChN++) { Mem += char(Rnd.GetUniDevInt(0, 255) - 128); }
    return Mem;
}

void CheckBlob(const PBlobBs& BlobBs, const TBlobPt& Pt, const TMem& Expect)
{
    PSIn SIn = BlobBs->GetBlob(Pt);
    ASSERT_EQ(Expect.Len(), SIn->Len());
    TMem Got(SIn->Len());
    for (int ChN = 0; ChN < Expect.Len(); ChN++) { Got += SIn->GetCh(); }
    EXPECT_EQ(0, memcmp(Expect.GetBf(), Got.GetBf(), Expect.Len()));
}

} // namespace

// byte-exact round trips for blobs of many sizes (each sits in a size class larger
// than the data, so the skipped-padding path runs every time), surviving reopen in
// faUpdate and faRdOnly
TEST(BlobbsWriteTests, PutGetRoundTripWithPadding)
{
    const TStr FPath = "./blobw_pad/";
    ResetDir(FPath);
    const int SizeV[6] = { 1, 137, 4999, 25000, 99999, 130001 };
    TVec<TMem> PayloadV;
    TVec<TBlobPt> PtV;
    {
        PBlobBs BlobBs = TGBlobBs::New(FPath + "pad", faCreate);
        for (int BlobN = 0; BlobN < 6; BlobN++) {
            PayloadV.Add(MakePayload(SizeV[BlobN], 100 + BlobN));
            PtV.Add(BlobBs->PutBlob(TMemIn::New(PayloadV[BlobN])));
            CheckBlob(BlobBs, PtV[BlobN], PayloadV[BlobN]);
        }
        // read back through the same handle after interleaved writes
        for (int BlobN = 0; BlobN < 6; BlobN++) { CheckBlob(BlobBs, PtV[BlobN], PayloadV[BlobN]); }
    }
    {
        PBlobBs BlobBs = TGBlobBs::New(FPath + "pad", faUpdate);
        for (int BlobN = 0; BlobN < 6; BlobN++) { CheckBlob(BlobBs, PtV[BlobN], PayloadV[BlobN]); }
    }
    {
        PBlobBs BlobBs = TGBlobBs::New(FPath + "pad", faRdOnly);
        for (int BlobN = 0; BlobN < 6; BlobN++) { CheckBlob(BlobBs, PtV[BlobN], PayloadV[BlobN]); }
    }
    TDir::DelNonEmptyDir(FPath);
}

// deleting a blob puts its block on the free list; the next same-class put must
// reuse the block (same address) and the reused block must read back byte-exact
// even though DelBlob no longer erases the old content
TEST(BlobbsWriteTests, DeleteAndReuseFreeBlock)
{
    const TStr FPath = "./blobw_reuse/";
    ResetDir(FPath);
    {
        PBlobBs BlobBs = TGBlobBs::New(FPath + "reuse", faCreate);
        const TMem PayloadA = MakePayload(150, 1);
        const TBlobPt PtA = BlobBs->PutBlob(TMemIn::New(PayloadA));
        CheckBlob(BlobBs, PtA, PayloadA);
        BlobBs->DelBlob(PtA);
        // 140 bytes falls into the same 200-byte class -> the freed block is reused
        const TMem PayloadB = MakePayload(140, 2);
        const TBlobPt PtB = BlobBs->PutBlob(TMemIn::New(PayloadB));
        EXPECT_EQ(PtA.GetAddr(), PtB.GetAddr()) << "freed block was not reused";
        CheckBlob(BlobBs, PtB, PayloadB);
        // a second delete/reuse cycle on the same block
        BlobBs->DelBlob(PtB);
        const TMem PayloadC = MakePayload(160, 3);
        const TBlobPt PtC = BlobBs->PutBlob(TMemIn::New(PayloadC));
        EXPECT_EQ(PtA.GetAddr(), PtC.GetAddr());
        CheckBlob(BlobBs, PtC, PayloadC);
    }
    TDir::DelNonEmptyDir(FPath);
}

// updating within the same size class stays in place; growing past the class
// relocates (delete + fresh put) and reports the released block, which a later
// same-class put picks up
TEST(BlobbsWriteTests, UpdateInPlaceAndGrowRelocation)
{
    const TStr FPath = "./blobw_grow/";
    ResetDir(FPath);
    TBlobPt PtSmallReuse;
    TMem PayloadBig;
    TBlobPt PtBig;
    {
        PBlobBs BlobBs = TGBlobBs::New(FPath + "grow", faCreate);
        const TMem PayloadA = MakePayload(150, 4);
        const TBlobPt PtA = BlobBs->PutBlob(TMemIn::New(PayloadA));
        // same-class update stays at the same address
        const TMem PayloadA2 = MakePayload(180, 5);
        int ReleasedSize = 0;
        const TBlobPt PtA2 = BlobBs->PutBlob(PtA, TMemIn::New(PayloadA2), ReleasedSize);
        EXPECT_EQ(PtA.GetAddr(), PtA2.GetAddr());
        EXPECT_EQ(-1, ReleasedSize);
        CheckBlob(BlobBs, PtA2, PayloadA2);
        // growth past the class relocates and releases the old block
        PayloadBig = MakePayload(5000, 6);
        PtBig = BlobBs->PutBlob(PtA2, TMemIn::New(PayloadBig), ReleasedSize);
        EXPECT_NE(PtA.GetAddr(), PtBig.GetAddr());
        EXPECT_EQ(200, ReleasedSize) << "released block size should be the old class size";
        CheckBlob(BlobBs, PtBig, PayloadBig);
        // the released 200-class block is reused by the next small put
        const TMem PayloadSmall = MakePayload(170, 7);
        PtSmallReuse = BlobBs->PutBlob(TMemIn::New(PayloadSmall));
        EXPECT_EQ(PtA.GetAddr(), PtSmallReuse.GetAddr());
        CheckBlob(BlobBs, PtSmallReuse, PayloadSmall);
    }
    {
        // everything persisted correctly across close/reopen
        PBlobBs BlobBs = TGBlobBs::New(FPath + "grow", faRdOnly);
        CheckBlob(BlobBs, PtBig, PayloadBig);
    }
    TDir::DelNonEmptyDir(FPath);
}

// TMBlobBs with a small segment cap: fresh allocations roll over into new
// segments once a segment is full, and every blob stays readable through
// faUpdate and faRdOnly reopens
TEST(BlobbsWriteTests, SegmentRolloverAndReopen)
{
    const TStr FPath = "./blobw_seg/";
    ResetDir(FPath);
    const int Blobs = 20;
    TVec<TMem> PayloadV;
    TVec<TBlobPt> PtV;
    {
        PBlobBs BlobBs = TMBlobBs::New(FPath + "seg", faCreate, 50000);
        for (int BlobN = 0; BlobN < Blobs; BlobN++) {
            PayloadV.Add(MakePayload(9000 + BlobN, 200 + BlobN));
            PtV.Add(BlobBs->PutBlob(TMemIn::New(PayloadV[BlobN])));
        }
        for (int BlobN = 0; BlobN < Blobs; BlobN++) { CheckBlob(BlobBs, PtV[BlobN], PayloadV[BlobN]); }
    }
    // the 20 x ~9KB blobs cannot fit one 50KB segment - several must exist
    int Segs = 0;
    while (TFile::Exists(FPath + "seg.mbb" + TStr::GetNrNumFExt(Segs, 5))) { Segs++; }
    EXPECT_GE(Segs, 3) << "expected the blobs to roll over into multiple segments";
    {
        PBlobBs BlobBs = TMBlobBs::New(FPath + "seg", faUpdate, 50000);
        for (int BlobN = 0; BlobN < Blobs; BlobN++) { CheckBlob(BlobBs, PtV[BlobN], PayloadV[BlobN]); }
    }
    {
        PBlobBs BlobBs = TMBlobBs::New(FPath + "seg", faRdOnly, 50000);
        for (int BlobN = 0; BlobN < Blobs; BlobN++) { CheckBlob(BlobBs, PtV[BlobN], PayloadV[BlobN]); }
    }
    TDir::DelNonEmptyDir(FPath);
}

// B9: a read-only open of a multi-segment base with a MISSING segment file must
// fail loudly at open time (it used to silently create an empty segment and fail
// much later, on the first read into it)
TEST(BlobbsWriteTests, RdOnlyMissingSegmentThrows)
{
    const TStr FPath = "./blobw_missing/";
    ResetDir(FPath);
    {
        PBlobBs BlobBs = TMBlobBs::New(FPath + "mis", faCreate, 50000);
        for (int BlobN = 0; BlobN < 20; BlobN++) {
            const TMem Payload = MakePayload(9000, 300 + BlobN);
            BlobBs->PutBlob(TMemIn::New(Payload));
        }
    }
    const TStr Seg1FNm = FPath + "mis.mbb" + TStr::GetNrNumFExt(1, 5);
    ASSERT_TRUE(TFile::Exists(Seg1FNm));
    TFile::Del(Seg1FNm);
    EXPECT_THROW(TMBlobBs::New(FPath + "mis", faRdOnly, 50000), PExcept);
    TDir::DelNonEmptyDir(FPath);
}

// B9: an EMPTY (zero-byte) blob file opened read-only must be refused instead of
// being silently initialized with fresh headers
TEST(BlobbsWriteTests, RdOnlyEmptyFileThrows)
{
    const TStr FPath = "./blobw_empty/";
    ResetDir(FPath);
    { TFOut FOut(FPath + "empty.gbb"); } // create a zero-byte file
    EXPECT_THROW(TGBlobBs::New(FPath + "empty", faRdOnly), PExcept);
    TDir::DelNonEmptyDir(FPath);
}

// stats stay coherent through put/update/del cycles (the write-path rework must
// not change the accounting)
TEST(BlobbsWriteTests, StatsStayCoherent)
{
    const TStr FPath = "./blobw_stats/";
    ResetDir(FPath);
    {
        PBlobBs BlobBs = TGBlobBs::New(FPath + "stats", faCreate);
        const TMem PayloadA = MakePayload(150, 8);
        const TBlobPt PtA = BlobBs->PutBlob(TMemIn::New(PayloadA));
        const TMem PayloadB = MakePayload(950, 9);
        BlobBs->PutBlob(TMemIn::New(PayloadB));
        BlobBs->DelBlob(PtA);
        const TBlobBsStats& Stats = BlobBs->GetStats();
        EXPECT_EQ(uint64(2), uint64(Stats.PutsNew));
        EXPECT_EQ(uint64(1), uint64(Stats.Dels));
        EXPECT_EQ(uint64(1), uint64(Stats.AllocCount)); // 2 allocs - 1 del
        EXPECT_EQ(uint64(1), uint64(Stats.ReleasedCount));
        EXPECT_EQ(uint64(200), uint64(Stats.ReleasedSize));
        EXPECT_TRUE(BlobBs->HasFreeBlobs());
    }
    TDir::DelNonEmptyDir(FPath);
}
