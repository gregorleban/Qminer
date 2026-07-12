/**
 * Stress / edge-case tests for index and store defragmentation on branch
 * fast-load-save:
 *   - TGix::CopyTo re-chunking to a SMALLER per-key split length, keys with
 *     many child vectors, and copies after item deletions (including a key
 *     whose items were all deleted)
 *   - TGix::VerifySample must return true for identical copies, and false for
 *     a deliberately corrupted destination
 *   - TStorePbBlob::DefragTo with every-2nd-record deletes (maximum number of
 *     id gaps) and multi-100KB TOAST-ed field values
 *
 * QmDefragEdgeTests covers the empty/boundary cases, GixDefragTests the happy
 * paths; this file covers adversarial shapes on top of those.
 */

#include <qminer.h>
#include <qminer_storage.h>
#include "gtest/gtest.h"

using namespace TQm;
using namespace TQm::TStorage;

namespace {

typedef TIntUInt64Pr TStressGixKey;
typedef TUInt TStressGixItem;
typedef TPt<TGix<TStressGixKey, TStressGixItem> > PStressGix;

// provider that returns the split length based on the first value in the key pair
class TStressSplitLenProvider : public TGixSplitLenProvider<TStressGixKey> {
public:
    THash<TInt, TInt> KeyIdSplitLenH;

    int GetSplitLen(const TStressGixKey& Key) const {
        TInt SplitLen;
        if (KeyIdSplitLenH.IsKeyGetDat(Key.Val1, SplitLen)) { return SplitLen; }
        return -1;
    }
};

void FreshDir(const TStr& FPath)
{
    if (TDir::Exists(FPath)) { TDir::DelNonEmptyDir(FPath); }
    TDir::GenDir(FPath);
}

PStressGix NewStressGix(const TStr& FPath, const TFAccess& Access,
    TGixDefItemHandler<TStressGixKey, TStressGixItem>& ItemHandler, const int& SplitLen)
{
    return TGix<TStressGixKey, TStressGixItem>::New("GixStress", FPath, Access,
        &ItemHandler, 10000000, SplitLen, true, SplitLen / 2, SplitLen * 2);
}

} // namespace

// the smallest non-empty index: one key with one item; sampling must succeed
// on it and on a completely empty pair of gix instances
TEST(QmDefragStressTests, CopyToSingleKeySingleItemAndVerifySample)
{
    const TStr SrcFPath = "./gixstress_single_src/";
    const TStr DestFPath = "./gixstress_single_dest/";
    FreshDir(SrcFPath); FreshDir(DestFPath);
    TGixDefItemHandler<TStressGixKey, TStressGixItem> ItemHandler;
    const TStressGixKey Key(1, 1);
    {
        PStressGix Src = NewStressGix(SrcFPath, faCreate, ItemHandler, 100);
        PStressGix Dest = NewStressGix(DestFPath, faCreate, ItemHandler, 100);
        // empty-vs-empty sampling must trivially pass (also with sample size 0)
        EXPECT_TRUE(Src->VerifySample(*Dest, 10));
        EXPECT_TRUE(Src->VerifySample(*Dest, 0));

        Src->AddItem(Key, TStressGixItem(77u));
        Src->CopyTo(*Dest);
        ASSERT_EQ(1, Dest->GetKeys());
        EXPECT_TRUE(Src->IsKeyDataEqual(*Dest, Key));
        EXPECT_TRUE(Src->VerifySample(*Dest, 10));
        EXPECT_EQ(0, Dest->GetItemSet(Key)->GetChildVectors());
    }
    {
        PStressGix Dest = NewStressGix(DestFPath, faRdOnly, ItemHandler, 100);
        TVec<TStressGixItem> ItemV; Dest->GetItemV(Key, ItemV);
        ASSERT_EQ(1, ItemV.Len());
        EXPECT_EQ(77, (int)ItemV[0].Val);
    }
    TDir::DelNonEmptyDir(SrcFPath);
    TDir::DelNonEmptyDir(DestFPath);
}

