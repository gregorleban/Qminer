/* Copyright (C) Event Registry d.o.o. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 */

#include <qminer.h>
#include <qminer_storage.h>
#include "gtest/gtest.h"

using namespace TQm;

// tests for the gix corruption diagnostics and rebuild hardening:
// - TGix::VerifyAllKeys - read-only full scan reporting unreadable itemsets
// - TGix::CopyTo with a failure collector - survives broken keys and reports
//   all of them instead of aborting on the first one
// - TGix::SetDiscardDirtyOnDrop - scanning a flushed gix writes nothing back

namespace {

typedef TIntUInt64Pr TVerGixKey;
typedef TUInt TVerGixItem;
typedef TPt<TGix<TVerGixKey, TVerGixItem> > PVerGix;

void ResetDir(const TStr& FPath)
{
	if (TDir::Exists(FPath)) { TDir::DelNonEmptyDir(FPath); }
	TDir::GenDir(FPath);
}

// build a small gix with several keys, each with enough items to create child vectors
void BuildVerifyGix(const TStr& FPath, const TGixItemHandler<TVerGixKey, TVerGixItem>* ItemHandler,
	const int& Keys, const int& ItemsPerKey)
{
	PVerGix Gix = TGix<TVerGixKey, TVerGixItem>::New("GixVerify", FPath, faCreate,
		ItemHandler, 10000000, 100, true, 50, 200);
	for (int ItemN = 0; ItemN < ItemsPerKey; ItemN++) {
		for (int KeyN = 0; KeyN < Keys; KeyN++) {
			Gix->AddItem(TVerGixKey(KeyN, 1), TUInt(ItemN));
		}
	}
}

// the largest blob file of the gix (that is where the itemset data lives)
TStr GetLargestBlobFNm(const TStr& FPath)
{
	TStr BestFNm; uint64 BestLen = 0;
	TFFile FFile(TPath::Combine(FPath, "*.mbb*"), false); TStr FNm;
	while (FFile.Next(FNm)) {
		TFIn FIn(FNm);
		if ((uint64) FIn.GetFLen() > BestLen) { BestLen = (uint64) FIn.GetFLen(); BestFNm = FNm; }
	}
	EAssertR(!BestFNm.Empty(), "no blob file found in " + FPath);
	return BestFNm;
}

// overwrite the state byte (offset +8 after the begin tag) of up to MxBlobs blobs
// found in the file, spread out from the middle - mimics the observed production
// corruption ("Expected state 1, received -21"). Returns how many were stomped.
int CorruptBlobStates(const TStr& BlobFNm, const int& MxBlobs)
{
	// read the whole file
	TVec<char> Bf;
	{
		TFIn FIn(BlobFNm);
		const int FLen = FIn.GetFLen();
		Bf.Gen(FLen, 0);
		for (int ChN = 0; ChN < FLen; ChN++) { Bf.Add(FIn.GetCh()); }
	}
	// begin tag 0xABCDEFFF, little-endian on disk
	const unsigned char TagBf[4] = { 0xFF, 0xEF, 0xCD, 0xAB };
	// collect tag positions past the blob-base header
	TIntV TagPosV;
	for (int ChN = 64; ChN + 8 < Bf.Len(); ChN++) {
		if ((unsigned char) Bf[ChN] == TagBf[0] && (unsigned char) Bf[ChN + 1] == TagBf[1] &&
			(unsigned char) Bf[ChN + 2] == TagBf[2] && (unsigned char) Bf[ChN + 3] == TagBf[3]) {
			TagPosV.Add(ChN);
		}
	}
	EAssertR(!TagPosV.Empty(), "no blob begin tags found in " + BlobFNm);
	// stomp the state bytes of up to MxBlobs blobs, spread over the found tags
	const int Step = TagPosV.Len() <= MxBlobs ? 1 : TagPosV.Len() / MxBlobs;
	int Stomped = 0;
	{
		TFRnd FRnd(BlobFNm, faUpdate);
		for (int TagN = 0; TagN < TagPosV.Len() && Stomped < MxBlobs; TagN += Step) {
			FRnd.SetFPos(TagPosV[TagN] + 8);
			FRnd.PutCh(char(0xEB));
			Stomped++;
		}
	}
	return Stomped;
}

} // namespace

