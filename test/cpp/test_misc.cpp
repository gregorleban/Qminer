#include <base.h>
#include <mine.h>
#include <qminer.h>

#include "gtest/gtest.h"

TEST(Misc, TDirExists) {
    ASSERT_TRUE(TDir::Exists("./test/cpp")); // here we assume that tests are always ran from root dir
    ASSERT_FALSE(TDir::Exists("./xyz"));
}


TEST(Misc, TMathFloorLog2_uint) {
    ASSERT_EQ(TMath::FloorLog2((uint)1), 0);
    ASSERT_EQ(TMath::FloorLog2((uint)2), 1);
    ASSERT_EQ(TMath::FloorLog2((uint)3), 1);
    ASSERT_EQ(TMath::FloorLog2((uint)4), 2);
    ASSERT_EQ(TMath::FloorLog2((uint)5), 2);
    ASSERT_EQ(TMath::FloorLog2((uint)6), 2);
    ASSERT_EQ(TMath::FloorLog2((uint)7), 2);
    ASSERT_EQ(TMath::FloorLog2((uint)8), 3);
    ASSERT_EQ(TMath::FloorLog2((uint)TMath::Pow2(18) - 1), 17);
    ASSERT_EQ(TMath::FloorLog2((uint)TMath::Pow2(18)), 18);
    ASSERT_EQ(TMath::FloorLog2((uint)TMath::Pow2(18) + 1), 18);
    ASSERT_EQ(TMath::FloorLog2((uint)TMath::Pow2(31)), 31);
    //ASSERT_EQ(TMath::FloorLog2((uint)TMath::Pow2(32) - 1), 31);
}

TEST(Misc, TMathFloorLog2_uint64) {
    ASSERT_EQ(TMath::FloorLog2((uint64)1), 0);
    ASSERT_EQ(TMath::FloorLog2((uint64)2), 1);
    ASSERT_EQ(TMath::FloorLog2((uint64)3), 1);
    ASSERT_EQ(TMath::FloorLog2((uint64)4), 2);
    ASSERT_EQ(TMath::FloorLog2((uint64)5), 2);
    ASSERT_EQ(TMath::FloorLog2((uint64)6), 2);
    ASSERT_EQ(TMath::FloorLog2((uint64)7), 2);
    ASSERT_EQ(TMath::FloorLog2((uint64)8), 3);
    ASSERT_EQ(TMath::FloorLog2((uint64)TMath::Pow2<uint64>(32) - 1), 31);
    ASSERT_EQ(TMath::FloorLog2((uint64)TMath::Pow2<uint64>(32)), 32);
    ASSERT_EQ(TMath::FloorLog2((uint64)TMath::Pow2<uint64>(32) + 1), 32);
    ASSERT_EQ(TMath::FloorLog2((uint64)TMath::Pow2<uint64>(63)), 63);
    //ASSERT_EQ(TMath::FloorLog2((uint64)TMath::Pow2<uint64>(64) - 1), 63);
}

// TBool string parsing: all of T/F, Y/N, Yes/No, True/False must parse in any
// case. Regression: the old IsValStr uppercased the input but compared against
// the mixed-case "Yes"/"No" constants, and did not accept "true"/"false" at
// all, so command-line args like "-add:false" were silently ignored and fell
// back to the default.
TEST(Misc, TBoolGetValFromStr) {
    // true-like values, assorted cases
    ASSERT_TRUE(TBool::GetValFromStr("T", false));
    ASSERT_TRUE(TBool::GetValFromStr("t", false));
    ASSERT_TRUE(TBool::GetValFromStr("Y", false));
    ASSERT_TRUE(TBool::GetValFromStr("Yes", false));
    ASSERT_TRUE(TBool::GetValFromStr("YES", false));
    ASSERT_TRUE(TBool::GetValFromStr("yes", false));
    ASSERT_TRUE(TBool::GetValFromStr("true", false));
    ASSERT_TRUE(TBool::GetValFromStr("True", false));
    ASSERT_TRUE(TBool::GetValFromStr("TRUE", false));

    // false-like values must override a true default
    ASSERT_FALSE(TBool::GetValFromStr("F", true));
    ASSERT_FALSE(TBool::GetValFromStr("f", true));
    ASSERT_FALSE(TBool::GetValFromStr("N", true));
    ASSERT_FALSE(TBool::GetValFromStr("No", true));
    ASSERT_FALSE(TBool::GetValFromStr("NO", true));
    ASSERT_FALSE(TBool::GetValFromStr("no", true));
    ASSERT_FALSE(TBool::GetValFromStr("false", true));
    ASSERT_FALSE(TBool::GetValFromStr("False", true));
    ASSERT_FALSE(TBool::GetValFromStr("FALSE", true));

    // unrecognized values keep the default
    ASSERT_TRUE(TBool::GetValFromStr("", true));
    ASSERT_FALSE(TBool::GetValFromStr("", false));
    ASSERT_TRUE(TBool::GetValFromStr("maybe", true));
    ASSERT_FALSE(TBool::GetValFromStr("maybe", false));

    // IsValStr agrees with the accepted set
    ASSERT_TRUE(TBool::IsValStr("false"));
    ASSERT_TRUE(TBool::IsValStr("TRUE"));
    ASSERT_TRUE(TBool::IsValStr("no"));
    ASSERT_TRUE(TBool::IsValStr("Yes"));
    ASSERT_FALSE(TBool::IsValStr(""));
    ASSERT_FALSE(TBool::IsValStr("maybe"));

    // single-arg variant: true-like of any case, everything else false
    ASSERT_TRUE(TBool::GetValFromStr("true"));
    ASSERT_TRUE(TBool::GetValFromStr("Yes"));
    ASSERT_TRUE(TBool::GetValFromStr("t"));
    ASSERT_FALSE(TBool::GetValFromStr("false"));
    ASSERT_FALSE(TBool::GetValFromStr("F"));

    // output formats are unchanged
    ASSERT_EQ(TStr("T"), TBool::GetStr(true));
    ASSERT_EQ(TStr("F"), TBool::GetStr(false));
    ASSERT_EQ(TStr("Yes"), TBool::GetYesNoStr(true));
    ASSERT_EQ(TStr("No"), TBool::GetYesNoStr(false));
}