// GixDefragTests covers re-chunking 100 -> 500 (larger); the opposite
// direction re-chunks each 500-item source child into many 50-item destination
// children, so a single AddItemV in the copy handler spans several splits
TEST(QmDefragStressTests, CopyToRechunksToSmallerSplitLen)
{
    const TStr SrcFPath = "./gixstress_shrink_src/";
    const TStr DestFPath = "./gixstress_shrink_dest/";
    FreshDir(SrcFPath); FreshDir(DestFPath);
    TGixDefItemHandler<TStressGixKey, TStressGixItem> ItemHandler;
    const TStressGixKey Key(7, 1);
    const int Items = 1200;
    {
        PStressGix Src = NewStressGix(SrcFPath, faCreate, ItemHandler, 500);
        for (int N = 0; N < Items; N++) { Src->AddItem(Key, TStressGixItem((uint)N)); }
        ASSERT_EQ((Items - 1) / 500, Src->GetItemSet(Key)->GetChildVectors());
    }
    TStressSplitLenProvider Provider;
    Provider.KeyIdSplitLenH.AddDat(7, 50);
    {
        PStressGix Src = NewStressGix(SrcFPath, faRdOnly, ItemHandler, 500);
        PStressGix Dest = NewStressGix(DestFPath, faCreate, ItemHandler, 500);
        Dest->SetSplitLenProvider(&Provider);
        Src->CopyTo(*Dest);
        EXPECT_TRUE(Src->IsKeyDataEqual(*Dest, Key));
        EXPECT_TRUE(Src->VerifySample(*Dest, 10));
        EXPECT_EQ(50, Dest->GetItemSet(Key)->GetSplitLen());
        EXPECT_EQ((Items - 1) / 50, Dest->GetItemSet(Key)->GetChildVectors());
    }
    // reload the destination and verify the re-chunked data
    {
        PStressGix Dest = NewStressGix(DestFPath, faRdOnly, ItemHandler, 500);
        Dest->SetSplitLenProvider(&Provider);
        TVec<TStressGixItem> ItemV; Dest->GetItemV(Key, ItemV);
        ASSERT_EQ(Items, ItemV.Len());
        for (int N = 0; N < Items; N++) { ASSERT_EQ(N, (int)ItemV[N].Val); }
    }
    TDir::DelNonEmptyDir(SrcFPath);
    TDir::DelNonEmptyDir(DestFPath);
}

// a key far beyond the split length (10000 items at splitLen 100 -> 99 child
// vectors) streams through many CopyTo handler invocations; both the child
// structure and every item must survive, alongside untouched small keys
TEST(QmDefragStressTests, CopyToKeyWithManyChildren)
{
    const TStr SrcFPath = "./gixstress_many_src/";
    const TStr DestFPath = "./gixstress_many_dest/";
    FreshDir(SrcFPath); FreshDir(DestFPath);
    TGixDefItemHandler<TStressGixKey, TStressGixItem> ItemHandler;
    const TStressGixKey BigKey(1, 1);
    const TStressGixKey SmallKey(2, 1);
    const int BigItems = 10000;
    const int SplitLen = 100;
    {
        PStressGix Src = NewStressGix(SrcFPath, faCreate, ItemHandler, SplitLen);
        for (int N = 0; N < BigItems; N++) { Src->AddItem(BigKey, TStressGixItem((uint)N)); }
        Src->AddItem(SmallKey, TStressGixItem(5u));
        ASSERT_EQ((BigItems - 1) / SplitLen, Src->GetItemSet(BigKey)->GetChildVectors());
    }
    {
        PStressGix Src = NewStressGix(SrcFPath, faRdOnly, ItemHandler, SplitLen);
        PStressGix Dest = NewStressGix(DestFPath, faCreate, ItemHandler, SplitLen);
        Src->CopyTo(*Dest);
        ASSERT_EQ(2, Dest->GetKeys());
        EXPECT_EQ((BigItems - 1) / SplitLen, Dest->GetItemSet(BigKey)->GetChildVectors());
        EXPECT_TRUE(Src->IsKeyDataEqual(*Dest, BigKey));
        EXPECT_TRUE(Src->IsKeyDataEqual(*Dest, SmallKey));
        EXPECT_TRUE(Src->VerifySample(*Dest, 10));
    }
    {
        PStressGix Dest = NewStressGix(DestFPath, faRdOnly, ItemHandler, SplitLen);
        TVec<TStressGixItem> ItemV; Dest->GetItemV(BigKey, ItemV);
        ASSERT_EQ(BigItems, ItemV.Len());
        for (int N = 0; N < BigItems; N++) { ASSERT_EQ(N, (int)ItemV[N].Val); }
    }
    TDir::DelNonEmptyDir(SrcFPath);
    TDir::DelNonEmptyDir(DestFPath);
}

