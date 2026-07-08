/**
 * Edge-case / bug-discovery tests for index and store defragmentation on branch
 * fast-load-save: TGix::CopyTo (gix.h/.hpp) and TStorePbBlob::DefragTo
 * (qminer_storage.cpp). These complement GixDefragTests, which covers the main
 * fragmented-copy / swap-and-reload happy paths, by pinning down the boundary
 * cases those tests do not hit: an empty index, item counts exactly on the
 * split-length boundary, a store with nothing deleted, and a store with every
 * record deleted.
 */

#include <qminer.h>
#include <qminer_storage.h>
#include "gtest/gtest.h"

using namespace TQm;
using namespace TQm::TStorage;

namespace {

typedef TIntUInt64Pr TEdgeGixKey;
typedef TUInt TEdgeGixItem;
typedef TPt<TGix<TEdgeGixKey, TEdgeGixItem> > PEdgeGix;

PEdgeGix NewGix(const TStr& FPath, const TFAccess& FAccess,
    TGixDefItemHandler<TEdgeGixKey, TEdgeGixItem>& ItemHandler, const int& SplitLen)
{
    // same shape as GixDefragTests: cache, splitLen, mostly-tiny, min/max child len
    return TGix<TEdgeGixKey, TEdgeGixItem>::New("GixEdge", FPath, FAccess, &ItemHandler,
        10000000, SplitLen, true, SplitLen / 2, SplitLen * 2);
}

void FreshDir(const TStr& FPath) {
    if (TDir::Exists(FPath)) { TDir::DelNonEmptyDir(FPath); }
    TDir::GenDir(FPath);
}

} // namespace

// copying an index that has no keys at all must produce a valid, empty
// destination rather than crashing or writing a corrupt blob
TEST(QmDefragEdgeTests, CopyToEmptyGixProducesValidEmptyDest)
{
    const TStr SrcFPath = "./gixedge_empty_src/";
    const TStr DestFPath = "./gixedge_empty_dest/";
    FreshDir(SrcFPath);
    FreshDir(DestFPath);
    TGixDefItemHandler<TEdgeGixKey, TEdgeGixItem> ItemHandler;
    {
        PEdgeGix Src = NewGix(SrcFPath, faCreate, ItemHandler, 100);
        PEdgeGix Dest = NewGix(DestFPath, faCreate, ItemHandler, 100);
        ASSERT_EQ(0, Src->GetKeys());
        Src->CopyTo(*Dest);
        EXPECT_EQ(0, Dest->GetKeys());
    }
    // the empty destination must reopen cleanly and still report no keys
    {
        PEdgeGix Dest = NewGix(DestFPath, faRdOnly, ItemHandler, 100);
        EXPECT_EQ(0, Dest->GetKeys());
    }
    TDir::DelNonEmptyDir(SrcFPath);
    TDir::DelNonEmptyDir(DestFPath);
}