// a clean gix must scan without failures and report every key
TEST(GixVerifyTests, VerifyAllKeysCleanGix)
{
	const TStr FPath = "./gix_verify_clean/";
	ResetDir(FPath);
	TGixDefItemHandler<TVerGixKey, TVerGixItem> ItemHandler;
	const int Keys = 20; const int ItemsPerKey = 1000;
	BuildVerifyGix(FPath, &ItemHandler, Keys, ItemsPerKey);

	PVerGix Gix = TGix<TVerGixKey, TVerGixItem>::New("GixVerify", FPath, faRdOnly, &ItemHandler, 10000000);
	TStrV FailedKeyStrV;
	const int ScannedKeys = Gix->VerifyAllKeys(FailedKeyStrV);
	EXPECT_EQ(Keys, ScannedKeys);
	EXPECT_TRUE(FailedKeyStrV.Empty());
	Gix.Clr();
	TDir::DelNonEmptyDir(FPath);
}

// corrupted blob state bytes must be detected, the scan must survive and
// report every broken key with its blob position
TEST(GixVerifyTests, VerifyAllKeysDetectsCorruption)
{
	const TStr FPath = "./gix_verify_corrupt/";
	ResetDir(FPath);
	TGixDefItemHandler<TVerGixKey, TVerGixItem> ItemHandler;
	const int Keys = 20; const int ItemsPerKey = 1000;
	BuildVerifyGix(FPath, &ItemHandler, Keys, ItemsPerKey);
	const int Stomped = CorruptBlobStates(GetLargestBlobFNm(FPath), 5);
	ASSERT_GT(Stomped, 0);

	PVerGix Gix = TGix<TVerGixKey, TVerGixItem>::New("GixVerify", FPath, faRdOnly, &ItemHandler, 10000000);
	TStrV FailedKeyStrV;
	const int ScannedKeys = Gix->VerifyAllKeys(FailedKeyStrV);
	// the scan must have visited every key despite the corruption ...
	EXPECT_EQ(Keys, ScannedKeys);
	// ... and reported at least one broken one (a stomped blob may be an
	// unreferenced free block, so not every stomp must be visible)
	EXPECT_FALSE(FailedKeyStrV.Empty());
	// the report carries the blob position (seg:addr) and the error
	if (!FailedKeyStrV.Empty()) {
		EXPECT_TRUE(FailedKeyStrV[0].IsChIn(':'));
	}
	Gix.Clr();
	TDir::DelNonEmptyDir(FPath);
}