// deletions leave shortened/unmerged itemsets behind; CopyTo must copy the
// merged (post-delete) view. a key whose items were ALL deleted must not be
// materialized in the destination at all.
TEST(QmDefragStressTests, CopyToAfterItemDeletions)
{
    const TStr SrcFPath = "./gixstress_del_src/";
    const TStr DestFPath = "./gixstress_del_dest/";
    FreshDir(SrcFPath); FreshDir(DestFPath);
    TGixDefItemHandler<TStressGixKey, TStressGixItem> ItemHandler;
    const TStressGixKey KeyA(1, 1);
    const TStressGixKey KeyGone(2, 1);
    const int Items = 1000;
    {
        PStressGix Src = NewStressGix(SrcFPath, faCreate, ItemHandler, 100);
        PStressGix Dest = NewStressGix(DestFPath, faCreate, ItemHandler, 100);
        for (int N = 0; N < Items; N++) { Src->AddItem(KeyA, TStressGixItem((uint)N)); }
        for (int N = 0; N < 10; N++) { Src->AddItem(KeyGone, TStressGixItem((uint)N)); }
        // delete the complete first child range and a block in the middle of KeyA
        for (int N = 0; N < 100; N++) { Src->DelItem(KeyA, TStressGixItem((uint)N)); }
        for (int N = 500; N < 550; N++) { Src->DelItem(KeyA, TStressGixItem((uint)N)); }
        // delete everything under KeyGone
        for (int N = 0; N < 10; N++) { Src->DelItem(KeyGone, TStressGixItem((uint)N)); }

        TVec<TStressGixItem> ExpItemV; Src->GetItemV(KeyA, ExpItemV);
        ASSERT_EQ(Items - 150, ExpItemV.Len());

        Src->CopyTo(*Dest);
        EXPECT_TRUE(Src->IsKeyDataEqual(*Dest, KeyA));
        // the emptied key must not appear in the destination
        EXPECT_FALSE(Dest->IsKey(KeyGone));

        TVec<TStressGixItem> GotItemV; Dest->GetItemV(KeyA, GotItemV);
        ASSERT_EQ(ExpItemV.Len(), GotItemV.Len());
        for (int N = 0; N < ExpItemV.Len(); N++) {
            ASSERT_EQ((int)ExpItemV[N].Val, (int)GotItemV[N].Val) << "mismatch at position " << N;
        }
        // none of the deleted values may resurface
        for (int N = 0; N < GotItemV.Len(); N++) {
            const int Val = (int)GotItemV[N].Val;
            ASSERT_TRUE(Val >= 100 && (Val < 500 || Val >= 550)) << "deleted item " << Val << " resurfaced";
        }
    }
    TDir::DelNonEmptyDir(SrcFPath);
    TDir::DelNonEmptyDir(DestFPath);
}

// VerifySample's whole purpose is catching a bad copy: corrupt the destination
// in two different ways (extra item appended, key with different item) and
// require a false result; the untouched key must still compare equal
TEST(QmDefragStressTests, VerifySampleDetectsCorruptedDestination)
{
    const TStr SrcFPath = "./gixstress_bad_src/";
    const TStr DestFPath = "./gixstress_bad_dest/";
    FreshDir(SrcFPath); FreshDir(DestFPath);
    TGixDefItemHandler<TStressGixKey, TStressGixItem> ItemHandler;
    const TStressGixKey KeyA(1, 1);
    const TStressGixKey KeyB(2, 1);
    {
        PStressGix Src = NewStressGix(SrcFPath, faCreate, ItemHandler, 100);
        PStressGix Dest = NewStressGix(DestFPath, faCreate, ItemHandler, 100);
        for (int N = 0; N < 250; N++) {
            Src->AddItem(KeyA, TStressGixItem((uint)N));
            Src->AddItem(KeyB, TStressGixItem((uint)N));
        }
        Src->CopyTo(*Dest);
        ASSERT_TRUE(Src->VerifySample(*Dest, 100)); // sanity: identical copy passes

        // corruption 1: extra item under KeyA changes the count
        Dest->AddItem(KeyA, TStressGixItem(100000u));
        EXPECT_FALSE(Src->IsKeyDataEqual(*Dest, KeyA));
        EXPECT_TRUE(Src->IsKeyDataEqual(*Dest, KeyB)); // untouched key still equal
        // sampling all keys (sample >= key count -> step 1) must flag the mismatch
        EXPECT_FALSE(Src->VerifySample(*Dest, 100));

        // corruption 2: same count but different content under KeyB
        Dest->DelItem(KeyB, TStressGixItem(17u));
        Dest->AddItem(KeyB, TStressGixItem(100001u));
        EXPECT_FALSE(Src->IsKeyDataEqual(*Dest, KeyB));
        EXPECT_FALSE(Src->VerifySample(*Dest, 100));
    }
    TDir::DelNonEmptyDir(SrcFPath);
    TDir::DelNonEmptyDir(DestFPath);
}

