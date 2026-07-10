/**
 * Additional edge-case tests for the bulk ("flat") container serialization
 * introduced in bd.h/ds.h/ds.hpp/dt.h/hash.h (TIsFlatSerializable + single-call
 * TVec::Load/Save).
 *
 * QmFastSerializationTests covers the trait opt-ins, checksum equivalence and
 * large random round-trips; here we pin down the remaining boundaries:
 *   - empty and single-element vectors produce byte-identical streams to the
 *     historical element-wise format
 *   - payload sizes exactly on the 1 MB file-buffer boundary (and +/- 1 element)
 *   - nested TVec<TVec<TInt>> must take the element-wise outer path (the trait
 *     must NOT be set for TVec) while inner vectors use the bulk path
 *   - non-finite / signed-zero doubles must round-trip bit-exact
 *   - hashes with deleted entries (holes in KeyDatV) must round-trip
 */

#include <base.h>
#include <mine.h>
#include "gtest/gtest.h"

#include <limits>

namespace {

const TStr FastSerMoreFPath = "./qm_fastser_more/";

void PrepareTestDir()
{
    if (!TDir::Exists(FastSerMoreFPath)) { TDir::GenDir(FastSerMoreFPath); }
}

template <class T>
void FileRoundTrip(const T& Src, T& Dest, const TStr& FNm)
{
    { TFOut FOut(FNm); Src.Save(FOut); }
    { TFIn FIn(FNm); Dest.Load(FIn); }
}

// reference writer replicating the historical element-by-element TVec format
template <class TVal>
void SaveVecElementwise(const TVec<TVal>& Vec, TMOut& MOut)
{
    MOut.Save(Vec.Reserved());
    MOut.Save(Vec.Len());
    for (int ValN = 0; ValN < Vec.Len(); ValN++) { Vec[ValN].Save(MOut); }
}

template <class TVal>
void CheckBulkMatchesElementwise(const TVec<TVal>& Vec)
{
    TMOut BulkOut; Vec.Save(BulkOut);
    TMOut RefOut; SaveVecElementwise(Vec, RefOut);
    ASSERT_EQ(RefOut.Len(), BulkOut.Len());
    if (BulkOut.Len() > 0) {
        EXPECT_EQ(0, memcmp(BulkOut.GetBfAddr(), RefOut.GetBfAddr(), BulkOut.Len()));
    }
    TMIn MIn(RefOut.GetBfAddr(), RefOut.Len(), false);
    TVec<TVal> Loaded; Loaded.Load(MIn);
    EXPECT_TRUE(Vec == Loaded);
}

// int wrapper without the flat opt-in: forces the historical element-wise path
// while producing the same byte stream as TVec<TInt> (same trick as in
// QmFastSerializationTests, kept file-local)
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

// zero- and one-element vectors are the smallest bulk payloads (0 and 4 bytes);
// their streams must stay byte-identical to the element-wise format
TEST(QmFastSerMoreTests, EmptyAndSingleElementVectorsMatchElementwise)
{
    {
        TVec<TInt> Vec; // empty
        CheckBulkMatchesElementwise(Vec);
    }
    {
        TVec<TInt> Vec; Vec.Add(-123456789);
        CheckBulkMatchesElementwise(Vec);
    }
    {
        TVec<TIntUInt64Pr> Vec; Vec.Add(TIntUInt64Pr(-1, TUInt64::Mx)); // TQmGixKey shape
        CheckBulkMatchesElementwise(Vec);
    }
    {
        TVec<TKeyDat<TUInt64, TInt> > Vec; Vec.Add(TKeyDat<TUInt64, TInt>(uint64(1) << 63, TInt::Mn));
        CheckBulkMatchesElementwise(Vec);
    }
}

// empty containers of the shapes used by the qminer stores must round-trip
// through a file (the bulk path must not write or expect any payload bytes)
TEST(QmFastSerMoreTests, EmptyContainersFileRoundTrip)
{
    PrepareTestDir();
    {
        TVec<TInt> Src, Dest;
        FileRoundTrip(Src, Dest, FastSerMoreFPath + "EmptyIntV.bin");
        EXPECT_EQ(0, Dest.Len());
    }
    {
        TVec<TStr> Src, Dest; // non-flat element type
        FileRoundTrip(Src, Dest, FastSerMoreFPath + "EmptyStrV.bin");
        EXPECT_EQ(0, Dest.Len());
    }
    {
        THash<TUInt64, TPgBlobPt> Src, Dest; // flat-entry hash
        FileRoundTrip(Src, Dest, FastSerMoreFPath + "EmptyPtH.bin");
        EXPECT_EQ(0, Dest.Len());
    }
    {
        THash<TStr, TUInt64> Src, Dest; // non-flat-entry hash
        FileRoundTrip(Src, Dest, FastSerMoreFPath + "EmptyStrH.bin");
        EXPECT_EQ(0, Dest.Len());
    }
}

// TVec<TInt> whose serialized size (8-byte header + 4*N payload) lands exactly
// on, one element under and one element over the 1 MB TFOut/TFIn buffer; the
// bulk SaveBf/LoadBf then starts/ends exactly at a flush/refill boundary.
// each size is also cross-loaded through the element-wise path (TBoxedInt) to
// prove the formats stay interchangeable at the boundary.
TEST(QmFastSerMoreTests, PayloadExactlyAtFileBufferBoundary)
{
    PrepareTestDir();
    const int OneMB = 1024 * 1024;
    // 8 + 4 * 262142 == 1 MB exactly
    const int BoundaryVals = (OneMB - 8) / 4;
    ASSERT_EQ(OneMB, 8 + 4 * BoundaryVals);
    const int Counts[] = { BoundaryVals - 1, BoundaryVals, BoundaryVals + 1 };
    for (int CntN = 0; CntN < (int)(sizeof(Counts) / sizeof(int)); CntN++) {
        const int Count = Counts[CntN];
        TVec<TInt> Src(Count, 0);
        for (int N = 0; N < Count; N++) { Src.Add(N * 2654435761u); }
        const TStr FNm = FastSerMoreFPath + TStr::Fmt("Boundary_%d.bin", Count);
        TVec<TInt> Dest;
        FileRoundTrip(Src, Dest, FNm);
        ASSERT_EQ(Count, Dest.Len()) << "wrong length at count " << Count;
        EXPECT_TRUE(Src == Dest) << "data mismatch at count " << Count;
        // the same file must load through the element-wise path
        TVec<TBoxedInt> BoxedDest;
        { TFIn FIn(FNm); BoxedDest.Load(FIn); }
        ASSERT_EQ(Count, BoxedDest.Len());
        EXPECT_EQ(Src[0].Val, BoxedDest[0].Val.Val);
        EXPECT_EQ(Src[Count - 1].Val, BoxedDest[Count - 1].Val.Val);
    }
}

// nested vectors: the outer TVec<TVec<TInt>> must NOT be flat (a bulk memcpy of
// TVec objects would serialize heap pointers), but it must still round-trip with
// the inner vectors going through the bulk path; includes empty inner vectors
// and a >1MB inner vector
TEST(QmFastSerMoreTests, NestedVecOfVecRoundTripsViaElementwiseOuterPath)
{
    PrepareTestDir();
    // the outer trait staying 0 is what keeps this safe - assert it
    EXPECT_EQ(0, (int)(TIsFlatSerializable<TVec<TInt> >::Val));
    EXPECT_EQ(0, (int)(TIsFlatSerializable<TVec<TVec<TInt> > >::Val));

    TVec<TVec<TInt> > Src;
    for (int VecN = 0; VecN < 100; VecN++) {
        TVec<TInt> Inner;
        const int InnerLen = (VecN % 7 == 0) ? 0 : ((VecN * 37) % 1000); // some empty
        for (int N = 0; N < InnerLen; N++) { Inner.Add(VecN * 100000 + N); }
        Src.Add(Inner);
    }
    // one inner vector with > 1MB of payload
    TVec<TInt> BigInner;
    for (int N = 0; N < 300000; N++) { BigInner.Add(N ^ 0x55AA55AA); }
    Src.Add(BigInner);

    TVec<TVec<TInt> > Dest;
    FileRoundTrip(Src, Dest, FastSerMoreFPath + "NestedVV.bin");
    ASSERT_EQ(Src.Len(), Dest.Len());
    for (int VecN = 0; VecN < Src.Len(); VecN++) {
        ASSERT_TRUE(Src[VecN] == Dest[VecN]) << "inner vector " << VecN << " differs";
    }
}

// NaN, +/-infinity, signed zero and denormals do not survive naive value-based
// copying (NaN != NaN); the bulk memcpy path must reproduce them bit-exact
TEST(QmFastSerMoreTests, NonFiniteDoublesRoundTripBitExact)
{
    TVec<TFlt> Src;
    Src.Add(std::numeric_limits<double>::quiet_NaN());
    Src.Add(TFlt::PInf);
    Src.Add(TFlt::NInf);
    Src.Add(0.0);
    Src.Add(-0.0);
    Src.Add(std::numeric_limits<double>::denorm_min());
    Src.Add(-std::numeric_limits<double>::denorm_min());
    Src.Add(TFlt::Mx);
    Src.Add(TFlt::Mn);
    Src.Add(3.14159265358979);

    TMOut MOut; Src.Save(MOut);
    TMIn MIn(MOut.GetBfAddr(), MOut.Len(), false);
    TVec<TFlt> Dest; Dest.Load(MIn);
    ASSERT_EQ(Src.Len(), Dest.Len());
    // compare raw bit patterns - NaN would fail an operator== comparison
    EXPECT_EQ(0, memcmp(&Src[0], &Dest[0], (size_t)Src.Len() * sizeof(TFlt)));
    EXPECT_TRUE(TFlt::IsNan(Dest[0]));
    // -0.0 must keep its sign bit (1/-0.0 == -inf)
    EXPECT_TRUE(Dest[4].Val == 0.0 && 1.0 / Dest[4].Val < 0.0) << "-0.0 lost its sign bit";
}

// a hash with deleted keys keeps holes (free list) in its internal KeyDatV; the
// bulk-entry save/load must reproduce the holes so lookups, misses and further
// inserts keep working after a reload
TEST(QmFastSerMoreTests, HashWithDeletedEntriesRoundTrips)
{
    PrepareTestDir();
    THash<TInt, TInt> Src; // THashKeyDat<TInt,TInt> is flat -> bulk path
    const int Keys = 30000;
    for (int KeyN = 0; KeyN < Keys; KeyN++) { Src.AddDat(KeyN) = KeyN * 3 + 1; }
    for (int KeyN = 0; KeyN < Keys; KeyN += 3) { Src.DelKey(KeyN); } // punch holes
    const int ExpLen = Src.Len();

    THash<TInt, TInt> Dest;
    FileRoundTrip(Src, Dest, FastSerMoreFPath + "HoleyH.bin");
    ASSERT_EQ(ExpLen, Dest.Len());
    for (int KeyN = 0; KeyN < Keys; KeyN++) {
        if (KeyN % 3 == 0) {
            EXPECT_FALSE(Dest.IsKey(KeyN)) << "deleted key " << KeyN << " resurfaced";
        } else {
            ASSERT_TRUE(Dest.IsKey(KeyN)) << "key " << KeyN << " lost";
            EXPECT_EQ(KeyN * 3 + 1, Dest.GetDat(KeyN).Val);
        }
    }
    // the loaded hash must accept new keys (free list intact)
    Dest.AddDat(Keys + 1) = 42;
    EXPECT_EQ(42, Dest.GetDat(Keys + 1).Val);
}
