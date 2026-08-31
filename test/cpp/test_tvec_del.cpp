/* Copyright (C) Event Registry d.o.o. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 */

#include <base.h>
#include "gtest/gtest.h"

// Tests for the TVec::Del fast path (ds.hpp): elements of a bitwise-movable type
// (TIsBitwiseMovable in ds.h) are shifted with a single memmove instead of the
// element-at-a-time Move loop. The trait must opt in exactly the types whose bytes
// fully define them - a wrong opt-in would memmove over an owning type and cause
// double destruction - and the memmove itself must reproduce the loop's semantics
// at every position, including the edges where the memmove is skipped entirely
// (deleting the last element / a range ending at the last element).

namespace {

// delete one element and compare against a manually rebuilt expectation
template <class TVal>
void CheckDelSingle(const TVec<TVal>& Src, const int& ValN)
{
    TVec<TVal> Expect;
    for (int N = 0; N < Src.Len(); N++) {
        if (N != ValN) { Expect.Add(Src[N]); }
    }
    TVec<TVal> Got = Src;
    Got.Del(ValN);
    ASSERT_EQ(Expect.Len(), Got.Len()) << "Del(" << ValN << ") of " << Src.Len();
    for (int N = 0; N < Expect.Len(); N++) {
        EXPECT_TRUE(Expect[N] == Got[N]) << "mismatch at " << N << " after Del(" << ValN << ")";
    }
}

// delete an inclusive range and compare against a manually rebuilt expectation
template <class TVal>
void CheckDelRange(const TVec<TVal>& Src, const int& MnValN, const int& MxValN)
{
    TVec<TVal> Expect;
    for (int N = 0; N < Src.Len(); N++) {
        if (N < MnValN || N > MxValN) { Expect.Add(Src[N]); }
    }
    TVec<TVal> Got = Src;
    Got.Del(MnValN, MxValN);
    ASSERT_EQ(Expect.Len(), Got.Len()) << "Del(" << MnValN << "," << MxValN << ") of " << Src.Len();
    for (int N = 0; N < Expect.Len(); N++) {
        EXPECT_TRUE(Expect[N] == Got[N])
            << "mismatch at " << N << " after Del(" << MnValN << "," << MxValN << ")";
    }
}

} // namespace

// the trait must be true for glib's flat-serializable value types (their user-provided
// operator= makes them non-trivially-copyable in the std sense, so they are picked up
// via the TIsFlatSerializable opt-in) and for plain trivially-copyable types, and false
// for anything owning heap memory
TEST(TVecDelTests, BitwiseMovableTraitOptIns)
{
    // glib number wrappers and the gix item/key types - all shifted with memmove
    EXPECT_EQ(1, (int)TIsBitwiseMovable<TCh>::Val);
    EXPECT_EQ(1, (int)TIsBitwiseMovable<TInt>::Val);
    EXPECT_EQ(1, (int)TIsBitwiseMovable<TUInt>::Val);
    EXPECT_EQ(1, (int)TIsBitwiseMovable<TUInt64>::Val);
    EXPECT_EQ(1, (int)TIsBitwiseMovable<TFlt>::Val);
    EXPECT_EQ(1, (int)(TIsBitwiseMovable<TKeyDat<TUInt64, TInt> >::Val)); // TQmGixItemFull
    EXPECT_EQ(1, (int)(TIsBitwiseMovable<TKeyDat<TUInt, TSInt> >::Val));  // TQmGixItemSmall
    EXPECT_EQ(1, (int)(TIsBitwiseMovable<TIntUInt64Pr>::Val));            // TQmGixKey
    // plain fundamental types come in through std::is_trivially_copyable
    EXPECT_EQ(1, (int)TIsBitwiseMovable<int>::Val);
    EXPECT_EQ(1, (int)TIsBitwiseMovable<double>::Val);
    // owning types MUST stay on the element-wise Move loop
    EXPECT_EQ(0, (int)TIsBitwiseMovable<TStr>::Val);
    EXPECT_EQ(0, (int)(TIsBitwiseMovable<TVec<TInt> >::Val));
}

// Del(ValN) on a movable type at every position, including both memmove edge cases:
// ValN == 0 (largest shift) and ValN == Len-1 (no memmove at all, only the tail reset)
TEST(TVecDelTests, DelSingleMovableEveryPosition)
{
    TVec<TInt> Src;
    for (int N = 0; N < 10; N++) { Src.Add(100 + N); }
    for (int ValN = 0; ValN < Src.Len(); ValN++) {
        CheckDelSingle(Src, ValN);
    }
    // single-element vector -> empty
    TVec<TInt> One; One.Add(42);
    CheckDelSingle(One, 0);
}

