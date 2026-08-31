/* Copyright (C) Event Registry d.o.o. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 */

// Tests for the local child rebalancing in TGixItemSet::Def() (gix.h/gix.hpp):
//   - SplitOversizedChildren: a key whose on-disk children were chunked at a LARGER
//     split length than the current one (the production case: per-key splitLen shrunk
//     between runs) is cut into in-tolerance pieces locally on the first delete,
//     instead of re-merging the whole posting list
//   - the relaxed Def() shortcut: the local path now runs with leftover adds in the
//     work buffer (the old guard demanded ItemV.Empty(), never true on a live key),
//     and CoalesceUndersizedChildren glues delete-fragmented neighbours locally -
//     observable because the glued child keeps its irregular length, while a global
//     merge would rebuild every child at exactly SplitLen
//   - PushWorkBufferToChildren: the linear drain (one running offset, ONE trailing
//     TVec::Del) must chunk a work buffer holding several SplitLen multiples into
//     children of exactly SplitLen with the remainder left in the buffer
//   - HasOverlappingChildren: out-of-order children (which no code path is expected
//     to produce) must be detected and sent through the global-merge safety net,
//     never through the local fixes that assume disjoint sorted children
//
// Plus the flat-serialization opt-in of TQmGixItemPos (qminer_core.h, 2026-08-17):
// its bulk-saved bytes must stay identical to the historical element-by-element
// format, or every existing pos index on disk becomes unreadable.

#define XTEST 1

#include <qminer.h>
#include "gtest/gtest.h"

using namespace TQm;

namespace {

typedef TIntUInt64Pr TRebalKey;
typedef TUInt TRebalItem;
typedef TGixItemSet<TRebalKey, TRebalItem> TRebalItemSet;
typedef TPt<TRebalItemSet> PRebalItemSet;
typedef TPt<TGix<TRebalKey, TRebalItem> > PRebalGix;

// provider that returns the split length based on the first value in the key pair
class TRebalSplitLenProvider : public TGixSplitLenProvider<TRebalKey> {
public:
    THash<TInt, TInt> KeyIdSplitLenH;

