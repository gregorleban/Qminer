/* Copyright (C) Event Registry d.o.o. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Written by Gregor Leban <gregor@eventregistry.org>, 2013-2017
 */

#include <qminer.h>
#include <qminer_storage.h>
#include "gtest/gtest.h"

using namespace TQm;
using namespace TQm::TStorage;

// tests for per-key split length overrides in TGix (TGixSplitLenProvider)

namespace {

typedef TIntUInt64Pr TTestGixKey;
typedef TUInt TTestGixItem;
typedef TPt<TGix<TTestGixKey, TTestGixItem> > PTestGix;
typedef TPt<TGixItemSet<TTestGixKey, TTestGixItem> > PTestGixItemSet;

// provider that returns the split length based on the first value in the key pair
class TTestSplitLenProvider : public TGixSplitLenProvider<TTestGixKey> {
public:
	THash<TInt, TInt> KeyIdSplitLenH;

	int GetSplitLen(const TTestGixKey& Key) const {
		TInt SplitLen;
		if (KeyIdSplitLenH.IsKeyGetDat(Key.Val1, SplitLen)) { return SplitLen; }
		return -1;
	}
};

PTestGix NewTestGix(const TStr& FPath, const TFAccess& Access,
	const TGixDefItemHandler<TTestGixKey, TTestGixItem>& ItemHandler, const TTestSplitLenProvider& Provider)
{
	// default split length 1000, tolerance [500, 2000]
	PTestGix Gix = TGix<TTestGixKey, TTestGixItem>::New("GixSplitLen", FPath, Access, &ItemHandler, 10000000, 1000, true, 500, 2000);
	Gix->SetSplitLenProvider(&Provider);
	return Gix;
}

} // namespace

TEST(GixSplitLenTests, PerKeySplitLen)
{
	const TStr FPath = "./gix_splitlen_test/";
	if (TDir::Exists(FPath)) { TDir::DelNonEmptyDir(FPath); }
	TDir::GenDir(FPath);

	TGixDefItemHandler<TTestGixKey, TTestGixItem> ItemHandler;
	TTestSplitLenProvider Provider;
	Provider.KeyIdSplitLenH.AddDat(7, 100);

	const TTestGixKey SmallSplitKey(7, 1);		// has a per-key override: split at 100
	const TTestGixKey DefaultSplitKey(8, 1);	// no override: split at the default 1000
	const int Items = 950;

	{
		PTestGix Gix = NewTestGix(FPath, faCreate, ItemHandler, Provider);

		// check split lengths are resolved per key
		EXPECT_EQ(Gix->GetSplitLen(SmallSplitKey), 100);
		EXPECT_EQ(Gix->GetSplitLenMin(SmallSplitKey), 50);
		EXPECT_EQ(Gix->GetSplitLenMax(SmallSplitKey), 200);
		EXPECT_EQ(Gix->GetSplitLen(DefaultSplitKey), 1000);
		EXPECT_EQ(Gix->GetSplitLenMin(DefaultSplitKey), 500);
		EXPECT_EQ(Gix->GetSplitLenMax(DefaultSplitKey), 2000);

		for (int N = 0; N < Items; N++) {
			Gix->AddItem(SmallSplitKey, TUInt(N));
			Gix->AddItem(DefaultSplitKey, TUInt(N));
		}

		// the key with the small split length must have pushed items into child vectors of length 100
		// (a child is created each time the work buffer is full when the next item is added)
		PTestGixItemSet SmallSplitItemSet = Gix->GetItemSet(SmallSplitKey);
		EXPECT_EQ(SmallSplitItemSet->GetSplitLen(), 100);
		EXPECT_EQ(SmallSplitItemSet->GetChildVectors(), (Items - 1) / 100);
		// the key with the default split length must still hold everything in the work buffer
		PTestGixItemSet DefaultSplitItemSet = Gix->GetItemSet(DefaultSplitKey);
		EXPECT_EQ(DefaultSplitItemSet->GetSplitLen(), 1000);
		EXPECT_EQ(DefaultSplitItemSet->GetChildVectors(), 0);

		// all items must be retrievable from both keys
		TVec<TTestGixItem> ItemV; Gix->GetItemV(SmallSplitKey, ItemV);
		ASSERT_EQ(ItemV.Len(), Items);
		for (int N = 0; N < Items; N++) { EXPECT_EQ((int) ItemV[N].Val, N); }
		TVec<TTestGixItem> DefaultItemV; Gix->GetItemV(DefaultSplitKey, DefaultItemV);
		ASSERT_EQ(DefaultItemV.Len(), Items);
	}

	// reopen the gix and check that children and items were persisted correctly
	{
		PTestGix Gix = NewTestGix(FPath, faRdOnly, ItemHandler, Provider);

		PTestGixItemSet SmallSplitItemSet = Gix->GetItemSet(SmallSplitKey);
		EXPECT_EQ(SmallSplitItemSet->GetSplitLen(), 100);
		EXPECT_EQ(SmallSplitItemSet->GetChildVectors(), (Items - 1) / 100);

		TVec<TTestGixItem> ItemV; Gix->GetItemV(SmallSplitKey, ItemV);
		ASSERT_EQ(ItemV.Len(), Items);
		for (int N = 0; N < Items; N++) { EXPECT_EQ((int) ItemV[N].Val, N); }
		TVec<TTestGixItem> DefaultItemV; Gix->GetItemV(DefaultSplitKey, DefaultItemV);
		ASSERT_EQ(DefaultItemV.Len(), Items);
	}

	TDir::DelNonEmptyDir(FPath);
}