// Del(Mn, Mx) on a movable type over every valid inclusive span, including
// Mx == Len-1 (memmove skipped), the whole vector, and single-element spans
TEST(TVecDelTests, DelRangeMovableAllSpans)
{
    TVec<TInt> Src;
    for (int N = 0; N < 12; N++) { Src.Add(1000 + 7 * N); }
    for (int Mn = 0; Mn < Src.Len(); Mn++) {
        for (int Mx = Mn; Mx < Src.Len(); Mx++) {
            CheckDelRange(Src, Mn, Mx);
        }
    }
}

// the 12-byte gix full item (8-byte key + 4-byte dat) through both Del overloads -
// the type the gix work buffers actually shift
TEST(TVecDelTests, DelMovableKeyDatItems)
{
    TVec<TKeyDat<TUInt64, TInt> > Src;
    for (int N = 0; N < 9; N++) { Src.Add(TKeyDat<TUInt64, TInt>(uint64(N) * 1000003ULL, N % 5)); }
    for (int ValN = 0; ValN < Src.Len(); ValN++) {
        CheckDelSingle(Src, ValN);
    }
    CheckDelRange(Src, 0, 3);
    CheckDelRange(Src, 2, 6);
    CheckDelRange(Src, 5, 8); // range ending at the last element - no memmove
    CheckDelRange(Src, 0, 8); // whole vector
}

// non-movable types must keep the element-wise Move loop: heap-owning strings and
// nested vectors survive both Del overloads with contents intact (a wrong memmove
// here would double-destroy - the CRT leak checker in run-all-tests would flag it)
TEST(TVecDelTests, DelNonMovableOwningTypes)
{
    TVec<TStr> StrV;
    for (int N = 0; N < 8; N++) {
        StrV.Add(TStr::Fmt("heap-allocated-string-long-enough-%d", N));
    }
    for (int ValN = 0; ValN < StrV.Len(); ValN++) {
        CheckDelSingle(StrV, ValN);
    }
    CheckDelRange(StrV, 0, 2);
    CheckDelRange(StrV, 3, 7);
    CheckDelRange(StrV, 0, 7);

    TVec<TVec<TInt> > NestV;
    for (int N = 0; N < 6; N++) {
        TVec<TInt> Inner;
        for (int M = 0; M <= N; M++) { Inner.Add(N * 10 + M); }
        NestV.Add(Inner);
    }
    CheckDelSingle(NestV, 0);
    CheckDelSingle(NestV, 5);
    CheckDelRange(NestV, 1, 3);
}

// randomized cross-check of both overloads against the rebuilt expectation
TEST(TVecDelTests, DelRandomizedAgainstReference)
{
    TRnd Rnd(1);
    for (int RunN = 0; RunN < 300; RunN++) {
        const int Len = Rnd.GetUniDevInt(1, 60);
        TVec<TInt> Src;
        for (int N = 0; N < Len; N++) { Src.Add(Rnd.GetUniDevInt(0, 1000)); }
        if (Rnd.GetUniDevInt(0, 1) == 0) {
            CheckDelSingle(Src, Rnd.GetUniDevInt(0, Len - 1));
        } else {
            const int Mn = Rnd.GetUniDevInt(0, Len - 1);
            const int Mx = Rnd.GetUniDevInt(Mn, Len - 1);
            CheckDelRange(Src, Mn, Mx);
        }
    }
}

// repeated deletes on the SAME vector until it is empty - catches state corruption
// that only shows up across successive shifts (stale tail slots, wrong Vals)
TEST(TVecDelTests, DelChainedUntilEmpty)
{
    TRnd Rnd(7);
    TVec<TInt> Vec;
    for (int N = 0; N < 100; N++) { Vec.Add(N); }
    while (Vec.Len() > 0) {
        const TVec<TInt> Before = Vec;
        if (Rnd.GetUniDevInt(0, 1) == 0 || Vec.Len() == 1) {
            const int ValN = Rnd.GetUniDevInt(0, Vec.Len() - 1);
            Vec.Del(ValN);
            ASSERT_EQ(Before.Len() - 1, Vec.Len());
            for (int N = 0; N < Vec.Len(); N++) {
                ASSERT_EQ((int)Before[N < ValN ? N : N + 1], (int)Vec[N]);
            }
        } else {
            const int Mn = Rnd.GetUniDevInt(0, Vec.Len() - 1);
            const int Mx = Rnd.GetUniDevInt(Mn, Vec.Len() - 1);
            Vec.Del(Mn, Mx);
            ASSERT_EQ(Before.Len() - (Mx - Mn + 1), Vec.Len());
            for (int N = 0; N < Vec.Len(); N++) {
                ASSERT_EQ((int)Before[N < Mn ? N : N + Mx - Mn + 1], (int)Vec[N]);
            }
        }
    }
}
