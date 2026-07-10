#include <base.h>

#include "gtest/gtest.h"

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