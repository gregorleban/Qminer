/**
 * Edge-case tests for the per-key split-length overrides in TGix
 * (TGixSplitLenProvider, gix.h/gix.hpp) and TIndex::PutKeySplitLen.
 *
 * GixSplitLenTests covers the happy path (one override + default key, schema
 * application); here we pin down:
 *   - providers returning 0 / negative values fall back to the gix default
 *   - overrides are a runtime-only parameter: reopening the gix WITHOUT the
 *     provider resolves the default again, while previously written child
 *     vectors keep their on-disk chunking
 *   - several keys with different overrides coexisting in one gix
 *   - TIndex::PutKeySplitLen (via TBase) rejecting non-positive values
 */

#include <qminer.h>
#include <qminer_storage.h>
#include "gtest/gtest.h"

using namespace TQm;
using namespace TQm::TStorage;

namespace {

typedef TIntUInt64Pr TSplitEdgeKey;
typedef TUInt TSplitEdgeItem;
typedef TPt<TGix<TSplitEdgeKey, TSplitEdgeItem> > PSplitEdgeGix;
typedef TPt<TGixItemSet<TSplitEdgeKey, TSplitEdgeItem> > PSplitEdgeItemSet;

// provider that returns the split length based on the first value in the key pair;
// values <= 0 stored in the hash are returned as-is, so we can also test that the
// gix treats them as "use the default"
class TSplitEdgeProvider : public TGixSplitLenProvider<TSplitEdgeKey> {
public:
    THash<TInt, TInt> KeyIdSplitLenH;