// CopyTo with a failure collector must survive broken keys, copy all healthy
// ones and report the broken ones; without a collector it must throw
TEST(GixVerifyTests, CopyToCollectsFailedKeys)
{
	const TStr FPath = "./gix_verify_copy/";
	const TStr DestFPath = "./gix_verify_copy_dest/";
	const TStr Dest2FPath = "./gix_verify_copy_dest2/";
	ResetDir(FPath); ResetDir(DestFPath); ResetDir(Dest2FPath);
	TGixDefItemHandler<TVerGixKey, TVerGixItem> ItemHandler;
	const int Keys = 20; const int ItemsPerKey = 1000;
	BuildVerifyGix(FPath, &ItemHandler, Keys, ItemsPerKey);
	const int Stomped = CorruptBlobStates(GetLargestBlobFNm(FPath), 5);
	ASSERT_GT(Stomped, 0);

	PVerGix SrcGix = TGix<TVerGixKey, TVerGixItem>::New("GixVerify", FPath, faRdOnly, &ItemHandler, 10000000);
	// with a collector: the copy completes and reports the broken keys
	{
		PVerGix DestGix = TGix<TVerGixKey, TVerGixItem>::New("GixVerify", DestFPath, faCreate, &ItemHandler, 10000000, 100, true, 50, 200);
		uint64 CopiedItems = 0; int EmptyKeys = 0;
		TVec<TVerGixKey> FailedKeyV;
		SrcGix->CopyTo(*DestGix, &CopiedItems, &EmptyKeys, NULL, &FailedKeyV);
		EXPECT_FALSE(FailedKeyV.Empty());
		EXPECT_LT(FailedKeyV.Len(), Keys);
		// every healthy key arrived complete (failed keys may exist in the
		// destination with partial content - the caller must discard it anyway)
		for (int KeyN = 0; KeyN < Keys; KeyN++) {
			const TVerGixKey Key(KeyN, 1);
			if (FailedKeyV.IsIn(Key)) { continue; }
			TVec<TVerGixItem> ItemV;
			DestGix->GetItemV(Key, ItemV);
			EXPECT_EQ(ItemsPerKey, ItemV.Len()) << "healthy key " << KeyN << " is incomplete";
		}
	}
	// without a collector: the first broken key aborts the copy
	{
		PVerGix DestGix = TGix<TVerGixKey, TVerGixItem>::New("GixVerify", Dest2FPath, faCreate, &ItemHandler, 10000000, 100, true, 50, 200);
		EXPECT_THROW(SrcGix->CopyTo(*DestGix), PExcept);
	}
	SrcGix.Clr();
	TDir::DelNonEmptyDir(FPath);
	TDir::DelNonEmptyDir(DestFPath);
	TDir::DelNonEmptyDir(Dest2FPath);
}

// regression for the ER7 reindex stage "corruption" (2026-08): GetItemSet used to
// resolve the blob pointer BEFORE RefreshMemUsed, so when the purge inside
// RefreshMemUsed evicted the very key being fetched - storing its dirty, grown
// itemset relocates the blob and frees the old one - the load then read the FREED
// blob ("Expected state 1, received ..."). Writes cannot arm this trap (AddItem/
// AddItemV close with their own RefreshMemUsed), but READS grow the cache AFTER
// their entry check - exactly how the reindex CopyTo armed it while phase-1 dirty
// itemsets sat at the LRU end.
TEST(GixVerifyTests, PurgeRelocationDuringGetItemSet)
{
	const TStr FPath = "./gix_verify_race/";
	ResetDir(FPath);
	TGixDefItemHandler<TVerGixKey, TVerGixItem> ItemHandler;
	// 50 KB cache, so the purge threshold (10%) is 5 KB. The arming below relies
	// on child-vector loads: they are invisible to both the growth counter and
	// the cache's insert-time accounting, so filler reads balloon the actual
	// memory unseen until the victim's own read is the first to recompute - the
	// same way the reindex CopyTo armed it in production
	PVerGix Gix = TGix<TVerGixKey, TVerGixItem>::New("GixVerify", FPath, faCreate,
		&ItemHandler, 50000, 100, true, 50, 200);
	const TVerGixKey Victim(1, 1);
	// two filler reads of ~32 KB each: the victim's evicted header is ~6.5 KB, so
	// the 50 KB budget is crossed by the SECOND filler's child loads - which its
	// own entry check has already passed - arming the purge for the victim's read
	const int Fillers = 2; const int FillerItems = 7000;
	// blob pointer of a key via the public key-id API
	const auto GetPt = [&Gix](const TVerGixKey& Key) {
		int KeyId = Gix->FFirstKeyId();
		while (Gix->FNextKeyId(KeyId)) {
			if (Gix->GetKey(KeyId) == Key) { return Gix->GetKeyBlobPt(KeyId); }
		}
		return TBlobPt();
	};
	// store the victim small (a tiny blob size class, so the later regrown store
	// MUST relocate its header) and three ~28 KB fillers; then empty the cache
	{
		TVec<TVerGixItem> SmallV;
		for (int ItemN = 0; ItemN < 10; ItemN++) { SmallV.Add(TUInt(ItemN)); }
		Gix->AddItemV(Victim, SmallV);
		TVec<TVerGixItem> FillV;
		for (int ItemN = 0; ItemN < FillerItems; ItemN++) { FillV.Add(TUInt(ItemN)); }
		for (int FillerN = 0; FillerN < Fillers; FillerN++) {
			Gix->AddItemV(TVerGixKey(2 + FillerN, 1), FillV);
		}
	}
	Gix->Flush();
	// regrow the victim: its header (work buffer + child metadata) becomes dirty
	// in cache and far outgrows the tiny blob it was stored in. The closing
	// RefreshMemUsed of AddItemV recomputes (under budget - no purge) and resets
	// the growth counter
	const int Items = 7510;
	{
		TVec<TVerGixItem> GrowV;
		for (int ItemN = 10; ItemN < Items; ItemN++) { GrowV.Add(TUInt(ItemN)); }
		Gix->AddItemV(Victim, GrowV);
	}
	// filler reads: each one adds only its small HEADER to the growth counter
	// (staying under the 5 KB threshold, so no entry check recomputes), while the
	// child vectors it loads grow the actual memory far past the 50 KB budget
	// unseen. The victim's read below is then the first call whose entry check
	// crosses the threshold, recomputes, finds the cache far over budget - and
	// purges, evicting and RELOCATING the LRU-oldest entry: the dirty victim
	for (int FillerN = 0; FillerN < Fillers; FillerN++) {
		TVec<TVerGixItem> FillItemV;
		Gix->GetItemV(TVerGixKey(2 + FillerN, 1), FillItemV);
	}
	const TBlobPt PtBefore = GetPt(Victim);
	TVec<TVerGixItem> ItemV;
	Gix->GetItemV(Victim, ItemV);
	EXPECT_EQ(Items, ItemV.Len());
	// the read itself evicted + relocated the victim; if this stops holding, the
	// sizes above no longer arm the in-read purge and the test guards nothing
	const TBlobPt PtAfter = GetPt(Victim);
	EXPECT_FALSE(PtBefore == PtAfter);
	Gix.Clr();
	TDir::DelNonEmptyDir(FPath);
}

