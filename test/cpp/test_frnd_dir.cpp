/* Copyright (C) Event Registry d.o.o. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 */

#include <base.h>

#include "gtest/gtest.h"

// Tests for the TFRnd read/write direction tracking (fl.cpp): GetBf/PutBf only
// issue the C-stdio-mandated fseek when the direction actually switches, instead
// of before every operation. The seek is required by the C standard between a
// read and a write on an update stream (and vice versa); skipping it on
// same-direction sequences restores FILE* buffering. These tests pin down that
// every switch pattern still reads and writes the right bytes at the right
// offsets - a missed seek would corrupt data or read stale bytes.

namespace {

const TStr FrndDir = "./test/cpp/files/";

TStr FrndFNm(const TStr& Nm) { return FrndDir + Nm; }

// read the complete file back through a fresh handle
void ReadWholeFile(const TStr& FNm, TVec<char>& BfV) {
    TFRnd FRnd(FNm, faRdOnly, false);
    const int FLen = FRnd.GetFLen();
    BfV.Gen(FLen);
    FRnd.SetFPos(0);
    if (FLen > 0) { FRnd.GetBf(BfV.BegI(), FLen); }
}

} // namespace

// consecutive writes (no interleaved seeks), then consecutive reads
TEST(FRndDirTests, SequentialWriteThenRead)
{
    const TStr FNm = FrndFNm("frnd_seq.bin");
    {
        TFRnd FRnd(FNm, faCreate);
        for (int N = 0; N < 256; N++) { FRnd.PutCh((char) N); }
        // write -> read switch without an explicit seek in between
        FRnd.SetFPos(0);
        for (int N = 0; N < 256; N++) { ASSERT_EQ((char) N, FRnd.GetCh()); }
    }
    TVec<char> BfV; ReadWholeFile(FNm, BfV);
    ASSERT_EQ(BfV.Len(), 256);
    for (int N = 0; N < 256; N++) { ASSERT_EQ((char) N, BfV[N]); }
    TFile::Del(FNm, false);
}

// read -> write switch with NO explicit seek: the write must land exactly at
// the position where the read stopped
TEST(FRndDirTests, ReadThenWriteNoSeek)
{
    const TStr FNm = FrndFNm("frnd_rw_switch.bin");
    {
        TFRnd FRnd(FNm, faCreate);
        const char* Bytes = "AAAABBBBCCCC";
        FRnd.PutBf(Bytes, 12);
        FRnd.SetFPos(0);
        char Bf[4];
        FRnd.GetBf(Bf, 4);                      // reads AAAA, position now 4
        ASSERT_EQ(0, memcmp(Bf, "AAAA", 4));
        FRnd.PutBf("XXXX", 4);                  // read -> write switch, must overwrite BBBB
        FRnd.GetBf(Bf, 4);                      // write -> read switch, must read CCCC
        ASSERT_EQ(0, memcmp(Bf, "CCCC", 4));
    }
    TVec<char> BfV; ReadWholeFile(FNm, BfV);
    ASSERT_EQ(0, memcmp(BfV.BegI(), "AAAAXXXXCCCC", 12));
    TFile::Del(FNm, false);
}

// explicit seeks between same-direction operations (the SetFPos/MoveFPos reset
// path): header rewrites in the middle of record appends, like TBlobBs does
TEST(FRndDirTests, HeaderRewriteBetweenAppends)
{
    const TStr FNm = FrndFNm("frnd_hdr.bin");
    {
        TFRnd FRnd(FNm, faCreate);
        int Hd = 0;
        FRnd.PutBf(&Hd, sizeof(int));           // header placeholder
        for (int RecN = 0; RecN < 100; RecN++) {
            FRnd.SetFPos(sizeof(int) + RecN * sizeof(int));
            FRnd.PutBf(&RecN, sizeof(int));     // append record
            FRnd.SetFPos(0);
            Hd = RecN + 1;
            FRnd.PutBf(&Hd, sizeof(int));       // rewrite header (write after seek)
        }
    }
    {
        TFRnd FRnd(FNm, faRdOnly, false);
        int Val;
        FRnd.GetBf(&Val, sizeof(int));
        ASSERT_EQ(Val, 100);                    // final header value
        for (int RecN = 0; RecN < 100; RecN++) {
            FRnd.GetBf(&Val, sizeof(int));      // consecutive reads, no seeks
            ASSERT_EQ(Val, RecN);
        }
    }
    TFile::Del(FNm, false);
}

// randomized read/write/seek sequences checked against an in-memory shadow copy;
// deterministic seed so a failure reproduces
TEST(FRndDirTests, RandomOpsMatchShadow)
{
    const TStr FNm = FrndFNm("frnd_shadow.bin");
    const int FileLen = 4096;
    TVec<char> Shadow(FileLen);
    {
        TFRnd FRnd(FNm, faCreate);
        // initial content
        for (int N = 0; N < FileLen; N++) { Shadow[N] = (char) (N * 7 + 13); }
        FRnd.PutBf(Shadow.BegI(), FileLen);

        TRnd Rnd(42);
        char Bf[64];
        for (int OpN = 0; OpN < 20000; OpN++) {
            const int Op = Rnd.GetUniDevInt(10);
            if (Op == 0) {
                // explicit reposition
                FRnd.SetFPos(Rnd.GetUniDevInt(FileLen - 64));
            } else if (Op < 6) {
                // read where the handle currently stands (clamped to the file end)
                int FPos = FRnd.GetFPos();
                if (FPos > FileLen - 64) { FRnd.SetFPos(FPos = Rnd.GetUniDevInt(FileLen - 64)); }
                const int Len = 1 + Rnd.GetUniDevInt(64);
                FRnd.GetBf(Bf, Len);
                for (int N = 0; N < Len; N++) {
                    ASSERT_EQ(Shadow[FPos + N], Bf[N]) << "read mismatch at op " << OpN;
                }
            } else {
                // write where the handle currently stands (clamped to the file end)
                int FPos = FRnd.GetFPos();
                if (FPos > FileLen - 64) { FRnd.SetFPos(FPos = Rnd.GetUniDevInt(FileLen - 64)); }
                const int Len = 1 + Rnd.GetUniDevInt(64);
                for (int N = 0; N < Len; N++) { Bf[N] = (char) Rnd.GetUniDevInt(256); }
                FRnd.PutBf(Bf, Len);
                for (int N = 0; N < Len; N++) { Shadow[FPos + N] = Bf[N]; }
            }
        }
    }
    // the persisted file must equal the shadow byte for byte
    TVec<char> BfV; ReadWholeFile(FNm, BfV);
    ASSERT_EQ(BfV.Len(), FileLen);
    for (int N = 0; N < FileLen; N++) { ASSERT_EQ(Shadow[N], BfV[N]) << "file mismatch at " << N; }
    TFile::Del(FNm, false);
}

// Flush in the middle of buffered writes must not disturb positions or content
TEST(FRndDirTests, FlushMidStream)
{
    const TStr FNm = FrndFNm("frnd_flush.bin");
    {
        TFRnd FRnd(FNm, faCreate);
        FRnd.PutBf("12345678", 8);
        FRnd.Flush();
        FRnd.PutBf("abcdefgh", 8);              // continues at position 8
        FRnd.SetFPos(4);
        char Bf[8];
        FRnd.GetBf(Bf, 8);
        ASSERT_EQ(0, memcmp(Bf, "5678abcd", 8));
    }
    TFile::Del(FNm, false);
}
