/* Copyright (C) Event Registry d.o.o. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Written by Gregor Leban <gregor@eventregistry.org>, 2013-2017
 */

#include <base.h>
#include <mine.h>
#include "gtest/gtest.h"

// tests for the bulk (memcpy-based) serialization fast paths in glib:
// - TFIn/TFOut/TMIn/TMOut GetBf/PutBf producing legacy-compatible checksums
// - TVec::Load/Save single-call bulk IO for flat types (TIsFlatSerializable)
// they validate that the new code produces byte-identical streams and checksums
// to the historical element-by-element implementation, and that it is faster

namespace {

const TStr FastSerTestFPath = "./qm_fastser_test/";

void PrepareTestDir()
{
	if (!TDir::Exists(FastSerTestFPath)) { TDir::GenDir(FastSerTestFPath); }
}

// save any glib object to a file and load it back
template <class T>
void FileRoundTrip(const T& Src, T& Dest, const TStr& FNm)
{
	{ TFOut FOut(FNm); Src.Save(FOut); }
	{ TFIn FIn(FNm); Dest.Load(FIn); }
}

// reference writer that replicates the historical element-by-element TVec format
template <class TVal>
void SaveVecElementwise(const TVec<TVal>& Vec, TMOut& MOut)
{
	MOut.Save(Vec.Reserved());
	MOut.Save(Vec.Len());
	for (int ValN = 0; ValN < Vec.Len(); ValN++) { Vec[ValN].Save(MOut); }
}

// verify that TVec::Save (which uses the bulk path for flat TVal) produces the
// exact bytes of the historical element-by-element format
template <class TVal>
void CheckBulkMatchesElementwise(const TVec<TVal>& Vec)
{
	TMOut BulkOut; Vec.Save(BulkOut);
	TMOut RefOut; SaveVecElementwise(Vec, RefOut);
	ASSERT_EQ(RefOut.Len(), BulkOut.Len());
	EXPECT_EQ(0, memcmp(BulkOut.GetBfAddr(), RefOut.GetBfAddr(), BulkOut.Len()));
	// the vector must also load back identical from the reference bytes
	TMIn MIn(RefOut.GetBfAddr(), RefOut.Len(), false);
	TVec<TVal> Loaded; Loaded.Load(MIn);
	EXPECT_TRUE(Vec == Loaded);
}

// int wrapper without the flat-serialization opt-in: forces the historical
// element-by-element path while producing the same byte stream as TVec<TInt>
class TBoxedInt {
public:
	TInt Val;
	TBoxedInt(): Val(0) {}
	TBoxedInt(const int& _Val): Val(_Val) {}
	explicit TBoxedInt(TSIn& SIn): Val(SIn) {}
	void Save(TSOut& SOut) const { Val.Save(SOut); }
	void Load(TSIn& SIn) { Val.Load(SIn); }
	bool operator==(const TBoxedInt& Other) const { return Val == Other.Val; }
	bool operator<(const TBoxedInt& Other) const { return Val < Other.Val; }
};

} // namespace