    int GetSplitLen(const TRebalKey& Key) const {
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

// default split length 1000, tolerance [500, 2000], first child may be unfilled
PRebalGix NewRebalGix(const TStr& FPath, const TFAccess& Access,
    const TGixDefItemHandler<TRebalKey, TRebalItem>& ItemHandler)
{
    return TGix<TRebalKey, TRebalItem>::New("GixRebal", FPath, Access,
        &ItemHandler, 10000000, 1000, true, 500, 2000);
}

// the items of Key must be exactly ExpectV, in strictly ascending order
void CheckItems(const PRebalGix& Gix, const TRebalKey& Key, const TVec<TRebalItem>& ExpectV)
{
    TVec<TRebalItem> ItemV; Gix->GetItemV(Key, ItemV);
    ASSERT_EQ(ExpectV.Len(), ItemV.Len());
    for (int N = 0; N < ItemV.Len(); N++) {
        ASSERT_EQ((uint)ExpectV[N].Val, (uint)ItemV[N].Val) << "mismatch at position " << N;
        if (N > 0) { ASSERT_LT((uint)ItemV[N - 1].Val, (uint)ItemV[N].Val) << "not ascending at " << N; }
    }
}

// [From, To] inclusive
void AddRange(TVec<TRebalItem>& ItemV, const int& From, const int& To)
{
    for (int N = From; N <= To; N++) { ItemV.Add(TRebalItem((uint)N)); }
}

} // namespace

// gix.h/qminer_storage.h expose their internals to a class named ::XTest under
// #ifdef XTEST. The historical XTest suite in test_tgix.cpp is entirely commented
// out, so the name is free for the white-box helpers these tests need.
class XTest {
public:
    // swap two children (metadata and loaded data together) - produces the
    // out-of-order state that HasOverlappingChildren guards against and that no
    // public code path can create
    static void SwapChildren(const PRebalItemSet& ItemSet, const int& ChildN1, const int& ChildN2) {
        ItemSet->ChildInfoV.Swap(ChildN1, ChildN2);
        ItemSet->ChildV.Swap(ChildN1, ChildN2);
        ItemSet->MergedP = false;
        ItemSet->DirtyP = true;
    }
    static bool HasOverlappingChildren(const PRebalItemSet& ItemSet) {
        return ItemSet->HasOverlappingChildren();
    }
    // append pre-sorted items directly to the work buffer, past the SplitLen cap that
    // the public AddItem path enforces - the only way to hand PushWorkBufferToChildren
    // a buffer holding several chunks at once
    static void AppendToWorkBuffer(const PRebalItemSet& ItemSet, const TVec<TRebalItem>& NewV) {
        ItemSet->ItemV.AddV(NewV);
        ItemSet->TotalCnt += NewV.Len();
        ItemSet->DirtyP = true;
    }
    static void PushWorkBufferToChildren(const PRebalItemSet& ItemSet) {
        ItemSet->PushWorkBufferToChildren();
        ItemSet->RecalcTotalCnt();
    }
    static uint GetChildMinItem(const PRebalItemSet& ItemSet, const int& ChildN) {
        return (uint)ItemSet->ChildInfoV[ChildN].MinItem.Val;
    }
    static uint GetChildMaxItem(const PRebalItemSet& ItemSet, const int& ChildN) {
        return (uint)ItemSet->ChildInfoV[ChildN].MaxItem.Val;
    }
};

// A key written with a large per-key split length (children of 2000) and reopened
// with a small one (200, tolerance [100, 400]) has every child oversized. The first
// delete must fix the layout LOCALLY: every child cut into in-tolerance pieces, all
// items preserved, and the new layout must survive a store/reopen round-trip. This is
// the exact production scenario the 2026-08-31 rebalancing commit was written for.
TEST(GixRebalanceTests, SplitOversizedChildrenOnSmallerSplitLen)
{
    const TStr FPath = "./gix_rebal_oversized/";
    FreshDir(FPath);
    TGixDefItemHandler<TRebalKey, TRebalItem> ItemHandler;
    const TRebalKey Key(1, 1);
    const int Items = 10000;

    // write with per-key split length 2000 -> children of length 2000
    {
        TRebalSplitLenProvider Provider;
        Provider.KeyIdSplitLenH.AddDat(1, 2000);
        PRebalGix Gix = NewRebalGix(FPath, faCreate, ItemHandler);
        Gix->SetSplitLenProvider(&Provider);
        for (int N = 0; N < Items; N++) { Gix->AddItem(Key, TRebalItem((uint)N)); }
        PRebalItemSet ItemSet = Gix->GetItemSet(Key);
        ASSERT_EQ(4, ItemSet->GetChildInfoCount());
        for (int ChildN = 0; ChildN < 4; ChildN++) { ASSERT_EQ(2000, ItemSet->GetChildInfoLen(ChildN)); }
        ASSERT_EQ(2000, ItemSet->GetWorkBufLen());
    }

    // reopen with per-key split length 200 and delete one item: Def must split every
    // oversized child in place (10 pieces each) instead of re-merging the whole list
    {
        TRebalSplitLenProvider Provider;
        Provider.KeyIdSplitLenH.AddDat(1, 200);
        PRebalGix Gix = NewRebalGix(FPath, faUpdate, ItemHandler);
        Gix->SetSplitLenProvider(&Provider);
        Gix->DelItem(Key, TRebalItem(5u));

        TVec<TRebalItem> ExpectV;
        AddRange(ExpectV, 0, 4); AddRange(ExpectV, 6, Items - 1);
        CheckItems(Gix, Key, ExpectV);

        // the persisted work buffer (2000 leftover adds) was drained into 10 children of
        // exactly 200 by the delete's own flush; the 4 on-disk children of 2000 were then
        // each cut into 10 pieces of 199-200 - nothing may be over- or undersized
        PRebalItemSet ItemSet = Gix->GetItemSet(Key);
        EXPECT_EQ(50, ItemSet->GetChildInfoCount());
        EXPECT_EQ(0, ItemSet->GetWorkBufLen());
        for (int ChildN = 0; ChildN < ItemSet->GetChildInfoCount(); ChildN++) {
            EXPECT_LE(ItemSet->GetChildInfoLen(ChildN), 400) << "oversized child " << ChildN;
            EXPECT_GE(ItemSet->GetChildInfoLen(ChildN), 100) << "undersized child " << ChildN;
        }
    }

    // the split layout must have been persisted correctly (the first piece of each cut
    // child reuses the old blob pointer, later pieces get fresh ones)
    {
        TRebalSplitLenProvider Provider;
        Provider.KeyIdSplitLenH.AddDat(1, 200);
        PRebalGix Gix = NewRebalGix(FPath, faRdOnly, ItemHandler);
        Gix->SetSplitLenProvider(&Provider);
        TVec<TRebalItem> ExpectV;
        AddRange(ExpectV, 0, 4); AddRange(ExpectV, 6, Items - 1);
        CheckItems(Gix, Key, ExpectV);
        PRebalItemSet ItemSet = Gix->GetItemSet(Key);
        EXPECT_EQ(50, ItemSet->GetChildInfoCount());
    }
    TDir::DelNonEmptyDir(FPath);
}

// The deletes-only local path must run with leftover ADDS still sitting in the work
// buffer (the old shortcut demanded an empty buffer, which never holds on a live key):
// after fragmenting deletes, empty children are dropped, adjacent undersized ones are
// glued, and the buffer leftovers stay untouched. The glued child keeps its irregular
// length (20) - the proof the local path ran, since a global merge would have rebuilt
// every child at exactly SplitLen (100).
TEST(GixRebalanceTests, LocalDefWithNonEmptyWorkBufferCoalesces)
{
    const TStr FPath = "./gix_rebal_coalesce/";
    FreshDir(FPath);
    TGixDefItemHandler<TRebalKey, TRebalItem> ItemHandler;
    const TRebalKey Key(2, 1);

    TRebalSplitLenProvider Provider;
    Provider.KeyIdSplitLenH.AddDat(2, 100); // tolerance [50, 200]
    {
        PRebalGix Gix = NewRebalGix(FPath, faCreate, ItemHandler);
        Gix->SetSplitLenProvider(&Provider);
        // 10 children of 100 (0..999) + 50 leftover adds in the buffer (1000..1049)
        for (int N = 0; N < 1050; N++) { Gix->AddItem(Key, TRebalItem((uint)N)); }
        PRebalItemSet ItemSet = Gix->GetItemSet(Key);
        ASSERT_EQ(10, ItemSet->GetChildInfoCount());
        ASSERT_EQ(50, ItemSet->GetWorkBufLen());

        // empty out child 0 completely and fragment children 1 and 2 down to 10 items each
        TVec<TRebalItem> DelV;
        AddRange(DelV, 0, 99); AddRange(DelV, 100, 189); AddRange(DelV, 200, 289);
        Gix->DelItemV(Key, DelV);

        TVec<TRebalItem> ExpectV;
        AddRange(ExpectV, 190, 199); AddRange(ExpectV, 290, 299);
        AddRange(ExpectV, 300, 999); AddRange(ExpectV, 1000, 1049);
        CheckItems(Gix, Key, ExpectV); // triggers the Def

        // empty child dropped, the two 10-item fragments glued into ONE child of 20,
        // full children untouched, buffer leftovers preserved
        EXPECT_EQ(8, ItemSet->GetChildInfoCount());
        EXPECT_EQ(20, ItemSet->GetChildInfoLen(0)) << "local coalesce did not run (a global merge would rebuild at SplitLen)";
        for (int ChildN = 1; ChildN < 8; ChildN++) { EXPECT_EQ(100, ItemSet->GetChildInfoLen(ChildN)); }
        EXPECT_EQ(50, ItemSet->GetWorkBufLen());
    }

    // the coalesced layout must survive a store/reopen round-trip
    {
        PRebalGix Gix = NewRebalGix(FPath, faRdOnly, ItemHandler);
        TVec<TRebalItem> ExpectV;
        AddRange(ExpectV, 190, 199); AddRange(ExpectV, 290, 299);
        AddRange(ExpectV, 300, 999); AddRange(ExpectV, 1000, 1049);
        CheckItems(Gix, Key, ExpectV);
        PRebalItemSet ItemSet = Gix->GetItemSet(Key);
        EXPECT_EQ(8, ItemSet->GetChildInfoCount());
        EXPECT_EQ(20, ItemSet->GetChildInfoLen(0));
    }
    TDir::DelNonEmptyDir(FPath);
}

// PushWorkBufferToChildren drains the buffer through a running offset with ONE
// trailing TVec::Del - the chunk boundaries must come out exact for buffers holding
// zero, one, and several SplitLen multiples (the multi-chunk case is what
// PushMergedDataBackToChildren hands it after a global merge)
TEST(GixRebalanceTests, WorkBufferDrainChunksExactly)
{
    const TStr FPath = "./gix_rebal_drain/";
    FreshDir(FPath);
    TGixDefItemHandler<TRebalKey, TRebalItem> ItemHandler;
    const int SplitLen = 100;
    const int TotalV[] = { 99, 100, 200, 537, 1000 };

    TRebalSplitLenProvider Provider;
    PRebalGix Gix = NewRebalGix(FPath, faCreate, ItemHandler);
    for (int CaseN = 0; CaseN < 5; CaseN++) { Provider.KeyIdSplitLenH.AddDat(10 + CaseN, SplitLen); }
    Gix->SetSplitLenProvider(&Provider);

    for (int CaseN = 0; CaseN < 5; CaseN++) {
        const int Total = TotalV[CaseN];
        const TRebalKey Key(10 + CaseN, 1);
        // create the itemset with one public add, then stuff the buffer past SplitLen
        Gix->AddItem(Key, TRebalItem(0u));
        PRebalItemSet ItemSet = Gix->GetItemSet(Key);
        TVec<TRebalItem> FillV; AddRange(FillV, 1, Total - 1);
        XTest::AppendToWorkBuffer(ItemSet, FillV);

        XTest::PushWorkBufferToChildren(ItemSet);

        const int ExpChildren = Total / SplitLen;
        ASSERT_EQ(ExpChildren, ItemSet->GetChildInfoCount()) << "buffer of " << Total;
        ASSERT_EQ(Total % SplitLen, ItemSet->GetWorkBufLen()) << "buffer of " << Total;
        for (int ChildN = 0; ChildN < ExpChildren; ChildN++) {
            ASSERT_EQ(SplitLen, ItemSet->GetChildInfoLen(ChildN));
            ASSERT_EQ(uint(ChildN * SplitLen), XTest::GetChildMinItem(ItemSet, ChildN));
            ASSERT_EQ(uint(ChildN * SplitLen + SplitLen - 1), XTest::GetChildMaxItem(ItemSet, ChildN));
        }
        // every item readable and in order across children + remaining buffer
        TVec<TRebalItem> ExpectV; AddRange(ExpectV, 0, Total - 1);
        CheckItems(Gix, Key, ExpectV);
    }
    Gix.Clr();
    TDir::DelNonEmptyDir(FPath);
}

// Out-of-order children (produced here by swapping two children through the friend
// hook - no public path can create them) must be detected by HasOverlappingChildren
// and repaired by the global-merge safety net, NOT "fixed" by the local path whose
// assumptions they violate. After the repair the posting list is sorted, in-tolerance
// and persists correctly.
TEST(GixRebalanceTests, OverlappingChildrenTakeGlobalMergeSafetyNet)
{
    const TStr FPath = "./gix_rebal_overlap/";
    FreshDir(FPath);
    TGixDefItemHandler<TRebalKey, TRebalItem> ItemHandler;
    const TRebalKey Key(3, 1);

    TRebalSplitLenProvider Provider;
    Provider.KeyIdSplitLenH.AddDat(3, 200); // tolerance [100, 400]
    {
        PRebalGix Gix = NewRebalGix(FPath, faCreate, ItemHandler);
        Gix->SetSplitLenProvider(&Provider);
        // 5 children of 200 (0..999) + 50 adds in the buffer (1000..1049)
        for (int N = 0; N < 1050; N++) { Gix->AddItem(Key, TRebalItem((uint)N)); }
        PRebalItemSet ItemSet = Gix->GetItemSet(Key);
        ASSERT_EQ(5, ItemSet->GetChildInfoCount());

        // make child 1 undersized (90 items) and dirty, so the next Def has a merge trigger
        TVec<TRebalItem> DelV; AddRange(DelV, 200, 309);
        Gix->DelItemV(Key, DelV);
        TVec<TRebalItem> AfterDelV;
        AddRange(AfterDelV, 0, 199); AddRange(AfterDelV, 310, 1049);
        CheckItems(Gix, Key, AfterDelV); // Def: local path leaves the 90-item child in place
        ASSERT_EQ(90, ItemSet->GetChildInfoLen(1));
        ASSERT_FALSE(XTest::HasOverlappingChildren(ItemSet));

        // corrupt the order: swap children 2 [400-599] and 3 [600-799]
        XTest::SwapChildren(ItemSet, 2, 3);
        ASSERT_TRUE(XTest::HasOverlappingChildren(ItemSet));

        // the next Def must fall through to the global merge and restore a sorted,
        // disjoint, in-tolerance layout with every item intact
        CheckItems(Gix, Key, AfterDelV);
        EXPECT_FALSE(XTest::HasOverlappingChildren(ItemSet));
        EXPECT_EQ(4, ItemSet->GetChildInfoCount());
        for (int ChildN = 0; ChildN < 4; ChildN++) {
            EXPECT_EQ(200, ItemSet->GetChildInfoLen(ChildN)) << "child " << ChildN;
        }
        EXPECT_EQ(140, ItemSet->GetWorkBufLen());
    }

    // repaired layout survives a store/reopen round-trip
    {
        PRebalGix Gix = NewRebalGix(FPath, faRdOnly, ItemHandler);
        TVec<TRebalItem> ExpectV;
        AddRange(ExpectV, 0, 199); AddRange(ExpectV, 310, 1049);
        CheckItems(Gix, Key, ExpectV);
        PRebalItemSet ItemSet = Gix->GetItemSet(Key);
        EXPECT_EQ(4, ItemSet->GetChildInfoCount());
    }
    TDir::DelNonEmptyDir(FPath);
}

// TQmGixItemPos opted into TVec's bulk flat serialization (2026-08-17). TVec's stream
// format carries no tag, so the bulk bytes MUST be identical to the historical
// element-by-element format - otherwise every pos index written before the change
// becomes unreadable. Pin the trait, the size, and the byte-for-byte format.
TEST(GixItemPosSerTests, FlatOptInAndByteCompatibleFormat)
{
    EXPECT_EQ(8u, sizeof(TIndex::TQmGixItemPos));
    EXPECT_EQ(1, (int)TIsFlatSerializable<TIndex::TQmGixItemPos>::Val);
    EXPECT_EQ(1, (int)TIsBitwiseMovable<TIndex::TQmGixItemPos>::Val);

    // vector with the full variety: 0-3 positions per item, empty items included
    TVec<TIndex::TQmGixItemPos> PosV;
    for (int RecId = 0; RecId < 5000; RecId++) {
        TIndex::TQmGixItemPos Item((uint64)RecId);
        const int PosCnt = RecId % 4;
        int Pos = (RecId % 900) + 1; // positions must be in [1, Modulo] and ascending
        for (int PosN = 0; PosN < PosCnt; PosN++) {
            Item.Add(Pos);
            Pos += 1 + (RecId % 7);
        }
        PosV.Add(Item);
    }

    // bulk save (the flat path) must produce the exact bytes of the historical
    // element-by-element format
    TMOut BulkOut; PosV.Save(BulkOut);
    TMOut RefOut;
    RefOut.Save(PosV.Reserved());
    RefOut.Save(PosV.Len());
    for (int N = 0; N < PosV.Len(); N++) { PosV[N].Save(RefOut); }
    ASSERT_EQ(RefOut.Len(), BulkOut.Len());
    EXPECT_EQ(0, memcmp(BulkOut.GetBfAddr(), RefOut.GetBfAddr(), BulkOut.Len()));

    // bytes written in the OLD format must load back identical through the new path
    TMIn MIn(RefOut.GetBfAddr(), RefOut.Len(), false);
    TVec<TIndex::TQmGixItemPos> LoadedV; LoadedV.Load(MIn);
    ASSERT_EQ(PosV.Len(), LoadedV.Len());
    for (int N = 0; N < PosV.Len(); N++) {
        ASSERT_EQ(PosV[N].GetRecId(), LoadedV[N].GetRecId()) << "RecId mismatch at " << N;
        ASSERT_EQ(PosV[N].GetPosLen(), LoadedV[N].GetPosLen()) << "PosLen mismatch at " << N;
        for (int PosN = 0; PosN < PosV[N].GetPosLen(); PosN++) {
            ASSERT_EQ(PosV[N].GetPos(PosN), LoadedV[N].GetPos(PosN))
                << "position " << PosN << " mismatch at " << N;
        }
    }
}
