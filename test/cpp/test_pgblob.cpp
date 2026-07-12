#include <base.h>

#include "gtest/gtest.h"

// regression tests for the space-reclamation fixes in TPgBlob (pgblob.cpp):
// 1. DeleteItem's empty-page reset had its condition inverted (Len==0 marks a
//    DELETED slot) so a page was never reset and ItemCount grew monotonically.
// 2. Put(Bf, BfL) never reused Len==0 slots - delete/insert churn exhausted the
//    ~2044-entry item directory of every page and forced new pages forever.
// 3. Put(Bf, BfL, Pt) resize path: with fix 1 active, deleting the last live
//    item resets the page, so patching the old slot index would corrupt it.

static TChA PgBlobTestBf(const int& Len, const char& Ch) {
    TChA ChA;
    while (ChA.Len() < Len) { ChA += Ch; }
    return ChA;
}

// deleted slots must be reused by subsequent inserts (fix 2)
TEST(PgBlob, DeletedSlotReuse) {
    TDir::GenDir("./test/cpp/files/pgblob");
    PPgBlob Blob = TPgBlob::Create("./test/cpp/files/pgblob/slot_reuse");
    const TChA Bf = PgBlobTestBf(500, 'x');

    TVec<TPgBlobPt> PtV;
    for (int i = 0; i < 4; i++) { PtV.Add(Blob->Put(Bf.CStr(), 500)); }
    for (int i = 0; i < 4; i++) {
        EXPECT_EQ(0, (int)PtV[i].GetPg());
        EXPECT_EQ(i, (int)PtV[i].GetIIx());
    }

    Blob->Del(PtV[1]);
    const TPgBlobPt NewPt = Blob->Put(Bf.CStr(), 500);
    EXPECT_EQ(0, (int)NewPt.GetPg());
    // must land in the freed slot 1, not grow the item table to slot 4
    EXPECT_EQ(1, (int)NewPt.GetIIx());
    EXPECT_EQ(500, Blob->GetMemBase(NewPt).Len());
}

// a page whose items are all deleted must be fully reusable again (fix 1)
TEST(PgBlob, PageResetAfterDeleteAll) {
    TDir::GenDir("./test/cpp/files/pgblob");
    PPgBlob Blob = TPgBlob::Create("./test/cpp/files/pgblob/page_reset");
    const TChA Bf = PgBlobTestBf(500, 'x');

    TVec<TPgBlobPt> PtV;
    for (int i = 0; i < 4; i++) { PtV.Add(Blob->Put(Bf.CStr(), 500)); }
    for (int i = 0; i < 4; i++) { Blob->Del(PtV[i]); }

    const TPgBlobPt NewPt = Blob->Put(Bf.CStr(), 500);
    EXPECT_EQ(0, (int)NewPt.GetPg());
    // the empty-page reset must have cleared the item table
    EXPECT_EQ(0, (int)NewPt.GetIIx());
}

// delete/insert churn must not allocate new pages without bound (fixes 1+2:
// before them each cycle grew the item directory until the page was dead)
TEST(PgBlob, ChurnDoesNotGrowFile) {
    TDir::GenDir("./test/cpp/files/pgblob");
    PPgBlob Blob = TPgBlob::Create("./test/cpp/files/pgblob/churn");
    const TChA Bf = PgBlobTestBf(3000, 'y');

    uint32 MxPg = 0;
    for (int CycleN = 0; CycleN < 3000; CycleN++) {
        const TPgBlobPt P1 = Blob->Put(Bf.CStr(), 3000);
        const TPgBlobPt P2 = Blob->Put(Bf.CStr(), 3000);
        MxPg = MAX(MxPg, MAX(P1.GetPg(), P2.GetPg()));
        Blob->Del(P1);
        Blob->Del(P2);
    }
    // with the bugs this reached ~2 slots/cycle -> a page died every ~1000
    // cycles and MxPg grew past 2; fixed code recycles the first page(s)
    EXPECT_LE((int)MxPg, 1);
}

// resizing the only item on a page triggers the empty-page reset inside the
// update path - the returned pt must stay valid and later inserts must not
// clobber it (fix 3)
TEST(PgBlob, UpdateLastLiveItemOnPage) {
    TDir::GenDir("./test/cpp/files/pgblob");
    PPgBlob Blob = TPgBlob::Create("./test/cpp/files/pgblob/update_last");
    const TChA BfA = PgBlobTestBf(1000, 'a');
    const TChA BfB = PgBlobTestBf(1200, 'b');

    const TPgBlobPt Pt = Blob->Put(BfA.CStr(), 1000);
    // different size that still fits in the page -> DeleteItem + re-add; the
    // delete empties the page and resets it
    const TPgBlobPt Pt2 = Blob->Put(BfB.CStr(), 1200, Pt);
    TMemBase Mem = Blob->GetMemBase(Pt2);
    ASSERT_EQ(1200, Mem.Len());
    EXPECT_EQ(0, memcmp(Mem.GetBf(), BfB.CStr(), 1200));

    // a subsequent insert must get its own slot, leaving Pt2's record intact
    const TPgBlobPt Pt3 = Blob->Put(BfA.CStr(), 1000);
    EXPECT_FALSE(Pt3.GetPg() == Pt2.GetPg() && Pt3.GetIIx() == Pt2.GetIIx());
    Mem = Blob->GetMemBase(Pt2);
    ASSERT_EQ(1200, Mem.Len());
    EXPECT_EQ(0, memcmp(Mem.GetBf(), BfB.CStr(), 1200));
    Mem = Blob->GetMemBase(Pt3);
    ASSERT_EQ(1000, Mem.Len());
    EXPECT_EQ(0, memcmp(Mem.GetBf(), BfA.CStr(), 1000));
}