    int GetSplitLen(const TSplitEdgeKey& Key) const {
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

// default split length 1000, tolerance [500, 2000] - same as GixSplitLenTests
PSplitEdgeGix NewEdgeGix(const TStr& FPath, const TFAccess& Access,
    const TGixDefItemHandler<TSplitEdgeKey, TSplitEdgeItem>& ItemHandler)
{
    return TGix<TSplitEdgeKey, TSplitEdgeItem>::New("GixSplitEdge", FPath, Access,
        &ItemHandler, 10000000, 1000, true, 500, 2000);
}

void CheckItems(const PSplitEdgeGix& Gix, const TSplitEdgeKey& Key, const int& ExpItems)
{
    TVec<TSplitEdgeItem> ItemV; Gix->GetItemV(Key, ItemV);
    ASSERT_EQ(ExpItems, ItemV.Len());
    for (int N = 0; N < ExpItems; N++) { ASSERT_EQ(N, (int)ItemV[N].Val); }
}

} // namespace

// a provider returning 0 or a negative value for a key must NOT override the
// default (the gix only honors values > 0), so no degenerate zero-length
// chunking can be configured by accident
TEST(GixSplitLenEdgeTests, ProviderZeroOrNegativeFallsBackToDefault)
{
    const TStr FPath = "./gix_splitedge_nonpos/";
    FreshDir(FPath);
    TGixDefItemHandler<TSplitEdgeKey, TSplitEdgeItem> ItemHandler;
    TSplitEdgeProvider Provider;
    Provider.KeyIdSplitLenH.AddDat(5, 0);   // zero -> default
    Provider.KeyIdSplitLenH.AddDat(6, -50); // negative -> default

    const TSplitEdgeKey ZeroKey(5, 1);
    const TSplitEdgeKey NegKey(6, 1);
    {
        PSplitEdgeGix Gix = NewEdgeGix(FPath, faCreate, ItemHandler);
        Gix->SetSplitLenProvider(&Provider);
        EXPECT_EQ(1000, Gix->GetSplitLen(ZeroKey));
        EXPECT_EQ(500, Gix->GetSplitLenMin(ZeroKey));
        EXPECT_EQ(2000, Gix->GetSplitLenMax(ZeroKey));
        EXPECT_EQ(1000, Gix->GetSplitLen(NegKey));
        EXPECT_EQ(500, Gix->GetSplitLenMin(NegKey));
        EXPECT_EQ(2000, Gix->GetSplitLenMax(NegKey));

        // chunking must follow the default, not the bogus provider value
        const int Items = 1500;
        for (int N = 0; N < Items; N++) { Gix->AddItem(ZeroKey, TUInt(N)); }
        PSplitEdgeItemSet ItemSet = Gix->GetItemSet(ZeroKey);
        EXPECT_EQ(1000, ItemSet->GetSplitLen());
        EXPECT_EQ((Items - 1) / 1000, ItemSet->GetChildVectors());
        CheckItems(Gix, ZeroKey, Items);
    }
    TDir::DelNonEmptyDir(FPath);
}

// split lengths are a runtime parameter: they are NOT stored with the gix.
// reopening the same gix without a provider must resolve the default length for
// the itemset, while the child vectors written earlier keep their (smaller)
// persisted chunking and all data stays readable
TEST(GixSplitLenEdgeTests, OverridesAreRuntimeOnlyAndNotPersisted)
{
    const TStr FPath = "./gix_splitedge_runtime/";
    FreshDir(FPath);
    TGixDefItemHandler<TSplitEdgeKey, TSplitEdgeItem> ItemHandler;
    const TSplitEdgeKey Key(7, 1);
    const int Items = 500;
    const int OverrideLen = 50;

    // write with a per-key override of 50 -> children of length 50
    {
        TSplitEdgeProvider Provider;
        Provider.KeyIdSplitLenH.AddDat(7, OverrideLen);
        PSplitEdgeGix Gix = NewEdgeGix(FPath, faCreate, ItemHandler);
        Gix->SetSplitLenProvider(&Provider);
        for (int N = 0; N < Items; N++) { Gix->AddItem(Key, TUInt(N)); }
        PSplitEdgeItemSet ItemSet = Gix->GetItemSet(Key);
        EXPECT_EQ(OverrideLen, ItemSet->GetSplitLen());
        EXPECT_EQ((Items - 1) / OverrideLen, ItemSet->GetChildVectors());
    }

    // reopen WITHOUT a provider: the itemset must resolve the gix default again
    // (proving nothing about the override was persisted), the previously written
    // children keep their chunking and the data is intact
    {
        PSplitEdgeGix Gix = NewEdgeGix(FPath, faRdOnly, ItemHandler);
        EXPECT_EQ(1000, Gix->GetSplitLen(Key));
        PSplitEdgeItemSet ItemSet = Gix->GetItemSet(Key);
        EXPECT_EQ(1000, ItemSet->GetSplitLen()) << "split-len override was unexpectedly persisted";
        EXPECT_EQ((Items - 1) / OverrideLen, ItemSet->GetChildVectors());
        CheckItems(Gix, Key, Items);
    }

    // reopen for update without a provider and add more items: the work buffer
    // now fills to the default length, so no new child is created before 1000
    {
        PSplitEdgeGix Gix = NewEdgeGix(FPath, faUpdate, ItemHandler);
        const int OldChildren = (Items - 1) / OverrideLen;
        for (int N = Items; N < Items + 400; N++) { Gix->AddItem(Key, TUInt(N)); }
        PSplitEdgeItemSet ItemSet = Gix->GetItemSet(Key);
        EXPECT_EQ(OldChildren, ItemSet->GetChildVectors())
            << "new children were chunked at the stale override length";
        CheckItems(Gix, Key, Items + 400);
    }
    TDir::DelNonEmptyDir(FPath);
}

// several keys with different overrides plus a default key must chunk
// independently within the same gix, both before and after a reload
TEST(GixSplitLenEdgeTests, MultipleKeysWithDifferentLensCoexist)
{
    const TStr FPath = "./gix_splitedge_multi/";
    FreshDir(FPath);
    TGixDefItemHandler<TSplitEdgeKey, TSplitEdgeItem> ItemHandler;
    TSplitEdgeProvider Provider;
    Provider.KeyIdSplitLenH.AddDat(1, 50);
    Provider.KeyIdSplitLenH.AddDat(2, 200);

    const TSplitEdgeKey Key50(1, 1);
    const TSplitEdgeKey Key200(2, 1);
    const TSplitEdgeKey KeyDef(3, 1);
    const int Items = 600;

    {
        PSplitEdgeGix Gix = NewEdgeGix(FPath, faCreate, ItemHandler);
        Gix->SetSplitLenProvider(&Provider);
        for (int N = 0; N < Items; N++) {
            Gix->AddItem(Key50, TUInt(N));
            Gix->AddItem(Key200, TUInt(N));
            Gix->AddItem(KeyDef, TUInt(N));
        }
        EXPECT_EQ((Items - 1) / 50, Gix->GetItemSet(Key50)->GetChildVectors());
        EXPECT_EQ((Items - 1) / 200, Gix->GetItemSet(Key200)->GetChildVectors());
        EXPECT_EQ(0, Gix->GetItemSet(KeyDef)->GetChildVectors()); // 600 < default 1000
        CheckItems(Gix, Key50, Items);
        CheckItems(Gix, Key200, Items);
        CheckItems(Gix, KeyDef, Items);
    }
    // reload with the same provider: chunking and data must be intact
    {
        PSplitEdgeGix Gix = NewEdgeGix(FPath, faRdOnly, ItemHandler);
        Gix->SetSplitLenProvider(&Provider);
        EXPECT_EQ((Items - 1) / 50, Gix->GetItemSet(Key50)->GetChildVectors());
        EXPECT_EQ((Items - 1) / 200, Gix->GetItemSet(Key200)->GetChildVectors());
        EXPECT_EQ(0, Gix->GetItemSet(KeyDef)->GetChildVectors());
        CheckItems(Gix, Key50, Items);
        CheckItems(Gix, Key200, Items);
        CheckItems(Gix, KeyDef, Items);
    }
    TDir::DelNonEmptyDir(FPath);
}

// TIndex::PutKeySplitLen (reached through TBase::PutIndexKeySplitLen) must
// reject zero and negative split lengths - a zero length would make the
// work-buffer push loop in TGixItemSet spin forever
TEST(GixSplitLenEdgeTests, PutKeySplitLenRejectsNonPositiveValues)
{
    const TStr FPath = "./gix_splitedge_reject/";
    FreshDir(FPath);
    const TStr SchemaStr =
        "[{ \"name\": \"EdgeStore\","
        "   \"fields\": [ { \"name\": \"Name\", \"type\": \"string\", \"primary\": true },"
        "                 { \"name\": \"Value\", \"type\": \"string\" } ],"
        "   \"keys\": [ { \"field\": \"Value\", \"type\": \"value\", \"storage\": \"tiny\" } ]"
        "}]";
    PJsonVal SchemaVal = TJsonVal::GetValFromStr(SchemaStr);
    if (!TQm::TEnv::IsInit()) { TQm::TEnv::Init(); }
    {
        PBase Base = TStorage::NewBase(FPath, SchemaVal, 10000000, 10000000, true);
        EXPECT_THROW(Base->PutIndexKeySplitLen("EdgeStore", "Value", 0), PExcept);
        EXPECT_THROW(Base->PutIndexKeySplitLen("EdgeStore", "Value", -1), PExcept);
        EXPECT_THROW(Base->PutIndexKeySplitLen("EdgeStore", "Value", -100000), PExcept);
        // a valid value must still be accepted afterwards
        Base->PutIndexKeySplitLen("EdgeStore", "Value", 100);
        TStorage::SaveBase(Base);
    }
    TDir::DelNonEmptyDir(FPath);
}