// store defrag with the maximum number of id gaps (every 2nd record deleted)
// and multi-100KB TOAST-ed bodies: after DefragTo + file swap + reload, the
// surviving records must keep their exact ids and byte-identical field values
// in both the in-memory part (Name, Value) and the disk/TOAST part (Body)
TEST(QmDefragStressTests, DefragToWithAlternatingDeletesAndLargeRecords)
{
    const TStr FPath = "./gixstress_store/";
    const TStr BuildFPath = "./gixstress_store_new/";
    const TStr BackupFPath = "./gixstress_store_old/";
    FreshDir(FPath); FreshDir(BuildFPath); FreshDir(BackupFPath);
    if (!TQm::TEnv::IsInit()) { TQm::TEnv::Init(); }

    const TStr SchemaStr =
        "[{ \"name\": \"StressItem\","
        "   \"fields\": [ { \"name\": \"Name\", \"type\": \"string\", \"primary\": true },"
        "                 { \"name\": \"Value\", \"type\": \"string\" },"
        "                 { \"name\": \"Body\", \"type\": \"string\", \"store\": \"cache\" } ],"
        "   \"keys\": [ { \"field\": \"Value\", \"type\": \"value\", \"storage\": \"tiny\" } ],"
        "   \"options\": { \"type\": \"paged\" }"
        "}]";
    PJsonVal SchemaVal = TJsonVal::GetValFromStr(SchemaStr);
    const int Recs = 60;

    THash<TUInt64, TStr> ExpNameH, ExpValueH, ExpBodyH;
    {
        PBase Base = TStorage::NewBase(FPath, SchemaVal, 10000000, 10000000, true);
        TWPt<TStore> Store = Base->GetStoreByStoreNm("StressItem");
        ASSERT_EQ(Store->GetStoreType(), "TStorePbBlob");
        for (int RecN = 0; RecN < Recs; RecN++) {
            // every 4th record gets a ~120KB body (spans multiple TOAST blocks
            // and pages), the others a small one
            TChA BodyChA = TStr::Fmt("body-%d-", RecN);
            const int BodyLen = (RecN % 4 == 0) ? 120000 : 100;
            while (BodyChA.Len() < BodyLen) { BodyChA += TStr::Fmt("%d;", RecN * 7 + BodyChA.Len()); }
            PJsonVal RecVal = TJsonVal::NewObj();
            RecVal->AddToObj("Name", TStr::Fmt("rec%d", RecN));
            RecVal->AddToObj("Value", TStr::Fmt("v%d", RecN % 5));
            RecVal->AddToObj("Body", TStr(BodyChA));
            Base->AddRec("StressItem", RecVal);
        }
        // delete every 2nd record: gaps between all surviving neighbors
        TUInt64V DelRecIdV;
        for (uint64 RecId = 1; RecId < (uint64)Recs; RecId += 2) { DelRecIdV.Add(RecId); }
        Store->DeleteRecs(DelRecIdV);
        ASSERT_EQ(Recs / 2, (int)Store->GetRecs());

        PStoreIter Iter = Store->GetIter();
        while (Iter->Next()) {
            const uint64 RecId = Iter->GetRecId();
            ExpNameH.AddDat(RecId, Store->GetFieldStr(RecId, Store->GetFieldId("Name")));
            ExpValueH.AddDat(RecId, Store->GetFieldStr(RecId, Store->GetFieldId("Value")));
            ExpBodyH.AddDat(RecId, Store->GetFieldStr(RecId, Store->GetFieldId("Body")));
        }
        ASSERT_EQ(Recs / 2, ExpNameH.Len());

        TStorePbBlob* PbStore = dynamic_cast<TStorePbBlob*>(Store());
        ASSERT_TRUE(PbStore != NULL);
        const uint64 CopiedRecs = PbStore->DefragTo(TPath::Combine(BuildFPath, "StressItem"), 10000000);
        EXPECT_EQ(Recs / 2, (int)CopiedRecs);
        TStorage::SaveBase(Base);
    }

    // swap the store files: old ones to backup, rebuilt ones into place
    {
        TStrV FNmV; TStr FNm;
        TFFile OldFFile(TPath::Combine(FPath, "StressItemPgBlob*"), false);
        while (OldFFile.Next(FNm)) { FNmV.Add(TDir::GetFileName(FNm)); }
        EXPECT_GE(FNmV.Len(), 5);
        for (int FNmN = 0; FNmN < FNmV.Len(); FNmN++) {
            TFile::Move(TPath::Combine(FPath, FNmV[FNmN]), TPath::Combine(BackupFPath, FNmV[FNmN]));
        }
        FNmV.Clr();
        TFFile NewFFile(TPath::Combine(BuildFPath, "StressItemPgBlob*"), false);
        while (NewFFile.Next(FNm)) { FNmV.Add(TDir::GetFileName(FNm)); }
        EXPECT_GE(FNmV.Len(), 5);
        for (int FNmN = 0; FNmN < FNmV.Len(); FNmN++) {
            TFile::Move(TPath::Combine(BuildFPath, FNmV[FNmN]), TPath::Combine(FPath, FNmV[FNmN]));
        }
    }

    // reload and verify ids, gaps and byte-identical content
    {
        PBase Base = TStorage::LoadBase(FPath, faRdOnly, 10000000, 10000000);
        TWPt<TStore> Store = Base->GetStoreByStoreNm("StressItem");
        ASSERT_EQ(Recs / 2, (int)Store->GetRecs());
        for (uint64 RecId = 1; RecId < (uint64)Recs; RecId += 2) {
            EXPECT_FALSE(Store->IsRecId(RecId)) << "deleted record " << RecId << " resurfaced";
        }
        for (int KeyId = ExpNameH.FFirstKeyId(); ExpNameH.FNextKeyId(KeyId); ) {
            const uint64 RecId = ExpNameH.GetKey(KeyId);
            ASSERT_TRUE(Store->IsRecId(RecId));
            EXPECT_EQ(ExpNameH[KeyId], Store->GetFieldStr(RecId, Store->GetFieldId("Name")));
            EXPECT_EQ(ExpValueH.GetDat(RecId), Store->GetFieldStr(RecId, Store->GetFieldId("Value")));
            const TStr GotBody = Store->GetFieldStr(RecId, Store->GetFieldId("Body"));
            const TStr& ExpBody = ExpBodyH.GetDat(RecId);
            ASSERT_EQ(ExpBody.Len(), GotBody.Len()) << "body length differs for record " << RecId;
            EXPECT_TRUE(ExpBody == GotBody) << "body content differs for record " << RecId;
            EXPECT_EQ(RecId, Store->GetRecId(ExpNameH[KeyId]));
        }
        // the index must still resolve against the defragmented store
        PRecSet RecSet = Base->Search("{ \"$from\": \"StressItem\", \"Value\": \"v0\" }");
        int ExpRecs = 0;
        for (int KeyId = ExpValueH.FFirstKeyId(); ExpValueH.FNextKeyId(KeyId); ) {
            if (ExpValueH[KeyId] == "v0") { ExpRecs++; }
        }
        EXPECT_EQ(ExpRecs, RecSet->GetRecs());
    }
    TDir::DelNonEmptyDir(FPath);
    TDir::DelNonEmptyDir(BuildFPath);
    TDir::DelNonEmptyDir(BackupFPath);
}