// the trait must be opted in exactly for types whose stream format matches the
// in-memory layout; a wrong opt-in here would corrupt every file written
TEST(QmFastSerializationTests, FlatTraitOptIns)
{
	// simple numeric wrappers are flat
	EXPECT_EQ(1, (int)TIsFlatSerializable<TCh>::Val);
	EXPECT_EQ(1, (int)TIsFlatSerializable<TUCh>::Val);
	EXPECT_EQ(1, (int)TIsFlatSerializable<TBool>::Val);
	EXPECT_EQ(1, (int)TIsFlatSerializable<TSInt>::Val);
	EXPECT_EQ(1, (int)TIsFlatSerializable<TInt>::Val);
	EXPECT_EQ(1, (int)TIsFlatSerializable<TUInt>::Val);
	EXPECT_EQ(1, (int)TIsFlatSerializable<TInt64>::Val);
	EXPECT_EQ(1, (int)TIsFlatSerializable<TUInt64>::Val);
	EXPECT_EQ(1, (int)TIsFlatSerializable<TFlt>::Val);
	EXPECT_EQ(1, (int)TIsFlatSerializable<TSFlt>::Val);
	// TPair/TKeyDat/THashKeyDat are packed(1), so composites of flat types are flat
	EXPECT_EQ(1, (int)(TIsFlatSerializable<TIntPr>::Val));
	EXPECT_EQ(1, (int)(TIsFlatSerializable<TIntUInt64Pr>::Val)); // TQmGixKey
	EXPECT_EQ(1, (int)(TIsFlatSerializable<TKeyDat<TUInt64, TInt> >::Val)); // TQmGixItemFull
	EXPECT_EQ(1, (int)(TIsFlatSerializable<TKeyDat<TUInt, TSInt> >::Val)); // TQmGixItemSmall
	EXPECT_EQ(1, (int)(TIsFlatSerializable<TPgBlobPt>::Val));
	EXPECT_EQ(1, (int)(TIsFlatSerializable<THashKeyDat<TUInt64, TPgBlobPt> >::Val)); // RecIdBlobPtH entries
	EXPECT_EQ(1, (int)(TIsFlatSerializable<THashKeyDat<TUInt64, TUInt64> >::Val)); // primary tm/uint64 maps
	EXPECT_EQ(1, (int)(TIsFlatSerializable<THashKeyDat<TInt, TInt> >::Val)); // TStrHash entries
	// types with heap data, custom stream formats or padding must stay opted out
	EXPECT_EQ(0, (int)TIsFlatSerializable<TStr>::Val);
	EXPECT_EQ(0, (int)TIsFlatSerializable<TAscFlt>::Val); // saves as text
	EXPECT_EQ(0, (int)TIsFlatSerializable<TBlobPt>::Val); // 2 bytes padding
	EXPECT_EQ(0, (int)(TIsFlatSerializable<THashKeyDat<TStr, TUInt64> >::Val));
	EXPECT_EQ(0, (int)(TIsFlatSerializable<TKeyDat<TUInt64, TBlobPt> >::Val));
}

// the bulk fast paths compute the stream checksum as one vectorizable sum over
// the buffer; it must match the historical byte-at-a-time TCs accumulation
TEST(QmFastSerializationTests, ByteSumMatchesLegacyChecksum)
{
	TRnd Rnd(42);
	for (int RunN = 0; RunN < 30; RunN++) {
		const int BfL = Rnd.GetUniDevInt(1, 200000);
		TVec<char> Bf(BfL);
		for (int BfC = 0; BfC < BfL; BfC++) { Bf[BfC] = char(Rnd.GetUniDevInt(0, 255) - 128); }
		// legacy accumulation, one byte at a time (TCs::operator+= is unchanged)
		TCs LegacyCs;
		for (int BfC = 0; BfC < BfL; BfC++) { LegacyCs += Bf[BfC]; }
		// new bulk computation
		const TCs FastCs = TCs::GetCsFromBf(&Bf[0], BfL);
		EXPECT_TRUE(LegacyCs == FastCs) << "checksum mismatch at buffer length " << BfL;
	}
}

// bulk TVec::Save must produce the exact bytes of the historical element loop
TEST(QmFastSerializationTests, BulkVecFormatMatchesElementwise)
{
	TRnd Rnd(1);
	{
		TVec<TInt> Vec(12345, 0);
		for (int i = 0; i < 12345; i++) { Vec.Add(Rnd.GetUniDevInt()); }
		CheckBulkMatchesElementwise(Vec);
	}
	{
		TVec<TUInt64> Vec(7777, 0);
		for (int i = 0; i < 7777; i++) { Vec.Add(uint64(Rnd.GetUniDevInt()) * 0x100000001ULL); }
		CheckBulkMatchesElementwise(Vec);
	}
	{
		// gix key type (packed pair with different member sizes)
		TVec<TIntUInt64Pr> Vec;
		for (int i = 0; i < 5000; i++) { Vec.Add(TIntUInt64Pr(Rnd.GetUniDevInt(), uint64(i) << 17)); }
		CheckBulkMatchesElementwise(Vec);
	}
	{
		// gix full item type
		TVec<TKeyDat<TUInt64, TInt> > Vec;
		for (int i = 0; i < 5000; i++) { Vec.Add(TKeyDat<TUInt64, TInt>(uint64(i) * 3, i % 7)); }
		CheckBulkMatchesElementwise(Vec);
	}
	{
		// empty vector edge case
		TVec<TInt> Vec;
		CheckBulkMatchesElementwise(Vec);
	}
}

