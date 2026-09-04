/* Copyright (C) Event Registry d.o.o. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 */

// White-box tests for TGixItemSet::DeleteMarkerP (gix.h / gix.hpp).
//
// Def() and DefLocal() finish with a GLOBAL handler merge over the work-buffer tail
// that scrubs lone delete markers (qminer's partial deletes are AddItem(RecId, -Fq);
// see test_gix_negfq.cpp for the ghost-posting bug this prevents). That pass used to
// run on every Def(), i.e. on every work-buffer flush of every key while indexing,
// which showed up as a slowdown of article adding on production. It now runs only
// while DeleteMarkerP is set - the flag is armed by AddItem() when the handler's
// IsDeleteMarker() says so, and cleared by the pass itself.
//
// The handler below is the summing handler with the production semantics (local merge
// keeps negatives for a partner that may sit in a child, global merge drops Dat <= 0)
// plus counters, so the tests can assert exactly when the scrub pass ran. Gix calls the
// global merge from two more places: InjectWorkBufferToChildren merges every child it
// injected into (its input is that child with the injected items appended, so it is
// never both sorted and duplicate-free), and Def()'s safety net for overlapping children
// (never reached here). The scrub pass is the only global merge whose input is the
// freshly local-merged work buffer - sorted with unique keys - which is how the handler
// tells it apart (TailPasses vs ChildMerges).
#include <qminer.h>
#include "gtest/gtest.h"

namespace {

typedef TIntUInt64Pr TDmKey;
typedef TKeyDat<TUInt64, TInt> TDmItem; // [RecId, Fq]
typedef TPt<TGix<TDmKey, TDmItem> > PDmGix;
typedef TPt<TGixItemSet<TDmKey, TDmItem> > PDmItemSet;

class TCountingSumHandler : public TGixItemHandler<TDmKey, TDmItem> {
public:
    mutable int LocalMerges;
    mutable int TailPasses;  // global merges of the work-buffer tail (the scrub pass)
    mutable int ChildMerges; // global merges of a child after an inject

    TCountingSumHandler() : LocalMerges(0), TailPasses(0), ChildMerges(0) {}

    void ResetCounts() const { LocalMerges = 0; TailPasses = 0; ChildMerges = 0; }

    static bool IsSortedUnique(const TVec<TDmItem>& ItemV) {
        for (int N = 1; N < ItemV.Len(); N++) {
            if (!(ItemV[N - 1].Key < ItemV[N].Key)) { return false; }
        }
        return true;
    }

    void Merge(TVec<TDmItem>& ItemV, const bool& IsLocal) const {
        if (IsLocal) { LocalMerges++; }
        else if (IsSortedUnique(ItemV)) { TailPasses++; }
        else { ChildMerges++; }
        if (ItemV.Empty()) { return; }
        if (!ItemV.IsSorted()) { ItemV.Sort(); }
        int LastN = 0;
        for (int ItemN = 1; ItemN < ItemV.Len(); ItemN++) {
            if (ItemV[ItemN].Key != ItemV[LastN].Key) { ItemV[++LastN] = ItemV[ItemN]; }
            else { ItemV[LastN].Dat += ItemV[ItemN].Dat; }
        }
        int OutN = 0;
        for (int ItemN = 0; ItemN <= LastN; ItemN++) {
            const TDmItem& Item = ItemV[ItemN];
            if (Item.Dat.Val > 0 || (IsLocal && Item.Dat.Val < 0)) { ItemV[OutN++] = Item; }
        }
        ItemV.Trunc(OutN);
    }
    void Delete(const TDmItem& Item, TVec<TDmItem>& MainV) const { MainV.DelAll(Item); }
    bool IsLt(const TDmItem& Item1, const TDmItem& Item2) const { return Item1 < Item2; }
    bool IsLtE(const TDmItem& Item1, const TDmItem& Item2) const { return Item1 <= Item2; }
    bool IsDeleteMarker(const TDmItem& Item) const { return Item.Dat.Val <= 0; }
    uint64 GetMemUsed() const { return sizeof(TCountingSumHandler); }
};

// the same handler that does not declare markers (what the pos and default handlers do)
class TNoMarkerSumHandler : public TCountingSumHandler {
public:
    bool IsDeleteMarker(const TDmItem& Item) const { return false; }
};

const TDmKey DmKey(1, 0);

void DmFreshDir(const TStr& FPath) {
    if (TDir::Exists(FPath)) { TDir::DelNonEmptyDir(FPath); }
    TDir::GenDir(FPath);
}

// split length 8, tolerance [4, 16] - a few hundred items make several children
PDmGix NewDmGix(const TStr& FPath, const TFAccess& Access, const TCountingSumHandler& Handler) {
    return TGix<TDmKey, TDmItem>::New("GixDm", FPath, Access, &Handler, 10000000, 8, true, 4, 16);
}

TDmItem Posting(const int& RecId, const int& Fq) { return TDmItem(TUInt64((uint64)RecId), TInt(Fq)); }

// [From, To] inclusive, each RecId added Times times in a row - the repeats leave the
// set unmerged (an equal item is not "greater than the last one") so Def() has real work
void AddRange(const PDmGix& Gix, const int& From, const int& To, const int& Times = 1) {
    for (int RecId = From; RecId <= To; RecId++) {
        for (int TimeN = 0; TimeN < Times; TimeN++) { Gix->AddItem(DmKey, Posting(RecId, 1)); }
    }
}

bool FindFq(const TVec<TDmItem>& ItemV, const int& RecId, int& Fq) {
    for (int N = 0; N < ItemV.Len(); N++) {
        if (ItemV[N].Key.Val == (uint64)RecId) { Fq = ItemV[N].Dat.Val; return true; }
    }
    return false;
}

int CountNonPositive(const TVec<TDmItem>& ItemV) {
    int Cnt = 0;
    for (int N = 0; N < ItemV.Len(); N++) { if (ItemV[N].Dat <= 0) { Cnt++; } }
    return Cnt;
}

// every RecId in [From, To] present with Fq, ascending, nothing else and nothing non-positive
void ExpectRange(const TVec<TDmItem>& ItemV, const int& From, const int& To, const int& Fq) {
    ASSERT_EQ(To - From + 1, ItemV.Len());
    for (int N = 0; N < ItemV.Len(); N++) {
        EXPECT_EQ((uint64)(From + N), (uint64)ItemV[N].Key) << "at position " << N;
        EXPECT_EQ(Fq, (int)ItemV[N].Dat) << "at position " << N;
    }
    EXPECT_EQ(0, CountNonPositive(ItemV));
}

} // namespace