TEST(GixSplitLenTests, SchemaSplitLenApplied)
{
	const TStr FPath = "./gix_splitlen_base/";
	if (TDir::Exists(FPath)) { TDir::DelNonEmptyDir(FPath); }
	TDir::GenDir(FPath);

	const TStr SchemaStr =
		"[{ \"name\": \"TestItem\","
		"   \"fields\": [ { \"name\": \"Name\", \"type\": \"string\", \"primary\": true },"
		"                 { \"name\": \"Value\", \"type\": \"string\" } ],"
		"   \"joins\": [ { \"name\": \"hasRelated\", \"type\": \"index\", \"store\": \"TestItem\", \"storage\": \"tiny\", \"splitLen\": 300 } ],"
		"   \"keys\": [ { \"field\": \"Value\", \"type\": \"value\", \"storage\": \"tiny\", \"splitLen\": 200 } ]"
		"}]";
	PJsonVal SchemaVal = TJsonVal::GetValFromStr(SchemaStr);

	if (!TQm::TEnv::IsInit()) { TQm::TEnv::Init(); }

	{
		// creating the base applies the splitLen attributes from the schema - it must
		// correctly resolve names of both field index keys and index join keys
		PBase Base = TStorage::NewBase(FPath, SchemaVal, 10000000, 10000000, true);
		// re-applying manually must also work (this is what happens when an existing base is loaded)
		TStorage::ApplyIndexKeySplitLen(Base, SchemaVal);
		// unknown store or key names must be rejected
		EXPECT_THROW(Base->PutIndexKeySplitLen("TestItem", "NoSuchKey", 100), PExcept);
		EXPECT_THROW(Base->PutIndexKeySplitLen("NoSuchStore", "Value", 100), PExcept);
		TStorage::SaveBase(Base);
	}

	TDir::DelNonEmptyDir(FPath);
}

TEST(GixSplitLenTests, DefaultWhenNoProvider)
{
	const TStr FPath = "./gix_splitlen_test2/";
	if (TDir::Exists(FPath)) { TDir::DelNonEmptyDir(FPath); }
	TDir::GenDir(FPath);

	TGixDefItemHandler<TTestGixKey, TTestGixItem> ItemHandler;
	const TTestGixKey Key(7, 1);

	{
		// no provider set - everything should use the gix defaults
		PTestGix Gix = TGix<TTestGixKey, TTestGixItem>::New("GixSplitLen", FPath, faCreate, &ItemHandler, 10000000, 1000, true, 500, 2000);
		EXPECT_EQ(Gix->GetSplitLen(Key), 1000);
		EXPECT_EQ(Gix->GetSplitLenMin(Key), 500);
		EXPECT_EQ(Gix->GetSplitLenMax(Key), 2000);

		for (int N = 0; N < 2500; N++) {
			Gix->AddItem(Key, TUInt(N));
		}
		PTestGixItemSet ItemSet = Gix->GetItemSet(Key);
		EXPECT_EQ(ItemSet->GetChildVectors(), 2);
		TVec<TTestGixItem> ItemV; Gix->GetItemV(Key, ItemV);
		ASSERT_EQ(ItemV.Len(), 2500);
	}

	TDir::DelNonEmptyDir(FPath);
}
