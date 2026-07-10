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

// tests for defragmentation of the inverted index (TGix::CopyTo, TIndex::DefragGix)

namespace {

typedef TIntUInt64Pr TDefragGixKey;
typedef TUInt TDefragGixItem;
typedef TPt<TGix<TDefragGixKey, TDefragGixItem> > PDefragGix;
typedef TPt<TGixItemSet<TDefragGixKey, TDefragGixItem> > PDefragGixItemSet;

// provider that returns the split length based on the first value in the key pair
class TDefragSplitLenProvider : public TGixSplitLenProvider<TDefragGixKey> {
public:
	THash<TInt, TInt> KeyIdSplitLenH;

	int GetSplitLen(const TDefragGixKey& Key) const {
		TInt SplitLen;
		if (KeyIdSplitLenH.IsKeyGetDat(Key.Val1, SplitLen)) { return SplitLen; }
		return -1;
	}
};

// collect file names (without path) that belong to one gix instance: the key hash
// table (<GixNm>.Gix) and the blob base files, whose name TMBlobBs normalizes
// (e.g. "Index.GixFull" stores blobs in "Index_GixFull.mbb" + numbered segments)
void GetGixFileNames(const TStr& FPath, const TStr& GixNm, TStrV& FNmV)
{
	FNmV.Clr();
	if (TFile::Exists(TPath::Combine(FPath, GixNm + ".Gix"))) {
		FNmV.Add(GixNm + ".Gix");
	}
	const TStr BlobNm = TStr::GetNrFMid(TStr(GixNm + ".GixDat").GetFMid());
	TFFile FFile(TPath::Combine(FPath, BlobNm + ".mbb*"), false);
	TStr FNm;
	while (FFile.Next(FNm)) {
		FNmV.Add(TDir::GetFileName(FNm));
	}
}

} // namespace

TEST(GixDefragTests, CopyToDefragments)
{
	const TStr SrcFPath = "./gix_defrag_src/";
	const TStr DestFPath = "./gix_defrag_dest/";
	if (TDir::Exists(SrcFPath)) { TDir::DelNonEmptyDir(SrcFPath); }
	if (TDir::Exists(DestFPath)) { TDir::DelNonEmptyDir(DestFPath); }
	TDir::GenDir(SrcFPath);
	TDir::GenDir(DestFPath);

	TGixDefItemHandler<TDefragGixKey, TDefragGixItem> ItemHandler;
	const TDefragGixKey KeyA(7, 1);
	const TDefragGixKey KeyB(8, 1);
	const int Items = 1000;

	// build a fragmented source: items for the two keys are added interleaved, so
	// their child vectors alternate in the source blob file
	{
		PDefragGix SrcGix = TGix<TDefragGixKey, TDefragGixItem>::New("GixDefrag", SrcFPath, faCreate, &ItemHandler, 10000000, 100, true, 50, 200);
		for (int N = 0; N < Items; N++) {
			SrcGix->AddItem(KeyA, TUInt(N));
			SrcGix->AddItem(KeyB, TUInt(N));
		}
	}

	// copy into a destination gix that uses a larger split length for KeyA
	TDefragSplitLenProvider Provider;
	Provider.KeyIdSplitLenH.AddDat(7, 500);
	{
		PDefragGix SrcGix = TGix<TDefragGixKey, TDefragGixItem>::New("GixDefrag", SrcFPath, faRdOnly, &ItemHandler, 10000000, 100, true, 50, 200);
		PDefragGix DestGix = TGix<TDefragGixKey, TDefragGixItem>::New("GixDefrag", DestFPath, faCreate, &ItemHandler, 10000000, 100, true, 50, 200);
		DestGix->SetSplitLenProvider(&Provider);

		SrcGix->CopyTo(*DestGix);

		// the copied data must be identical to the source
		EXPECT_TRUE(SrcGix->IsKeyDataEqual(*DestGix, KeyA));
		EXPECT_TRUE(SrcGix->IsKeyDataEqual(*DestGix, KeyB));
		EXPECT_TRUE(SrcGix->VerifySample(*DestGix, 10));

		// KeyA must now use the larger split length, KeyB the default one
		EXPECT_EQ(DestGix->GetItemSet(KeyA)->GetChildVectors(), (Items - 1) / 500);
		EXPECT_EQ(DestGix->GetItemSet(KeyB)->GetChildVectors(), (Items - 1) / 100);
	}

	// reopen the destination and check the data was persisted correctly
	{
		PDefragGix DestGix = TGix<TDefragGixKey, TDefragGixItem>::New("GixDefrag", DestFPath, faRdOnly, &ItemHandler, 10000000, 100, true, 50, 200);
		DestGix->SetSplitLenProvider(&Provider);
		TVec<TDefragGixItem> ItemV; DestGix->GetItemV(KeyA, ItemV);
		ASSERT_EQ(ItemV.Len(), Items);
		for (int N = 0; N < Items; N++) { EXPECT_EQ((int) ItemV[N].Val, N); }
		TVec<TDefragGixItem> ItemBV; DestGix->GetItemV(KeyB, ItemBV);
		ASSERT_EQ(ItemBV.Len(), Items);
	}

	TDir::DelNonEmptyDir(SrcFPath);
	TDir::DelNonEmptyDir(DestFPath);
}