// item counts exactly on and around the split-length boundary are where an
// off-by-one in the child-vector splitting would surface; verify both the data
// and the resulting child-vector count for counts N-1, N, N+1, 2N
TEST(QmDefragEdgeTests, CopyToAtSplitLengthBoundariesKeepsDataAndSplitCount)
{
    const TStr SrcFPath = "./gixedge_bnd_src/";
    const TStr DestFPath = "./gixedge_bnd_dest/";
    const int SplitLen = 100;
    const int Counts[] = { 1, SplitLen - 1, SplitLen, SplitLen + 1, 2 * SplitLen, 2 * SplitLen + 1 };

    for (int CntN = 0; CntN < (int)(sizeof(Counts) / sizeof(int)); CntN++) {
        const int Count = Counts[CntN];
        FreshDir(SrcFPath);
        FreshDir(DestFPath);
        TGixDefItemHandler<TEdgeGixKey, TEdgeGixItem> ItemHandler;
        const TEdgeGixKey Key(1, 1);
        {
            PEdgeGix Src = NewGix(SrcFPath, faCreate, ItemHandler, SplitLen);
            for (int N = 0; N < Count; N++) { Src->AddItem(Key, TUInt(N)); }
        }
        {
            PEdgeGix Src = NewGix(SrcFPath, faRdOnly, ItemHandler, SplitLen);
            PEdgeGix Dest = NewGix(DestFPath, faCreate, ItemHandler, SplitLen);
            Src->CopyTo(*Dest);
            EXPECT_TRUE(Src->IsKeyDataEqual(*Dest, Key))
                << "data differs after copy at count " << Count;
            // a full itemset of C items with split length L holds (C-1)/L child
            // vectors (the last, still-growing vector is kept in the parent)
            EXPECT_EQ((Count - 1) / SplitLen, Dest->GetItemSet(Key)->GetChildVectors())
                << "wrong child-vector count at item count " << Count;
        }
        {
            PEdgeGix Dest = NewGix(DestFPath, faRdOnly, ItemHandler, SplitLen);
            TVec<TEdgeGixItem> ItemV; Dest->GetItemV(Key, ItemV);
            ASSERT_EQ(Count, ItemV.Len()) << "wrong item count reloaded at count " << Count;
            for (int N = 0; N < Count; N++) { EXPECT_EQ(N, (int)ItemV[N].Val); }
        }
    }
    TDir::DelNonEmptyDir(SrcFPath);
    TDir::DelNonEmptyDir(DestFPath);
}