// the indexing path only ever adds: the scrub pass must not run at all, while the
// local merge still does its job (repeats summed up)
TEST(GixDeleteMarkerTests, AddOnlyPathNeverRunsScrubPass)
{
    const TStr FPath = "./gix_dm_addonly/";
    DmFreshDir(FPath);
    TCountingSumHandler Handler;
    {
        PDmGix Gix = NewDmGix(FPath, faCreate, Handler);
        AddRange(Gix, 1, 300, 2); // many flushes through Def() + PushWorkBufferToChildren
        TVec<TDmItem> ItemV; Gix->GetItemV(DmKey, ItemV);
        ExpectRange(ItemV, 1, 300, 2);
        EXPECT_GT(Handler.LocalMerges, 0);
        EXPECT_EQ(0, Handler.TailPasses);
        // (a repeat that straddles a flush is injected into the child it belongs to and
        // that child is merged - normal, and not the pass under test)
        EXPECT_GT(Handler.ChildMerges, 0);
        EXPECT_GT(Gix->GetItemSet(DmKey)->GetChildVectors(), 1);
    }
    TDir::DelNonEmptyDir(FPath);
}

// a lone marker past everything stored arms the flag: exactly one global pass on the
// next Def(), the marker is gone, and the flag is disarmed again - further adds do not
// pay for the pass, while a later marker arms it again
TEST(GixDeleteMarkerTests, LoneMarkerRunsOnePassThenDisarms)
{
    const TStr FPath = "./gix_dm_lone/";
    DmFreshDir(FPath);
    TCountingSumHandler Handler;
    {
        PDmGix Gix = NewDmGix(FPath, faCreate, Handler);
        AddRange(Gix, 1, 100);
        TVec<TDmItem> ItemV; Gix->GetItemV(DmKey, ItemV);
        ExpectRange(ItemV, 1, 100, 1);
        Handler.ResetCounts();

        // the delete of a posting that was never indexed
        Gix->AddItem(DmKey, Posting(1000, -1));
        EXPECT_FALSE(Gix->GetItemSet(DmKey)->IsMerged());
        Gix->GetItemV(DmKey, ItemV);
        ExpectRange(ItemV, 1, 100, 1);
        EXPECT_EQ(1, Handler.TailPasses);

        // the pass disarmed the flag: add-only traffic afterwards must not re-run it
        AddRange(Gix, 101, 300, 2);
        Gix->GetItemV(DmKey, ItemV);
        ASSERT_EQ(300, ItemV.Len());
        EXPECT_EQ(0, CountNonPositive(ItemV));
        EXPECT_EQ(1, Handler.TailPasses);

        // the record the ghost was for now legitimately gets the posting: found once, Fq 1
        Gix->AddItem(DmKey, Posting(1000, 1));
        Gix->GetItemV(DmKey, ItemV);
        int Fq = 0;
        ASSERT_TRUE(FindFq(ItemV, 1000, Fq));
        EXPECT_EQ(1, Fq);
        EXPECT_EQ(1, Handler.TailPasses);

        // a second lone marker re-arms the flag
        Gix->AddItem(DmKey, Posting(2000, -1));
        Gix->GetItemV(DmKey, ItemV);
        EXPECT_FALSE(FindFq(ItemV, 2000, Fq));
        EXPECT_EQ(0, CountNonPositive(ItemV));
        EXPECT_EQ(2, Handler.TailPasses);
    }
    TDir::DelNonEmptyDir(FPath);
}