TEST(StoreDefragTests, DefragToPreservesRecordsAndIds)
{
	const TStr FPath = "./store_defrag_base/";
	const TStr BuildFPath = "./store_defrag_base_new/";
	const TStr BackupFPath = "./store_defrag_base_old/";
	if (TDir::Exists(FPath)) { TDir::DelNonEmptyDir(FPath); }
	if (TDir::Exists(BuildFPath)) { TDir::DelNonEmptyDir(BuildFPath); }
	if (TDir::Exists(BackupFPath)) { TDir::DelNonEmptyDir(BackupFPath); }
	TDir::GenDir(FPath);
	TDir::GenDir(BuildFPath);
	TDir::GenDir(BackupFPath);

	// paged store with a big disk-stored field, so some values get TOAST-ed
	const TStr SchemaStr =
		"[{ \"name\": \"TestPagedItem\","
		"   \"fields\": [ { \"name\": \"Name\", \"type\": \"string\", \"primary\": true },"
		"                 { \"name\": \"Value\", \"type\": \"string\" },"
		"                 { \"name\": \"Body\", \"type\": \"string\", \"store\": \"cache\" } ],"
		"   \"keys\": [ { \"field\": \"Value\", \"type\": \"value\", \"storage\": \"tiny\" } ],"
		"   \"options\": { \"type\": \"paged\" }"
		"}]";
	PJsonVal SchemaVal = TJsonVal::GetValFromStr(SchemaStr);
	const int Recs = 100;
	const int Values = 5;

	if (!TQm::TEnv::IsInit()) { TQm::TEnv::Init(); }

	THash<TUInt64, TStr> ExpNameH, ExpValueH, ExpBodyH;
	{
		PBase Base = TStorage::NewBase(FPath, SchemaVal, 10000000, 10000000, true);
		TWPt<TStore> Store = Base->GetStoreByStoreNm("TestPagedItem");
		EXPECT_EQ(Store->GetStoreType(), "TStorePbBlob");

		for (int RecN = 0; RecN < Recs; RecN++) {
			// every third record gets a body over the TOAST limit (~2KB)
			TChA BodyChA = TStr::Fmt("body-%d-", RecN);
			const int BodyLen = (RecN % 3 == 0) ? 5000 : 50;
			while (BodyChA.Len() < BodyLen) { BodyChA += TStr::Fmt("%d,", RecN); }
			PJsonVal RecVal = TJsonVal::NewObj();
			RecVal->AddToObj("Name", TStr::Fmt("rec%d", RecN));
			RecVal->AddToObj("Value", TStr::Fmt("v%d", RecN % Values));
			RecVal->AddToObj("Body", TStr(BodyChA));
			Base->AddRec("TestPagedItem", RecVal);
		}
		// delete a block of records to create gaps in the record id space
		TUInt64V DelRecIdV;
		for (uint64 RecId = 10; RecId < 20; RecId++) { DelRecIdV.Add(RecId); }
		Store->DeleteRecs(DelRecIdV);
		ASSERT_EQ((int) Store->GetRecs(), Recs - 10);

		// remember the expected content of all surviving records
		PStoreIter Iter = Store->GetIter();
		while (Iter->Next()) {
			const uint64 RecId = Iter->GetRecId();
			ExpNameH.AddDat(RecId, Store->GetFieldStr(RecId, Store->GetFieldId("Name")));
			ExpValueH.AddDat(RecId, Store->GetFieldStr(RecId, Store->GetFieldId("Value")));
			ExpBodyH.AddDat(RecId, Store->GetFieldStr(RecId, Store->GetFieldId("Body")));
		}
		ASSERT_EQ(ExpNameH.Len(), Recs - 10);

		// rebuild the store blobs into the build folder
		TStorePbBlob* PbStore = dynamic_cast<TStorePbBlob*>(Store());
		ASSERT_TRUE(PbStore != NULL);
		const uint64 CopiedRecs = PbStore->DefragTo(TPath::Combine(BuildFPath, "TestPagedItem"), 10000000);
		EXPECT_EQ((int) CopiedRecs, Recs - 10);

		TStorage::SaveBase(Base);
	}

	// swap the store files: move the old ones to a backup folder, the rebuilt ones into place
	{
		TStrV FNmV;
		TFFile OldFFile(TPath::Combine(FPath, "TestPagedItemPgBlob*"), false);
		TStr FNm;
		while (OldFFile.Next(FNm)) { FNmV.Add(TDir::GetFileName(FNm)); }
		EXPECT_GE(FNmV.Len(), 5); // PgBlobStore + 2x (.main + .bin0000)
		for (int FNmN = 0; FNmN < FNmV.Len(); FNmN++) {
			TFile::Move(TPath::Combine(FPath, FNmV[FNmN]), TPath::Combine(BackupFPath, FNmV[FNmN]));
		}
		FNmV.Clr();
		TFFile NewFFile(TPath::Combine(BuildFPath, "TestPagedItemPgBlob*"), false);
		while (NewFFile.Next(FNm)) { FNmV.Add(TDir::GetFileName(FNm)); }
		EXPECT_GE(FNmV.Len(), 5);
		for (int FNmN = 0; FNmN < FNmV.Len(); FNmN++) {
			TFile::Move(TPath::Combine(BuildFPath, FNmV[FNmN]), TPath::Combine(FPath, FNmV[FNmN]));
		}
	}

	// reload the base with the defragmented store and check all the records
	{
		PBase Base = TStorage::LoadBase(FPath, faRdOnly, 10000000, 10000000);
		TWPt<TStore> Store = Base->GetStoreByStoreNm("TestPagedItem");
		ASSERT_EQ((int) Store->GetRecs(), Recs - 10);
		// deleted records must stay deleted
		EXPECT_FALSE(Store->IsRecId(10));
		EXPECT_FALSE(Store->IsRecId(15));
		// all the surviving records must keep their ids and content
		for (int KeyId = ExpNameH.FFirstKeyId(); ExpNameH.FNextKeyId(KeyId); ) {
			const uint64 RecId = ExpNameH.GetKey(KeyId);
			ASSERT_TRUE(Store->IsRecId(RecId));
			EXPECT_EQ(Store->GetFieldStr(RecId, Store->GetFieldId("Name")), ExpNameH[KeyId]);
			EXPECT_EQ(Store->GetFieldStr(RecId, Store->GetFieldId("Value")), ExpValueH.GetDat(RecId));
			EXPECT_EQ(Store->GetFieldStr(RecId, Store->GetFieldId("Body")), ExpBodyH.GetDat(RecId));
			// the primary key must still resolve to the same record id
			EXPECT_EQ(Store->GetRecId(ExpNameH[KeyId]), RecId);
		}
		// the index must still be consistent with the store
		PRecSet RecSet = Base->Search("{ \"$from\": \"TestPagedItem\", \"Value\": \"v1\" }");
		int ExpRecs = 0;
		for (int KeyId = ExpValueH.FFirstKeyId(); ExpValueH.FNextKeyId(KeyId); ) {
			if (ExpValueH[KeyId] == "v1") { ExpRecs++; }
		}
		EXPECT_EQ(RecSet->GetRecs(), ExpRecs);
	}

	TDir::DelNonEmptyDir(FPath);
	TDir::DelNonEmptyDir(BuildFPath);
	TDir::DelNonEmptyDir(BackupFPath);
}