TEST(PgBlob, TPgBlob1) {
    return;

    PPgBlob Blob = TPgBlob::Create("./test/cpp/data/test_pgblob");

    TStr Str1 = "1234567890";
    TStr Str2 = "ABCDEFGHIJ";
    const int Recs = 10;

    TChA ChA1;
    while (ChA1.Len() < 10000) { ChA1 += Str1; }
    TChA ChA2;
    while (ChA2.Len() < 10000) { ChA2 += Str2; }

    TVec<TPgBlobPt> PtV;
    for (int i = 0; i < Recs; i++) {
        TMOut MOut; TStr::Fmt("%s_%02d", ChA1.CStr(), i).Save(MOut);
        PtV.Add(Blob->Put(MOut.GetBfAddr(), MOut.Len()));
        printf("%d Pt: f%d:p%d:i%d\n", i, PtV[i].GetFIx(), PtV[i].GetPg(), PtV[i].GetIIx());
    }
    printf("----------------\n");

    for (int i = 0; i < Recs/2; i++) {
        Blob->Del(PtV[i]);
    }
    printf("----------------\n");

    for (int i = 0; i < Recs; i++) {
        TMOut MOut; TStr::Fmt("%s_%02d", ChA2.CStr(), i).Save(MOut);
        const TPgBlobPt Pt = Blob->Put(MOut.GetBfAddr(), MOut.Len());
        printf("%d Pt: f%d:p%d:i%d\n", i, Pt.GetFIx(), Pt.GetPg(), Pt.GetIIx());
    }
}

TEST(PgBlob, TPgBlob2) {
    return;

    PPgBlob Blob = TPgBlob::Create("./test/cpp/data/test_pgblob", TInt::Mega);

    const int Iters = 50;
    const int Recs = 100000;
    const int PagesPerFile = 2000000000 / 8192;

    printf("Iters: %d, Recs: %d, PagesPerFile: %d\n", Iters, Recs, PagesPerFile);

    TStr Str = "1234567890";

    THash<TPgBlobPt, TInt> PtSizeH;
    for (int IterN = 0; IterN < Iters; IterN++) {
        uint64 StartTm = TTm::GetCurUniMSecs();
        // insert records
        for (int i = 0; i < Recs; i++) {
            const int RecSize = TInt::Rnd.GetUniDevInt(10, 6000);
            TChA ChA; while (ChA.Len() < RecSize) { ChA += Str; }
            TMOut MOut; ChA.Save(MOut);

            const TPgBlobPt Pt = Blob->Put(MOut.GetBfAddr(), MOut.Len());
            PtSizeH.AddDat(Pt, MOut.Len());
        }
        const uint64 InsertTm = TTm::GetCurUniMSecs() - StartTm;

        // find max file and page number
        TIntPr MxPg(-1, -1);
        int PtKeyId = PtSizeH.FFirstKeyId();
        while (PtSizeH.FNextKeyId(PtKeyId)) {
            const TPgBlobPt Pt = PtSizeH.GetKey(PtKeyId);
            TIntPr PgVal = { Pt.GetFIx(), Pt.GetPg() };
            if (MxPg < PgVal) { MxPg = PgVal; }
        }

        // delete half of the records per iteration
        TVec<TPgBlobPt> PtV; PtSizeH.GetKeyV(PtV);
        PtV.Shuffle(TInt::Rnd); PtV.Trunc(Recs / 2);
        StartTm = TTm::GetCurUniMSecs();
        for (const auto& Pt : PtV) {
            Blob->Del(Pt);
            PtSizeH.DelKey(Pt);
        }
        const uint64 DeleteTm = TTm::GetCurUniMSecs() - StartTm;

        double StoredSize = 0;
        PtKeyId = PtSizeH.FFirstKeyId();
        while (PtSizeH.FNextKeyId(PtKeyId)) {
            StoredSize += PtSizeH[PtKeyId];
        }

        const double Ratio = StoredSize / (MxPg.Val1 * PagesPerFile + MxPg.Val2);
        printf("Iter: %d, PtSet.Len(): %d, MxPg: %d:%d, Ratio: %.2f, InsertTm: %d, DeleteTm: %d\n",
            IterN, PtSizeH.Len(), MxPg.Val1.Val, MxPg.Val2.Val, Ratio, int(InsertTm), int(DeleteTm));
    }
}