// two markers added before any Def(): one pass covers both
TEST(GixDeleteMarkerTests, SeveralMarkersBeforeDefShareOnePass)
{
    const TStr FPath = "./gix_dm_several/";
    DmFreshDir(FPath);
    TCountingSumHandler Handler;
    {
        PDmGix Gix = NewDmGix(FPath, faCreate, Handler);
        AddRange(Gix, 1, 50);
        TVec<TDmItem> ItemV; Gix->GetItemV(DmKey, ItemV);
        Handler.ResetCounts();
        Gix->AddItem(DmKey, Posting(1000, -1));
        Gix->AddItem(DmKey, Posting(1001, -2));
        Gix->AddItem(DmKey, Posting(1002, 1));
        Gix->GetItemV(DmKey, ItemV);
        ASSERT_EQ(51, ItemV.Len());
        EXPECT_EQ(0, CountNonPositive(ItemV));
        int Fq = 0;
        EXPECT_TRUE(FindFq(ItemV, 1002, Fq));
        EXPECT_EQ(1, Handler.TailPasses);
    }
    TDir::DelNonEmptyDir(FPath);
}

// a marker whose positive is still in the work buffer cancels it there; one whose
// positive sits in a child cancels it through the inject - in both cases nothing
// non-positive survives and a re-add brings the posting back exactly once
TEST(GixDeleteMarkerTests, MarkerCancelsPartnerInBufferAndInChild)
{
    const TStr FPath = "./gix_dm_cancel/";
    DmFreshDir(FPath);
    TCountingSumHandler Handler;
    {
        PDmGix Gix = NewDmGix(FPath, faCreate, Handler);
        AddRange(Gix, 1, 100);
        TVec<TDmItem> ItemV; Gix->GetItemV(DmKey, ItemV);

        // partner in a child (RecId 50 lies well inside the children)
        Gix->AddItem(DmKey, Posting(50, -1));
        Gix->GetItemV(DmKey, ItemV);
        ASSERT_EQ(99, ItemV.Len());
        int Fq = 0;
        EXPECT_FALSE(FindFq(ItemV, 50, Fq));
        EXPECT_EQ(0, CountNonPositive(ItemV));
        Gix->AddItem(DmKey, Posting(50, 1));
        Gix->GetItemV(DmKey, ItemV);
        ExpectRange(ItemV, 1, 100, 1);

        // partner in the work buffer: add and delete within one flush
        Gix->AddItem(DmKey, Posting(500, 1));
        Gix->AddItem(DmKey, Posting(500, -1));
        Gix->AddItem(DmKey, Posting(501, 1));
        Gix->AddItem(DmKey, Posting(501, -1));
        Gix->AddItem(DmKey, Posting(501, 1));
        Gix->GetItemV(DmKey, ItemV);
        ASSERT_EQ(101, ItemV.Len());
        EXPECT_FALSE(FindFq(ItemV, 500, Fq));
        ASSERT_TRUE(FindFq(ItemV, 501, Fq));
        EXPECT_EQ(1, Fq);
        EXPECT_EQ(0, CountNonPositive(ItemV));
    }
    TDir::DelNonEmptyDir(FPath);
}