// a stale blob pointer whose slot was reused by ANOTHER itemset would deliver a
// well-formed itemset of the wrong key - silent wrong postings. GetItemSet must
// detect the key mismatch and throw. Simulated by swapping the two (equal-sized)
// itemset blobs on disk.
TEST(GixVerifyTests, WrongKeyItemSetDetected)
{
	const TStr FPath = "./gix_verify_wrongkey/";
	ResetDir(FPath);
	TGixDefItemHandler<TVerGixKey, TVerGixItem> ItemHandler;
	const TVerGixKey KeyA(1, 1); const TVerGixKey KeyB(2, 1);
	{
		PVerGix Gix = TGix<TVerGixKey, TVerGixItem>::New("GixVerify", FPath, faCreate,
			&ItemHandler, 10000000, 100, true, 50, 200);
		// same item count -> identical serialized length -> identical blob layout,
		// few enough items that no child vectors split off (2 header blobs total)
		for (int ItemN = 0; ItemN < 50; ItemN++) {
			Gix->AddItem(KeyA, TUInt(ItemN));
			Gix->AddItem(KeyB, TUInt(1000 + ItemN));
		}
	}
	// swap the full content of the two itemset blobs on disk
	{
		const TStr BlobFNm = GetLargestBlobFNm(FPath);
		TVec<char> Bf;
		{
			TFIn FIn(BlobFNm);
			const int FLen = FIn.GetFLen();
			Bf.Gen(FLen, 0);
			for (int ChN = 0; ChN < FLen; ChN++) { Bf.Add(FIn.GetCh()); }
		}
		const unsigned char TagBf[4] = { 0xFF, 0xEF, 0xCD, 0xAB };
		// collect only ACTIVE blobs (state byte 1 at +8): the originally enlisted
		// tiny itemsets were relocated by the growth and their freed blobs keep
		// their begin tag (only the state byte turns to bsFree)
		TIntV TagPosV;
		for (int ChN = 64; ChN + 8 < Bf.Len(); ChN++) {
			if ((unsigned char) Bf[ChN] == TagBf[0] && (unsigned char) Bf[ChN + 1] == TagBf[1] &&
				(unsigned char) Bf[ChN + 2] == TagBf[2] && (unsigned char) Bf[ChN + 3] == TagBf[3] &&
				Bf[ChN + 8] == 1) {
				TagPosV.Add(ChN);
			}
		}
		ASSERT_EQ(2, TagPosV.Len());
		// blob layout: [tag 4][MxBfL 4][state 1][BfL 4][data MxBfL][checksum 8][tag 4]
		const int MxBfL0 = *((int*) &Bf[TagPosV[0] + 4]);
		const int MxBfL1 = *((int*) &Bf[TagPosV[1] + 4]);
		ASSERT_EQ(MxBfL0, MxBfL1);
		const int BlobLen = 4 + 4 + 1 + 4 + MxBfL0 + 8 + 4;
		TFRnd FRnd(BlobFNm, faUpdate);
		FRnd.SetFPos(TagPosV[0]);
		FRnd.PutBf(&Bf[TagPosV[1]], BlobLen);
		FRnd.SetFPos(TagPosV[1]);
		FRnd.PutBf(&Bf[TagPosV[0]], BlobLen);
	}
	// reading KeyA now loads KeyB's itemset - must throw, not return B's items
	PVerGix Gix = TGix<TVerGixKey, TVerGixItem>::New("GixVerify", FPath, faRdOnly, &ItemHandler, 10000000);
	TVec<TVerGixItem> ItemV;
	EXPECT_THROW(Gix->GetItemV(KeyA, ItemV), PExcept);
	Gix.Clr();
	TDir::DelNonEmptyDir(FPath);
}