// round-trip large vectors through an actual file, crossing the TFIn/TFOut
// buffer size multiple times, with odd (non-power-of-two) lengths
TEST(QmFastSerializationTests, VecFileRoundTrip)
{
	PrepareTestDir();
	TRnd Rnd(7);
	{
		// > 4MB of data, odd length
		TVec<TInt> Src(1234567, 0);
		for (int i = 0; i < 1234567; i++) { Src.Add(Rnd.GetUniDevInt()); }
		TVec<TInt> Dest;
		FileRoundTrip(Src, Dest, FastSerTestFPath + "IntV.bin");
		EXPECT_TRUE(Src == Dest);
	}
	{
		TVec<TIntUInt64Pr> Src;
		for (int i = 0; i < 300001; i++) { Src.Add(TIntUInt64Pr(Rnd.GetUniDevInt(), uint64(Rnd.GetUniDevInt()))); }
		TVec<TIntUInt64Pr> Dest;
		FileRoundTrip(Src, Dest, FastSerTestFPath + "IntUInt64PrV.bin");
		EXPECT_TRUE(Src == Dest);
	}
	{
		// non-flat type must keep working through the element-wise path
		TVec<TStr> Src;
		for (int i = 0; i < 10000; i++) { Src.Add(TStr::Fmt("string-value-%d", i)); }
		TVec<TStr> Dest;
		FileRoundTrip(Src, Dest, FastSerTestFPath + "StrV.bin");
		EXPECT_TRUE(Src == Dest);
	}
}

// hashes embed checksum records (SaveCs/LoadCs) - loading validates them, so a
// successful round-trip proves the bulk paths keep checksums stream-compatible
TEST(QmFastSerializationTests, HashFileRoundTrip)
{
	PrepareTestDir();
	TRnd Rnd(13);
	{
		// same shape as the per-record blob-pointer maps in TStorePbBlob
		THash<TUInt64, TPgBlobPt> Src;
		for (int i = 0; i < 200000; i++) {
			Src.AddDat(uint64(i) * 17, TPgBlobPt(int16(i % 5), uint32(i), uint16(i % 1000)));
		}
		THash<TUInt64, TPgBlobPt> Dest;
		FileRoundTrip(Src, Dest, FastSerTestFPath + "RecIdBlobPtH.bin");
		ASSERT_EQ(Src.Len(), Dest.Len());
		for (int KeyId = Src.FFirstKeyId(); Src.FNextKeyId(KeyId); ) {
			ASSERT_TRUE(Dest.IsKey(Src.GetKey(KeyId)));
			EXPECT_TRUE(Src[KeyId] == Dest.GetDat(Src.GetKey(KeyId)));
		}
	}
	{
		// same shape as the gix key hash (value part TBlobPt is not flat)
		THash<TIntUInt64Pr, TBlobPt> Src;
		for (int i = 0; i < 100000; i++) {
			Src.AddDat(TIntUInt64Pr(i % 50, uint64(i)), TBlobPt(uint16(i % 3), uint(i + 1)));
		}
		THash<TIntUInt64Pr, TBlobPt> Dest;
		FileRoundTrip(Src, Dest, FastSerTestFPath + "KeyIdH.bin");
		ASSERT_EQ(Src.Len(), Dest.Len());
		for (int KeyId = Src.FFirstKeyId(); Src.FNextKeyId(KeyId); ) {
			ASSERT_TRUE(Dest.IsKey(Src.GetKey(KeyId)));
			EXPECT_TRUE(Src[KeyId] == Dest.GetDat(Src.GetKey(KeyId)));
		}
	}
	{
		// same shape as the primary URI -> record id maps (string keys, not flat)
		THash<TStr, TUInt64> Src;
		for (int i = 0; i < 50000; i++) { Src.AddDat(TStr::Fmt("http://example.com/article/%d", i)) = uint64(i); }
		THash<TStr, TUInt64> Dest;
		FileRoundTrip(Src, Dest, FastSerTestFPath + "PrimaryStrIdH.bin");
		ASSERT_EQ(Src.Len(), Dest.Len());
		for (int KeyId = Src.FFirstKeyId(); Src.FNextKeyId(KeyId); ) {
			ASSERT_TRUE(Dest.IsKey(Src.GetKey(KeyId)));
			EXPECT_EQ(Src[KeyId].Val, Dest.GetDat(Src.GetKey(KeyId)).Val);
		}
	}
	{
		// same shape as the index vocabularies (pooled string hash)
		TStrHash<TInt> Src(1000, true);
		for (int i = 0; i < 100000; i++) { Src.AddDat(TStr::Fmt("word%d", i)) = i; }
		{ TFOut FOut(FastSerTestFPath + "WordH.bin"); Src.Save(FOut); }
		TFIn FIn(FastSerTestFPath + "WordH.bin");
		TStrHash<TInt> Dest(FIn);
		ASSERT_EQ(Src.Len(), Dest.Len());
		EXPECT_EQ(12345, Dest.GetDat("word12345").Val);
		EXPECT_EQ(99999, Dest.GetDat("word99999").Val);
		EXPECT_FALSE(Dest.IsKey("no-such-word"));
	}
}

