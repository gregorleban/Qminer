/* Copyright (C) Event Registry d.o.o. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Written by Gregor Leban <gregor@eventregistry.org>, 2013-2017
 */

#include <qminer.h>
#include <qminer_storage.h>
#include "gtest/gtest.h"

// tests for the dirty-flag save skipping on shutdown:
// - TGix skips rewriting the key hash (.Gix file) when no key changed
// - TIndexVoc skips rewriting IndexVoc.dat when no key/word was added
// - TStoreImpl/TStorePbBlob skip rewriting their metadata files when unchanged
// each test also verifies that actual changes are still saved and reloadable

namespace {

typedef TIntUInt64Pr TTestGixKey;
typedef TUInt TTestGixItem;
typedef TPt<TGix<TTestGixKey, TTestGixItem> > PTestGix;

void ResetDir(const TStr& FPath)
{
	if (TDir::Exists(FPath)) { TDir::DelNonEmptyDir(FPath); }
	TDir::GenDir(FPath);
}

// file write times have limited resolution; make sure consecutive writes would
// get a different timestamp
void SleepForTmResolution() { TSysProc::Sleep(100); }

void EnsureQmEnv()
{
	if (!TQm::TEnv::IsInit()) { TQm::TEnv::Init(); }
}

PJsonVal GetTestStoreSchema(const bool& PagedP)
{
	TStr SchemaStr =
		"[{ \"name\": \"TestStore\", "
		"   \"fields\": [ "
		"     { \"name\": \"Name\", \"type\": \"string\", \"primary\": true }, "
		"     { \"name\": \"Value\", \"type\": \"int\" } ], "
		"   \"keys\": [ { \"field\": \"Name\", \"type\": \"value\" } ]";
	if (PagedP) { SchemaStr += ", \"options\": { \"type\": \"paged\" }"; }
	SchemaStr += "}]";
	return TJsonVal::GetValFromStr(SchemaStr);
}

void AddTestRec(const TWPt<TQm::TStore>& Store, const int& RecN)
{
	PJsonVal RecVal = TJsonVal::NewObj();
	RecVal->AddToObj("Name", TStr::Fmt("rec-%d", RecN));
	RecVal->AddToObj("Value", RecN);
	Store->AddRec(RecVal);
}

} // namespace

TEST(QmDirtySkipTests, GixSkipsKeyHashSaveWhenClean)
{
	const TStr FPath = "./qm_dirtyskip_gix/";
	ResetDir(FPath);
	TGixDefItemHandler<TTestGixKey, TTestGixItem> ItemHandler;
	const TStr GixFNm = FPath + "DirtySkip.Gix";

	// create and populate the gix
	{
		PTestGix Gix = TGix<TTestGixKey, TTestGixItem>::New("DirtySkip", FPath, faCreate, &ItemHandler, 10000000);
		for (int KeyN = 0; KeyN < 100; KeyN++) {
			for (int ItemN = 0; ItemN < 50; ItemN++) {
				Gix->AddItem(TTestGixKey(KeyN, 0), TTestGixItem(uint(ItemN)));
			}
		}
	}
	ASSERT_TRUE(TFile::Exists(GixFNm));
	const uint64 CreatedTm = TFile::GetLastWriteTm(GixFNm);
	SleepForTmResolution();

	// open in update mode, only read - the key hash must not be rewritten
	{
		PTestGix Gix = TGix<TTestGixKey, TTestGixItem>::New("DirtySkip", FPath, faUpdate, &ItemHandler, 10000000);
		EXPECT_FALSE(Gix->IsKeyIdHDirty());
		TVec<TTestGixItem> ItemV;
		Gix->GetItemV(TTestGixKey(3, 0), ItemV);
		EXPECT_EQ(50, ItemV.Len());
		EXPECT_FALSE(Gix->IsKeyIdHDirty());
	}
	EXPECT_EQ(CreatedTm, TFile::GetLastWriteTm(GixFNm)) << ".Gix file was rewritten on a read-only pass";
	SleepForTmResolution();

	// open in update mode and add an item under a new key - now it must be saved
	{
		PTestGix Gix = TGix<TTestGixKey, TTestGixItem>::New("DirtySkip", FPath, faUpdate, &ItemHandler, 10000000);
		Gix->AddItem(TTestGixKey(500, 0), TTestGixItem(1u));
		EXPECT_TRUE(Gix->IsKeyIdHDirty());
	}
	const uint64 ModifiedTm = TFile::GetLastWriteTm(GixFNm);
	EXPECT_NE(CreatedTm, ModifiedTm) << ".Gix file was not saved after a modification";

	// the modification must be visible after reload, and old data intact
	{
		PTestGix Gix = TGix<TTestGixKey, TTestGixItem>::New("DirtySkip", FPath, faUpdate, &ItemHandler, 10000000);
		EXPECT_TRUE(Gix->IsKey(TTestGixKey(500, 0)));
		TVec<TTestGixItem> ItemV;
		Gix->GetItemV(TTestGixKey(500, 0), ItemV);
		EXPECT_EQ(1, ItemV.Len());
		Gix->GetItemV(TTestGixKey(99, 0), ItemV);
		EXPECT_EQ(50, ItemV.Len());
	}
	EXPECT_EQ(ModifiedTm, TFile::GetLastWriteTm(GixFNm)) << ".Gix file was rewritten on a read-only pass";
}

