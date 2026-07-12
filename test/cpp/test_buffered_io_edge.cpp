/**
 * Edge-case tests for the buffered TFIn/TFOut IO paths in src/glib/base/fl.cpp
 * after the internal buffer was enlarged from 16 KB to 1 MB and GetBf/PutBf were
 * rewritten to chunked memcpy across buffer refills/flushes.
 *
 * These complement QmFlEdgeTests (which covers the checksum primitive and single
 * PutBf/GetBf calls around the 1 MB boundary) by targeting stream *sequences*:
 *   - empty files (Eof, length)
 *   - files whose length is an exact multiple of the 1 MB buffer, where Eof()
 *     itself has to trigger the final zero-byte FillBf
 *   - interleaved small/large writes and reads with mismatched chunking, so
 *     individual calls start and end at arbitrary offsets within the buffer
 *   - TStr values that straddle the buffer boundary
 *   - explicit Flush() calls in the middle of a write sequence
 */

#include <base.h>
#include <mine.h>
#include "gtest/gtest.h"

namespace {

const TStr BufIoTestFPath = "./qm_bufio_test/";
const int OneMB = 1024 * 1024; // == TFIn::MxBfL == TFOut::MxBfL

void PrepareTestDir()
{
    if (!TDir::Exists(BufIoTestFPath)) { TDir::GenDir(BufIoTestFPath); }
}

// deterministic pseudo-random byte pattern (includes high-bit bytes)
char PatternByte(const int& N)
{
    return (char)((N * 131 + (N >> 7) * 31 + 7) & 0xFF);
}

TVec<char> MakePattern(const int& Len)
{
    TVec<char> Buf(Len);
    for (int N = 0; N < Len; N++) { Buf[N] = PatternByte(N); }
    return Buf;
}

// read the complete file with a single GetBf and compare it to the expected bytes
void CheckFileBytes(const TStr& FNm, const TVec<char>& Exp)
{
    TFIn FIn(FNm);
    ASSERT_EQ(Exp.Len(), FIn.GetFLen());
    if (Exp.Len() > 0) {
        TVec<char> Got(Exp.Len());
        FIn.GetBf(&Got[0], Exp.Len());
        ASSERT_EQ(0, memcmp(&Exp[0], &Got[0], Exp.Len()));
    }
    EXPECT_TRUE(FIn.Eof());
}

} // namespace

// a file with no writes at all must open, report zero length and immediate Eof
TEST(QmBufferedIoEdgeTests, EmptyFileReportsEofAndZeroLength)
{
    PrepareTestDir();
    const TStr FNm = BufIoTestFPath + "empty.bin";
    { TFOut FOut(FNm); } // no writes
    ASSERT_TRUE(TFile::Exists(FNm));
    TFIn FIn(FNm);
    EXPECT_EQ(0, FIn.GetFLen());
    EXPECT_EQ(0, FIn.Len());
    EXPECT_TRUE(FIn.Eof());
}

// when the file length is an exact multiple of the 1 MB buffer, the last
// FillBf fills the buffer completely and Eof() has to detect the end by
// issuing one more (zero byte) FillBf; sizes 1 MB and 2 MB hit this, the
// +/- 1 sizes guard the neighboring off-by-one cases
TEST(QmBufferedIoEdgeTests, ExactBufferMultipleFilesReadBackAndHitEof)
{
    PrepareTestDir();
    const int Lens[] = { OneMB - 1, OneMB, OneMB + 1, 2 * OneMB };
    for (int LenN = 0; LenN < (int)(sizeof(Lens) / sizeof(int)); LenN++) {
        const int Len = Lens[LenN];
        const TStr FNm = BufIoTestFPath + TStr::Fmt("exact_%d.bin", Len);
        TVec<char> Exp = MakePattern(Len);
        { TFOut FOut(FNm); FOut.PutBf(&Exp[0], Len); }
        // bulk read; Eof checked inside
        CheckFileBytes(FNm, Exp);
        // byte-by-byte read must consume exactly Len bytes before Eof
        TFIn FIn(FNm);
        int Bytes = 0;
        while (!FIn.Eof()) {
            const char Ch = FIn.GetCh();
            ASSERT_EQ(Exp[Bytes], Ch) << "byte mismatch at " << Bytes << " for length " << Len;
            Bytes++;
        }
        EXPECT_EQ(Len, Bytes) << "wrong byte count via GetCh at length " << Len;
    }
}