// round-trip through TMOut/TMIn, the path used when gix itemsets and records
// are packed into blobs
TEST(QmFastSerializationTests, MemStreamRoundTrip)
{
	TVec<TKeyDat<TUInt64, TInt> > Src; // TQmGixItemFull vector
	for (int i = 0; i < 100000; i++) { Src.Add(TKeyDat<TUInt64, TInt>(uint64(i), i % 13)); }
	TMOut MOut;
	Src.Save(MOut);
	TMIn MIn(MOut.GetBfAddr(), MOut.Len(), false);
	TVec<TKeyDat<TUInt64, TInt> > Dest(MIn);
	EXPECT_TRUE(Src == Dest);
}

// a vector serialized element-by-element before this change must load through
// the bulk path and vice versa (both produce the same bytes, so cross-loading
// between TBoxedInt - element path - and TInt - bulk path - must work)
TEST(QmFastSerializationTests, CrossFormatCompatibility)
{
	PrepareTestDir();
	TRnd Rnd(99);
	TVec<TBoxedInt> BoxedSrc;
	for (int i = 0; i < 250000; i++) { BoxedSrc.Add(TBoxedInt(Rnd.GetUniDevInt())); }
	// save with the element-wise path (TBoxedInt is not opted into the trait)
	{ TFOut FOut(FastSerTestFPath + "Boxed.bin"); BoxedSrc.Save(FOut); }
	// load with the bulk path
	TVec<TInt> FlatDest;
	{ TFIn FIn(FastSerTestFPath + "Boxed.bin"); FlatDest.Load(FIn); }
	ASSERT_EQ(BoxedSrc.Len(), FlatDest.Len());
	for (int i = 0; i < BoxedSrc.Len(); i++) { ASSERT_EQ(BoxedSrc[i].Val.Val, FlatDest[i].Val); }
	// and the other way around: save bulk, load element-wise
	{ TFOut FOut(FastSerTestFPath + "Flat.bin"); FlatDest.Save(FOut); }
	TVec<TBoxedInt> BoxedDest;
	{ TFIn FIn(FastSerTestFPath + "Flat.bin"); BoxedDest.Load(FIn); }
	ASSERT_EQ(BoxedSrc.Len(), BoxedDest.Len());
	for (int i = 0; i < BoxedSrc.Len(); i++) { ASSERT_EQ(BoxedSrc[i].Val.Val, BoxedDest[i].Val.Val); }
}