namespace {

const TStr StoreDefragSchema =
    "[{ \"name\": \"EdgeItem\","
    "   \"fields\": [ { \"name\": \"Name\", \"type\": \"string\", \"primary\": true },"
    "                 { \"name\": \"Value\", \"type\": \"string\" } ],"
    "   \"keys\": [ { \"field\": \"Value\", \"type\": \"value\", \"storage\": \"tiny\" } ],"
    "   \"options\": { \"type\": \"paged\" }"
    "}]";

// create a paged store with Recs records, delete the record ids in DelRecIdV,
// DefragTo a rebuilt copy, swap the files in, reload read-only and verify that
// exactly the surviving records remain with their original ids and content.
// ExpDefragReturn is the value DefragTo is expected to return.
void RunStoreDefragScenario(const TStr& Tag, const int& Recs, const TUInt64V& DelRecIdV) {
    const TStr FPath = "./storedge_" + Tag + "/";
    const TStr BuildFPath = "./storedge_" + Tag + "_new/";
    const TStr BackupFPath = "./storedge_" + Tag + "_old/";
    FreshDir(FPath); FreshDir(BuildFPath); FreshDir(BackupFPath);
    if (!TQm::TEnv::IsInit()) { TQm::TEnv::Init(); }
    PJsonVal SchemaVal = TJsonVal::GetValFromStr(StoreDefragSchema);

    THash<TUInt64, TStr> ExpNameH, ExpValueH;
    {
        PBase Base = TStorage::NewBase(FPath, SchemaVal, 10000000, 10000000, true);
        TWPt<TStore> Store = Base->GetStoreByStoreNm("EdgeItem");
        for (int RecN = 0; RecN < Recs; RecN++) {
            PJsonVal RecVal = TJsonVal::NewObj();
            RecVal->AddToObj("Name", TStr::Fmt("rec%d", RecN));
            RecVal->AddToObj("Value", TStr::Fmt("v%d", RecN % 5));
            Base->AddRec("EdgeItem", RecVal);
        }
        if (DelRecIdV.Len() > 0) {
            TUInt64V DelV(DelRecIdV);
            Store->DeleteRecs(DelV);
        }
        const int Surviving = Recs - DelRecIdV.Len();
        ASSERT_EQ(Surviving, (int)Store->GetRecs());

        PStoreIter Iter = Store->GetIter();
        while (Iter->Next()) {
            const uint64 RecId = Iter->GetRecId();
            ExpNameH.AddDat(RecId, Store->GetFieldStr(RecId, Store->GetFieldId("Name")));
            ExpValueH.AddDat(RecId, Store->GetFieldStr(RecId, Store->GetFieldId("Value")));
        }

        TStorePbBlob* PbStore = dynamic_cast<TStorePbBlob*>(Store());
        ASSERT_TRUE(PbStore != NULL);
        const uint64 CopiedRecs = PbStore->DefragTo(TPath::Combine(BuildFPath, "EdgeItem"), 10000000);
        EXPECT_EQ(Surviving, (int)CopiedRecs) << "DefragTo returned wrong count for " << Tag.CStr();
        TStorage::SaveBase(Base);
    }

    // swap: old store files out to backup, rebuilt ones into place
    {
        TStrV FNmV; TStr FNm;
        TFFile OldFFile(TPath::Combine(FPath, "EdgeItemPgBlob*"), false);
        while (OldFFile.Next(FNm)) { FNmV.Add(TDir::GetFileName(FNm)); }
        for (int i = 0; i < FNmV.Len(); i++) {
            TFile::Move(TPath::Combine(FPath, FNmV[i]), TPath::Combine(BackupFPath, FNmV[i]));
        }
        FNmV.Clr();
        TFFile NewFFile(TPath::Combine(BuildFPath, "EdgeItemPgBlob*"), false);
        while (NewFFile.Next(FNm)) { FNmV.Add(TDir::GetFileName(FNm)); }
        for (int i = 0; i < FNmV.Len(); i++) {
            TFile::Move(TPath::Combine(BuildFPath, FNmV[i]), TPath::Combine(FPath, FNmV[i]));
        }
    }

    // reload and verify surviving records only, with ids and content intact
    {
        PBase Base = TStorage::LoadBase(FPath, faRdOnly, 10000000, 10000000);
        TWPt<TStore> Store = Base->GetStoreByStoreNm("EdgeItem");
        ASSERT_EQ(Recs - DelRecIdV.Len(), (int)Store->GetRecs());
        for (int i = 0; i < DelRecIdV.Len(); i++) {
            EXPECT_FALSE(Store->IsRecId(DelRecIdV[i])) << "deleted id resurfaced in " << Tag.CStr();
        }
        for (int KeyId = ExpNameH.FFirstKeyId(); ExpNameH.FNextKeyId(KeyId); ) {
            const uint64 RecId = ExpNameH.GetKey(KeyId);
            ASSERT_TRUE(Store->IsRecId(RecId));
            EXPECT_EQ(Store->GetFieldStr(RecId, Store->GetFieldId("Name")), ExpNameH[KeyId]);
            EXPECT_EQ(Store->GetFieldStr(RecId, Store->GetFieldId("Value")), ExpValueH.GetDat(RecId));
            EXPECT_EQ(Store->GetRecId(ExpNameH[KeyId]), RecId);
        }
    }
    TDir::DelNonEmptyDir(FPath);
    TDir::DelNonEmptyDir(BuildFPath);
    TDir::DelNonEmptyDir(BackupFPath);
}

} // namespace

// defragmenting a store that has no deleted records must copy every record and
// keep every id/content unchanged (the "nothing to compact" identity case)
TEST(QmDefragEdgeTests, DefragToWithNoDeletesPreservesEverything)
{
    RunStoreDefragScenario("nodel", 60, TUInt64V());
}

// defragmenting a store whose records were all deleted must return zero and
// reload as an empty store without resurrecting anything
TEST(QmDefragEdgeTests, DefragToWithAllRecordsDeletedYieldsEmptyStore)
{
    const int Recs = 40;
    TUInt64V DelRecIdV;
    for (uint64 RecId = 0; RecId < (uint64)Recs; RecId++) { DelRecIdV.Add(RecId); }
    RunStoreDefragScenario("alldel", Recs, DelRecIdV);
}

// deleting the very first and very last records puts the id gaps at the extreme
// ends of the id space, which the mid-range deletes in GixDefragTests do not
TEST(QmDefragEdgeTests, DefragToWithDeletesAtIdSpaceExtremes)
{
    const int Recs = 50;
    TUInt64V DelRecIdV;
    DelRecIdV.Add(0);
    DelRecIdV.Add((uint64)(Recs - 1));
    RunStoreDefragScenario("extremes", Recs, DelRecIdV);
}