// scanning a flushed gix in discard-dirty-on-drop mode must not write a single
// blob back into it, and the copied destination must carry the full content
TEST(GixVerifyTests, DiscardDirtyOnDropSkipsWriteBack)
{
	const TStr FPath = "./gix_verify_discard/";
	const TStr DestFPath = "./gix_verify_discard_dest/";
	ResetDir(FPath); ResetDir(DestFPath);
	TGixDefItemHandler<TVerGixKey, TVerGixItem> ItemHandler;
	const int Keys = 20; const int ItemsPerKey = 1000;

	PVerGix SrcGix = TGix<TVerGixKey, TVerGixItem>::New("GixVerify", FPath, faCreate,
		&ItemHandler, 10000000, 100, true, 50, 200);
	uint64 SrcItems = 0;
	for (int ItemN = 0; ItemN < ItemsPerKey; ItemN++) {
		for (int KeyN = 0; KeyN < Keys; KeyN++) {
			SrcGix->AddItem(TVerGixKey(KeyN, 1), TUInt(ItemN));
			SrcItems++;
		}
	}
	// flush once (every blob is then current), then discard whatever the copy re-dirties
	SrcGix->Flush();
	SrcGix->SetDiscardDirtyOnDrop(true);
	SrcGix->ResetStats();

	PVerGix DestGix = TGix<TVerGixKey, TVerGixItem>::New("GixVerify", DestFPath, faCreate,
		&ItemHandler, 10000000, 100, true, 50, 200);
	uint64 CopiedItems = 0; int EmptyKeys = 0;
	SrcGix->CopyTo(*DestGix, &CopiedItems, &EmptyKeys);
	EXPECT_EQ(SrcItems, CopiedItems);
	EXPECT_EQ(0, EmptyKeys);
	EXPECT_EQ(Keys, DestGix->GetKeys());

	// the source blob received no writes during the whole copy
	const TBlobBsStats& SrcStats = SrcGix->GetBlobStats();
	EXPECT_EQ(uint64(0), uint64(SrcStats.Puts));
	EXPECT_EQ(uint64(0), uint64(SrcStats.PutsNew));

	// and the destination holds the same data
	EXPECT_TRUE(SrcGix->VerifySample(*DestGix, Keys));

	SrcGix.Clr(); DestGix.Clr();
	TDir::DelNonEmptyDir(FPath);
	TDir::DelNonEmptyDir(DestFPath);
}