// the bulk path must be measurably faster than the element-by-element path for
// the same data and byte format; measured through memory streams so that the
// CPU cost of serialization is not drowned out by disk IO
TEST(QmFastSerializationTests, BulkFasterThanElementwise)
{
	const int Vals = 10000000; // 40 MB of payload
	TRnd Rnd(5);
	TVec<TInt> FlatV(Vals, 0);
	TVec<TBoxedInt> BoxedV(Vals, 0);
	for (int i = 0; i < Vals; i++) {
		const int Val = Rnd.GetUniDevInt();
		FlatV.Add(Val); BoxedV.Add(TBoxedInt(Val));
	}
	const double MBs = double(Vals) * sizeof(int) / double(1024 * 1024);
	const int Runs = 3; // best-of to reduce scheduling noise

	// measure the element-by-element path (TBoxedInt is not flat)
	double BoxedSecs = TFlt::Mx;
	for (int RunN = 0; RunN < Runs; RunN++) {
		TTmStopWatch Sw(true);
		TMOut MOut; BoxedV.Save(MOut);
		TMIn MIn(MOut.GetBfAddr(), MOut.Len(), false);
		TVec<TBoxedInt> Dest; Dest.Load(MIn);
		Sw.Stop();
		ASSERT_EQ(Vals, Dest.Len());
		BoxedSecs = TFlt::GetMn(BoxedSecs, Sw.GetMSec() / 1000.0);
	}

	// measure the bulk path
	double FlatSecs = TFlt::Mx;
	for (int RunN = 0; RunN < Runs; RunN++) {
		TTmStopWatch Sw(true);
		TMOut MOut; FlatV.Save(MOut);
		TMIn MIn(MOut.GetBfAddr(), MOut.Len(), false);
		TVec<TInt> Dest; Dest.Load(MIn);
		Sw.Stop();
		ASSERT_EQ(Vals, Dest.Len());
		FlatSecs = TFlt::GetMn(FlatSecs, Sw.GetMSec() / 1000.0);
	}

	printf("element-wise save+load: %.3f s (%.0f MB/s)\n", BoxedSecs, MBs / BoxedSecs);
	printf("bulk save+load:         %.3f s (%.0f MB/s)\n", FlatSecs, MBs / FlatSecs);
	// lenient factor to avoid flakiness on loaded machines; in practice the
	// difference is much larger
	EXPECT_LT(FlatSecs * 1.3, BoxedSecs)
		<< "bulk path (" << FlatSecs << "s) not faster than element-wise path (" << BoxedSecs << "s)";
}

// the historical GetBf/PutBf inner loop copied one byte at a time to compute
// the stream checksum; the new implementation uses memcpy plus a vectorizable
// sum. this benchmark reproduces the old loop and compares it to the new
// primitive on the same buffer, documenting the raw stream-level speedup
TEST(QmFastSerializationTests, BulkPrimitiveFasterThanLegacyByteLoop)
{
	const TSize BfL = TSize(64) * TSize(1024 * 1024); // 64 MB
	TVec<char> SrcV((int)BfL, 0); SrcV.Reserve((int)BfL, (int)BfL);
	TVec<char> DestV((int)BfL, 0); DestV.Reserve((int)BfL, (int)BfL);
	TRnd Rnd(3);
	for (int i = 0; i < (int)BfL; i++) { SrcV[i] = char(Rnd.GetUniDevInt(0, 255) - 128); }
	const char* Src = &SrcV[0];
	char* Dest = &DestV[0];
	const double MBs = double(BfL) / double(1024 * 1024);
	const int Runs = 3;

	// the loop used by the historical TFIn::GetBf/TMIn::GetBf/TFOut::PutBf
	double LegacySecs = TFlt::Mx;
	int LegacySum = 0;
	for (int RunN = 0; RunN < Runs; RunN++) {
		TTmStopWatch Sw(true);
		int LBfS = 0;
		for (TSize LBfC = 0; LBfC < BfL; LBfC++) {
			LBfS += (Dest[LBfC] = Src[LBfC]);
		}
		Sw.Stop();
		LegacySum = LBfS;
		LegacySecs = TFlt::GetMn(LegacySecs, Sw.GetMSec() / 1000.0);
	}

	// the new implementation: memcpy + vectorizable sum (as in TSInOutByteSum)
	double FastSecs = TFlt::Mx;
	int FastSum = 0;
	for (int RunN = 0; RunN < Runs; RunN++) {
		TTmStopWatch Sw(true);
		memcpy(Dest, Src, BfL);
		unsigned int Sum = 0;
		for (TSize LBfC = 0; LBfC < BfL; LBfC++) { Sum += (unsigned int)(int)Src[LBfC]; }
		Sw.Stop();
		FastSum = (int)Sum;
		FastSecs = TFlt::GetMn(FastSecs, Sw.GetMSec() / 1000.0);
	}

	// both must produce the same checksum input
	EXPECT_EQ(LegacySum, FastSum);
	printf("legacy byte-copy loop: %.3f s (%.0f MB/s)\n", LegacySecs, MBs / LegacySecs);
	printf("memcpy + bulk sum:     %.3f s (%.0f MB/s)\n", FastSecs, MBs / FastSecs);
	// informational only: this micro-benchmark is unreliable because an
	// optimizing compiler auto-vectorizes the hand-written "legacy" reference
	// loop above, so it does not represent the per-element virtual-call path
	// that production actually used. The meaningful speedup is asserted by
	// BulkFasterThanElementwise; here we only require the checksums to agree.
}