namespace {

// shared body for the base-level test, run for both store implementations;
// MetaFNm is the store metadata file whose rewrite behavior we verify
void CheckBaseDirtySkip(const TStr& FPath, const bool& PagedP)
{
	ResetDir(FPath);
	EnsureQmEnv();
	PJsonVal SchemaVal = GetTestStoreSchema(PagedP);
	const uint64 CacheSize = uint64(16) * uint64(TInt::Mega);

	// create the base and add some records
	{
		TQm::PBase Base = TQm::TStorage::NewBase(FPath, SchemaVal, CacheSize, CacheSize, true);
		TWPt<TQm::TStore> Store = Base->GetStoreByStoreNm("TestStore");
		for (int RecN = 0; RecN < 100; RecN++) { AddTestRec(Store, RecN); }
		TQm::TStorage::SaveBase(Base);
	}

	const TStr MetaFNm = FPath + (PagedP ? "TestStorePgBlobStore" : "TestStore.GenericStore");
	const TStr BaseStoreFNm = FPath + "TestStore.BaseStore";
	const TStr IndexVocFNm = FPath + "IndexVoc.dat";
	const TStr GixFNm = FPath + "Index.GixFull.Gix";
	ASSERT_TRUE(TFile::Exists(MetaFNm));
	ASSERT_TRUE(TFile::Exists(BaseStoreFNm));
	ASSERT_TRUE(TFile::Exists(IndexVocFNm));
	ASSERT_TRUE(TFile::Exists(GixFNm));

	const uint64 MetaTm1 = TFile::GetLastWriteTm(MetaFNm);
	const uint64 BaseStoreTm1 = TFile::GetLastWriteTm(BaseStoreFNm);
	const uint64 IndexVocTm1 = TFile::GetLastWriteTm(IndexVocFNm);
	const uint64 GixTm1 = TFile::GetLastWriteTm(GixFNm);
	SleepForTmResolution();

	// load in update mode, only read - none of the metadata may be rewritten
	{
		TQm::PBase Base = TQm::TStorage::LoadBase(FPath, faUpdate, CacheSize, CacheSize);
		TWPt<TQm::TStore> Store = Base->GetStoreByStoreNm("TestStore");
		EXPECT_EQ(100, (int)Store->GetRecs());
		const uint64 RecId = Store->GetRecId("rec-42");
		ASSERT_TRUE(Store->IsRecId(RecId));
		EXPECT_EQ(42, Store->GetFieldInt(RecId, Store->GetFieldId("Value")));
	}
	EXPECT_EQ(MetaTm1, TFile::GetLastWriteTm(MetaFNm)) << "store metadata rewritten on a read-only pass";
	EXPECT_EQ(BaseStoreTm1, TFile::GetLastWriteTm(BaseStoreFNm)) << ".BaseStore rewritten on a read-only pass";
	EXPECT_EQ(IndexVocTm1, TFile::GetLastWriteTm(IndexVocFNm)) << "IndexVoc.dat rewritten on a read-only pass";
	EXPECT_EQ(GixTm1, TFile::GetLastWriteTm(GixFNm)) << ".Gix key hash rewritten on a read-only pass";
	SleepForTmResolution();

	// load in update mode and add a record - metadata and vocabulary must be saved
	{
		TQm::PBase Base = TQm::TStorage::LoadBase(FPath, faUpdate, CacheSize, CacheSize);
		TWPt<TQm::TStore> Store = Base->GetStoreByStoreNm("TestStore");
		AddTestRec(Store, 100);
	}
	EXPECT_NE(MetaTm1, TFile::GetLastWriteTm(MetaFNm)) << "store metadata not saved after adding a record";
	EXPECT_NE(IndexVocTm1, TFile::GetLastWriteTm(IndexVocFNm)) << "IndexVoc.dat not saved after adding a record";

	// the added record must be present after reload together with the old data
	{
		TQm::PBase Base = TQm::TStorage::LoadBase(FPath, faRdOnly, CacheSize, CacheSize);
		TWPt<TQm::TStore> Store = Base->GetStoreByStoreNm("TestStore");
		EXPECT_EQ(101, (int)Store->GetRecs());
		const uint64 NewRecId = Store->GetRecId("rec-100");
		ASSERT_TRUE(Store->IsRecId(NewRecId));
		EXPECT_EQ(100, Store->GetFieldInt(NewRecId, Store->GetFieldId("Value")));
		const uint64 OldRecId = Store->GetRecId("rec-7");
		ASSERT_TRUE(Store->IsRecId(OldRecId));
		EXPECT_EQ(7, Store->GetFieldInt(OldRecId, Store->GetFieldId("Value")));
	}
}

} // namespace

TEST(QmDirtySkipTests, PagedStoreBaseSkipsUnchangedSaves)
{
	CheckBaseDirtySkip("./qm_dirtyskip_paged/", true);
}

TEST(QmDirtySkipTests, GenericStoreBaseSkipsUnchangedSaves)
{
	CheckBaseDirtySkip("./qm_dirtyskip_generic/", false);
}