TEST(GixDefragTests, DefragGixAndSwapFiles)
{
	const TStr FPath = "./gix_defrag_base/";
	const TStr BuildFPath = "./gix_defrag_base_new/";
	const TStr BackupFPath = "./gix_defrag_base_old/";
	if (TDir::Exists(FPath)) { TDir::DelNonEmptyDir(FPath); }
	if (TDir::Exists(BuildFPath)) { TDir::DelNonEmptyDir(BuildFPath); }
	if (TDir::Exists(BackupFPath)) { TDir::DelNonEmptyDir(BackupFPath); }
	TDir::GenDir(FPath);
	TDir::GenDir(BackupFPath);

	const TStr SchemaStr =
		"[{ \"name\": \"TestItem\","
		"   \"fields\": [ { \"name\": \"Name\", \"type\": \"string\", \"primary\": true },"
		"                 { \"name\": \"Value\", \"type\": \"string\" } ],"
		"   \"keys\": [ { \"field\": \"Value\", \"type\": \"value\", \"storage\": \"tiny\", \"splitLen\": 50 } ]"
		"}]";
	PJsonVal SchemaVal = TJsonVal::GetValFromStr(SchemaStr);
	const int Recs = 500;
	const int Values = 5;

	if (!TQm::TEnv::IsInit()) { TQm::TEnv::Init(); }

	// create a base, fill it with records and defragment its index while open
	{
		PBase Base = TStorage::NewBase(FPath, SchemaVal, 10000000, 10000000, true);
		for (int RecN = 0; RecN < Recs; RecN++) {
			PJsonVal RecVal = TJsonVal::GetValFromStr(TStr::Fmt(
				"{ \"Name\": \"rec%d\", \"Value\": \"v%d\" }", RecN, RecN % Values));
			Base->AddRec("TestItem", RecVal);
		}
		// each value must match Recs / Values records
		PRecSet RecSet = Base->Search("{ \"$from\": \"TestItem\", \"Value\": \"v1\" }");
		ASSERT_EQ(RecSet->GetRecs(), Recs / Values);

		// rebuild the index into the build folder with deep verification of all keys
		Base->GetIndex()->DefragGix(BuildFPath, TStrV(), 10000000, 1000);

		TStorage::SaveBase(Base);
	}

	// swap the index files: move the old ones to a backup folder, the rebuilt ones into place
	TStrV GixNmV;
	GixNmV.Add("Index.GixFull"); GixNmV.Add("Index.GixSmall");
	GixNmV.Add("Index.GixTiny"); GixNmV.Add("Index.GixPos");
	for (int GixNmN = 0; GixNmN < GixNmV.Len(); GixNmN++) {
		TStrV OldFNmV; GetGixFileNames(FPath, GixNmV[GixNmN], OldFNmV);
		EXPECT_GE(OldFNmV.Len(), 2);
		for (int FNmN = 0; FNmN < OldFNmV.Len(); FNmN++) {
			TFile::Move(TPath::Combine(FPath, OldFNmV[FNmN]), TPath::Combine(BackupFPath, OldFNmV[FNmN]));
		}
		TStrV NewFNmV; GetGixFileNames(BuildFPath, GixNmV[GixNmN], NewFNmV);
		EXPECT_GE(NewFNmV.Len(), 2);
		for (int FNmN = 0; FNmN < NewFNmV.Len(); FNmN++) {
			TFile::Move(TPath::Combine(BuildFPath, NewFNmV[FNmN]), TPath::Combine(FPath, NewFNmV[FNmN]));
		}
	}

	// reload the base with the defragmented index and check that searches return the same results
	{
		PBase Base = TStorage::LoadBase(FPath, faRdOnly, 10000000, 10000000);
		TStorage::ApplyIndexKeySplitLen(Base, SchemaVal);
		for (int ValueN = 0; ValueN < Values; ValueN++) {
			PRecSet RecSet = Base->Search(TStr::Fmt("{ \"$from\": \"TestItem\", \"Value\": \"v%d\" }", ValueN));
			EXPECT_EQ(RecSet->GetRecs(), Recs / Values);
		}
	}

	TDir::DelNonEmptyDir(FPath);
	TDir::DelNonEmptyDir(BuildFPath);
	TDir::DelNonEmptyDir(BackupFPath);
}