// interleave single-character writes with small and >1MB block writes, so
// individual PutBf calls start at arbitrary in-buffer offsets and straddle one
// or more flushes; the file must come out byte-identical to the same bytes
// written in one call
TEST(QmBufferedIoEdgeTests, InterleavedSmallAndLargeWritesRoundTrip)
{
    PrepareTestDir();
    const TStr FNm = BufIoTestFPath + "interleaved.bin";
    // sizes are relatively prime to the buffer size, and one exceeds it
    const int PieceLens[] = { 1, 3, 17, 65537, 1, 700001, 13, OneMB + 7, 255, 524287, 1 };
    int Total = 0;
    for (int PieceN = 0; PieceN < (int)(sizeof(PieceLens) / sizeof(int)); PieceN++) { Total += PieceLens[PieceN]; }
    TVec<char> Exp = MakePattern(Total);
    {
        TFOut FOut(FNm);
        int Off = 0;
        for (int PieceN = 0; PieceN < (int)(sizeof(PieceLens) / sizeof(int)); PieceN++) {
            const int PieceLen = PieceLens[PieceN];
            if (PieceLen == 1) {
                FOut.PutCh(Exp[Off]); // single-char path
            } else {
                FOut.PutBf(&Exp[Off], PieceLen);
            }
            Off += PieceLen;
        }
        ASSERT_EQ(Total, Off);
    }
    CheckFileBytes(FNm, Exp);
}

// write in one big call, read back in chunks whose size does not divide the
// buffer size, so every refill boundary is crossed mid-GetBf; then repeat with
// a chunk size just under the buffer size
TEST(QmBufferedIoEdgeTests, ReadChunksSpanningRefillBoundaries)
{
    PrepareTestDir();
    const TStr FNm = BufIoTestFPath + "chunked_read.bin";
    const int Total = 2 * OneMB + 500000 + 13;
    TVec<char> Exp = MakePattern(Total);
    { TFOut FOut(FNm); FOut.PutBf(&Exp[0], Total); }

    const int ChunkLens[] = { 65537, OneMB - 1, OneMB + 1 };
    for (int ChunkN = 0; ChunkN < (int)(sizeof(ChunkLens) / sizeof(int)); ChunkN++) {
        const int ChunkLen = ChunkLens[ChunkN];
        TFIn FIn(FNm);
        TVec<char> Got(Total);
        int Off = 0;
        while (Off < Total) {
            const int Len = MIN(ChunkLen, Total - Off);
            FIn.GetBf(&Got[Off], Len);
            Off += Len;
        }
        EXPECT_TRUE(FIn.Eof());
        ASSERT_EQ(0, memcmp(&Exp[0], &Got[0], Total))
            << "content mismatch when reading in chunks of " << ChunkLen;
    }
}

// TStr values large enough that their character data straddles the 1 MB flush
// and refill boundaries, mixed with tiny and empty strings; each string (and a
// trailing marker) must round-trip identically
TEST(QmBufferedIoEdgeTests, StringsCrossingBufferBoundaryRoundTrip)
{
    PrepareTestDir();
    const TStr FNm = BufIoTestFPath + "strings.bin";
    TVec<TStr> SrcV;
    // ~5 x 300 KB pushes the stream across several 1 MB boundaries
    const int StrLens[] = { 300000, 0, 1, 300000, 7, 300000, 300000, 12345, 300000 };
    for (int StrN = 0; StrN < (int)(sizeof(StrLens) / sizeof(int)); StrN++) {
        const int StrLen = StrLens[StrN];
        TChA ChA;
        for (int ChN = 0; ChN < StrLen; ChN++) {
            ChA += (char)('a' + ((ChN + StrN * 31) % 26));
        }
        SrcV.Add(TStr(ChA));
    }
    const int Marker = 0x5A5A1234;
    {
        TFOut FOut(FNm);
        for (int StrN = 0; StrN < SrcV.Len(); StrN++) { SrcV[StrN].Save(FOut); }
        TInt(Marker).Save(FOut);
    }
    {
        TFIn FIn(FNm);
        for (int StrN = 0; StrN < SrcV.Len(); StrN++) {
            TStr Str(FIn);
            ASSERT_EQ(SrcV[StrN].Len(), Str.Len()) << "string " << StrN << " length differs";
            EXPECT_TRUE(SrcV[StrN] == Str) << "string " << StrN << " content differs";
        }
        TInt MarkerBack(FIn);
        EXPECT_EQ(Marker, MarkerBack.Val);
        EXPECT_TRUE(FIn.Eof());
    }
}

// explicit Flush() calls between writes (as done by long-running services)
// must not corrupt, duplicate or drop any bytes, wherever they land relative
// to the internal buffer state
TEST(QmBufferedIoEdgeTests, FlushMidStreamKeepsStreamByteIdentical)
{
    PrepareTestDir();
    const TStr FNm = BufIoTestFPath + "flushed.bin";
    const int PieceLen = 300007; // prime, so flush points drift over the buffer
    const int Pieces = 10;       // ~3 MB total
    TVec<char> Exp = MakePattern(PieceLen * Pieces);
    {
        TFOut FOut(FNm);
        for (int PieceN = 0; PieceN < Pieces; PieceN++) {
            FOut.PutBf(&Exp[PieceN * PieceLen], PieceLen);
            if (PieceN % 2 == 0) { FOut.Flush(); } // flush with a partially filled buffer
        }
        FOut.Flush(); // double flush at the end must also be harmless
    }
    CheckFileBytes(FNm, Exp);
}
