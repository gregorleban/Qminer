/**
 * Edge-case / bug-discovery tests for the bulk serialization fast paths in
 * src/glib/base/fl.cpp (branch fast-load-save).
 *
 * These complement QmFastSerializationTests by targeting the parts of the new
 * code that random round-trips do not deterministically exercise:
 *   - the SSE2 byte-sum checksum at and around the 16-byte block boundary
 *   - adversarial byte patterns where the signed<->unsigned correction
 *     (UnsignedSum - 256*HighBitCount) is most likely to be wrong
 *   - the TMIn::GetBfMemCpy position fix (copy from Bf+BfC, not Bf)
 *   - TFOut/TFIn PutBf/GetBf crossing the enlarged 1 MB buffer
 */

#include <base.h>
#include <mine.h>
#include "gtest/gtest.h"

namespace {

// the historical checksum: sign-extended byte-at-a-time TCs accumulation.
// TCs::GetCsFromBf must reproduce this exactly, because the value is written
// into the stream and validated on load.
TCs ReferenceCs(const char* Bf, const int& BfL) {
    TCs Cs;
    for (int BfC = 0; BfC < BfL; BfC++) { Cs += Bf[BfC]; }
    return Cs;
}

// the interesting lengths: below the SSE2 block (16), exactly on it, one past
// it, and around multiples so the 16-byte main loop + scalar tail split is hit
// with every possible remainder
const int BoundaryLens[] = {
    0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 18, 23, 24, 31, 32, 33,
    47, 48, 49, 63, 64, 65, 127, 128, 129, 255, 256, 257, 1000
};

void CheckPattern(const TStr& Name, const TVec<char>& Buf) {
    const char* Bf = Buf.Len() > 0 ? &Buf[0] : (const char*)"";
    const TCs Ref = ReferenceCs(Bf, Buf.Len());
    const TCs Fast = TCs::GetCsFromBf((char*)Bf, Buf.Len());
    EXPECT_TRUE(Ref == Fast)
        << "checksum mismatch for pattern '" << Name.CStr()
        << "' at length " << Buf.Len();
}

TVec<char> FilledBuf(const int& Len, const unsigned char& Val) {
    TVec<char> Buf(Len, 0);
    for (int i = 0; i < Len; i++) { Buf.Add((char)Val); }
    return Buf;
}

} // namespace

// all-high-bit buffers are the worst case for the SSE2 path, which sums the
// UNSIGNED byte values and then subtracts 256 for every byte with the high bit
// set; an off-by-one in that correction only shows up when many/all bytes are
// >= 0x80. random data (as in QmFastSerializationTests) averages this out.
TEST(QmFlEdgeTests, ChecksumMatchesForHighBitPatternsAcrossBoundaryLengths) {
    const unsigned char Vals[] = { 0x00, 0x01, 0x7F, 0x80, 0x81, 0xFF };
    for (int LenN = 0; LenN < (int)(sizeof(BoundaryLens) / sizeof(int)); LenN++) {
        const int Len = BoundaryLens[LenN];
        for (int ValN = 0; ValN < (int)(sizeof(Vals)); ValN++) {
            CheckPattern(TStr::Fmt("fill-0x%02X", Vals[ValN]), FilledBuf(Len, Vals[ValN]));
        }
    }
}

// a single high-bit byte placed at the start, in the SSE2 body, in the scalar
// tail, and at the very end - isolates whether the high-bit correction counts
// the right bytes regardless of position
TEST(QmFlEdgeTests, ChecksumMatchesForSingleHighBitByteAtEveryPosition) {
    for (int LenN = 0; LenN < (int)(sizeof(BoundaryLens) / sizeof(int)); LenN++) {
        const int Len = BoundaryLens[LenN];
        if (Len == 0) { continue; }
        for (int Pos = 0; Pos < Len; Pos++) {
            TVec<char> Buf = FilledBuf(Len, 0x00);
            Buf[Pos] = (char)0x80;
            CheckPattern(TStr::Fmt("single-0x80@%d", Pos), Buf);
        }
    }
}