// the cache-cleanup path (DefLocal): a tail marker is scrubbed there with one pass and
// the set becomes merged; a marker whose partner may sit in a child is left for Def(),
// and the flag must survive that DefLocal so Def() still scrubs it
TEST(GixDeleteMarkerTests, DefLocalScrubsTailMarkerAndKeepsFlagOtherwise)
{
    const TStr FPath = "./gix_dm_deflocal/";
    DmFreshDir(FPath);
    TCountingSumHandler Handler;
    {
        PDmGix Gix = NewDmGix(FPath, faCreate, Handler);
        AddRange(Gix, 1, 100);
        TVec<TDmItem> ItemV; Gix->GetItemV(DmKey, ItemV);
        PDmItemSet ItemSet = Gix->GetItemSet(DmKey);
        Handler.ResetCounts();

        // tail marker: DefLocal alone finishes the job
        Gix->AddItem(DmKey, Posting(1000, -1));
        EXPECT_FALSE(ItemSet->IsMerged());
        ItemSet->DefLocal();
        EXPECT_TRUE(ItemSet->IsMerged());
        EXPECT_EQ(1, Handler.TailPasses);
        ItemSet->GetItemV(ItemV);
        ExpectRange(ItemV, 1, 100, 1);

        // add-only traffic that leaves the set unmerged: DefLocal must not run the pass
        Gix->AddItem(DmKey, Posting(1001, 1));
        Gix->AddItem(DmKey, Posting(1001, 1));
        EXPECT_FALSE(ItemSet->IsMerged());
        ItemSet->DefLocal();
        EXPECT_TRUE(ItemSet->IsMerged());
        EXPECT_EQ(1, Handler.TailPasses);
        ItemSet->GetItemV(ItemV);
        int Fq = 0;
        ASSERT_TRUE(FindFq(ItemV, 1001, Fq));
        EXPECT_EQ(2, Fq);

        // marker inside the children's range: DefLocal cannot decide, Def() must
        Gix->AddItem(DmKey, Posting(50, -1));
        ItemSet->DefLocal();
        EXPECT_FALSE(ItemSet->IsMerged());
        ItemSet->Def();
        EXPECT_TRUE(ItemSet->IsMerged());
        ItemSet->GetItemV(ItemV);
        EXPECT_FALSE(FindFq(ItemV, 50, Fq));
        EXPECT_EQ(0, CountNonPositive(ItemV));
        ASSERT_EQ(100, ItemV.Len());
    }
    TDir::DelNonEmptyDir(FPath);
}

// an itemset loaded from disk starts disarmed; a marker added to it must still arm the
// flag and be scrubbed, and the scrubbed state is what gets persisted
TEST(GixDeleteMarkerTests, MarkerOnReopenedItemSetIsScrubbed)
{
    const TStr FPath = "./gix_dm_reopen/";
    DmFreshDir(FPath);
    TCountingSumHandler Handler;
    {
        PDmGix Gix = NewDmGix(FPath, faCreate, Handler);
        AddRange(Gix, 1, 100);
    }
    {
        PDmGix Gix = NewDmGix(FPath, faUpdate, Handler);
        TVec<TDmItem> ItemV; Gix->GetItemV(DmKey, ItemV);
        ExpectRange(ItemV, 1, 100, 1);
        Handler.ResetCounts();
        Gix->AddItem(DmKey, Posting(1000, -1));
        Gix->GetItemV(DmKey, ItemV);
        ExpectRange(ItemV, 1, 100, 1);
        EXPECT_EQ(1, Handler.TailPasses);
    }
    {
        PDmGix Gix = NewDmGix(FPath, faRdOnly, Handler);
        TVec<TDmItem> ItemV; Gix->GetItemV(DmKey, ItemV);
        ExpectRange(ItemV, 1, 100, 1);
    }
    TDir::DelNonEmptyDir(FPath);
}

// the switch is the handler's IsDeleteMarker: a handler that declares no markers (pos,
// default) never pays for the pass, and gix does no scrubbing of its own for it
TEST(GixDeleteMarkerTests, HandlerWithoutMarkersNeverRunsScrubPass)
{
    const TStr FPath = "./gix_dm_nomarker/";
    DmFreshDir(FPath);
    TNoMarkerSumHandler Handler;
    {
        PDmGix Gix = NewDmGix(FPath, faCreate, Handler);
        AddRange(Gix, 1, 100, 2);
        Gix->AddItem(DmKey, Posting(1000, -1));
        TVec<TDmItem> ItemV; Gix->GetItemV(DmKey, ItemV);
        EXPECT_EQ(0, Handler.TailPasses);
        EXPECT_GT(Handler.LocalMerges, 0);
        ASSERT_EQ(101, ItemV.Len());
        // the negative survives: with no marker declared, the local merge's "keep
        // negatives for a partner in a child" rule is the last word
        int Fq = 0;
        ASSERT_TRUE(FindFq(ItemV, 1000, Fq));
        EXPECT_EQ(-1, Fq);
    }
    TDir::DelNonEmptyDir(FPath);
}