// alternating and structured patterns that a vectorized sum could reorder or
// mis-mask, at boundary lengths
TEST(QmFlEdgeTests, ChecksumMatchesForAlternatingAndRampPatterns) {
    for (int LenN = 0; LenN < (int)(sizeof(BoundaryLens) / sizeof(int)); LenN++) {
        const int Len = BoundaryLens[LenN];
        TVec<char> Alt(Len, 0), Ramp(Len, 0);
        for (int i = 0; i < Len; i++) {
            Alt.Add((char)((i % 2 == 0) ? 0x7F : 0x80));
            Ramp.Add((char)(i & 0xFF)); // 0..255 ramp, wraps
        }
        CheckPattern("alt-7F-80", Alt);
        CheckPattern("ramp", Ramp);
    }
}

// the empty buffer must produce the neutral checksum (0), same as the historical
// loop that never executes
TEST(QmFlEdgeTests, ChecksumOfEmptyBufferEqualsReference) {
    char Dummy = 0;
    const TCs Fast = TCs::GetCsFromBf(&Dummy, 0);
    EXPECT_TRUE(TCs() == Fast);
    EXPECT_TRUE(ReferenceCs(&Dummy, 0) == Fast);
}

// regression for the TMIn::GetBfMemCpy fix: it used to copy from the start of
// the buffer (Bf) instead of the current position (Bf + BfC), so any read
// before a bulk copy returned wrong data. reproduce by reading a field first,
// then loading a flat vector (which uses the bulk memcpy path) from a non-zero
// stream position.
TEST(QmFlEdgeTests, MemCpyLoadHonorsStreamPositionAfterPriorRead) {
    TMOut MOut;
    // something read before the vector, so BfC is well past 0 at the bulk copy
    const int Marker = 0x11223344;
    TInt(Marker).Save(MOut);
    TVec<TInt> Src;
    for (int i = 0; i < 5000; i++) { Src.Add(i * 7 + 3); }
    Src.Save(MOut); // flat type -> bulk (memcpy) save path

    TMIn MIn(MOut.GetBfAddr(), MOut.Len(), false);
    TInt MarkerBack(MIn);
    ASSERT_EQ(Marker, MarkerBack.Val); // advances the read position
    TVec<TInt> Dest;
    Dest.Load(MIn); // bulk (memcpy) load from BfC > 0
    ASSERT_EQ(Src.Len(), Dest.Len());
    for (int i = 0; i < Src.Len(); i++) {
        ASSERT_EQ(Src[i].Val, Dest[i].Val) << "corrupted at index " << i;
    }
    // the whole stream must have been consumed exactly
    ASSERT_TRUE(MIn.Eof());
}

namespace {

const TStr FlEdgeTestFPath = "./qm_fledge_test/";

// raw PutBf/GetBf round-trip of a byte buffer of a given length, comparing
// contents byte-for-byte
void CheckFileByteRoundTrip(const int& Len) {
    if (!TDir::Exists(FlEdgeTestFPath)) { TDir::GenDir(FlEdgeTestFPath); }
    TVec<char> Src(Len == 0 ? 1 : Len, 0);
    for (int i = 0; i < Len; i++) { Src.Add((char)((i * 31 + 7) & 0xFF)); } // includes high-bit bytes
    const TStr FNm = FlEdgeTestFPath + TStr::Fmt("bytes_%d.bin", Len);
    { TFOut FOut(FNm); if (Len > 0) { FOut.PutBf(&Src[0], Len); } }
    TVec<char> Dst(Len == 0 ? 1 : Len, 0);
    for (int i = 0; i < Len; i++) { Dst.Add((char)0); }
    { TFIn FIn(FNm); if (Len > 0) { FIn.GetBf(&Dst[0], Len); } }
    for (int i = 0; i < Len; i++) {
        ASSERT_EQ(Src[i], Dst[i]) << "byte mismatch at " << i << " for length " << Len;
    }
}

} // namespace

// TFIn/TFOut buffers grew from 16 KB to 1 MB; PutBf/GetBf now memcpy in chunks
// across buffer refills/flushes. exercise exactly around the 1 MB boundary and
// a couple of megabytes, with odd lengths, to catch off-by-one chunking bugs.
TEST(QmFlEdgeTests, FilePutGetBfCrossesOneMegabyteBufferBoundary) {
    const int OneMB = 1024 * 1024;
    const int Lens[] = {
        0, 1, 15, 16, 17,
        OneMB - 1, OneMB, OneMB + 1,
        2 * OneMB + 13, 3 * OneMB
    };
    for (int LenN = 0; LenN < (int)(sizeof(Lens) / sizeof(int)); LenN++) {
        CheckFileByteRoundTrip(Lens[LenN]);
    }
}
