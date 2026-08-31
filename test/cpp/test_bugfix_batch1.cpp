/* Copyright (C) Event Registry d.o.o. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 */

// Regression tests for bug batch 1 (2026-08-31 deep analysis, docs/2026-08-31
// qminer-glib-deep-analysis.md in the backend repo):
//   B1 TRecSet::Merge(TVec<PRecSet>) - loop condition never involved the index
//   B2 TChA::operator+=(TMem) used strcpy on unterminated binary memory
//   B3 SerializeUpdateInPlace set the null flag to TRUE before writing a value
//   B4 RunVerificationForRecord read the mem-section record from the disk blob
//   B5 TQmGixItemPos::Intersect wraparound test used abs(Pos1 - Pos1)
//   B6 delete paths added words to the vocabulary via AddWordStr/AddWordIdV

#include <qminer.h>
#include <qminer_storage.h>
#include "gtest/gtest.h"

using namespace TQm;
using namespace TQm::TStorage;

namespace {

void FreshDir(const TStr& FPath)
{
    if (TDir::Exists(FPath)) { TDir::DelNonEmptyDir(FPath); }
    TDir::GenDir(FPath);
}

void EnsureQmEnv()
{
    if (!TQm::TEnv::IsInit()) { TQm::TEnv::Init(); }
}

PJsonVal ParseJson(const TStr& JsonStr)
{
    PJsonVal Val = TJsonVal::GetValFromStr(JsonStr);
    EAssertR(Val->IsDef(), "test JSON failed to parse: " + JsonStr);
    return Val;
}

} // namespace

// B2: TMem is raw memory - appending it to a TChA must copy exactly Len() bytes,
// embedded NUL bytes included (the old strcpy stopped at the first NUL while still
// advancing the length, leaving garbage in the middle of the string)
TEST(BugfixBatch1, TChAAppendTMemKeepsBinaryContent)
{
    TMem Mem;
    Mem += 'a'; Mem += '\0'; Mem += 'b'; Mem += '\0'; Mem += 'c';
    TChA ChA("xy");
    ChA += Mem;
    ASSERT_EQ(7, ChA.Len());
    EXPECT_EQ('x', ChA[0]);
    EXPECT_EQ('y', ChA[1]);
    EXPECT_EQ('a', ChA[2]);
    EXPECT_EQ('\0', ChA[3]);
    EXPECT_EQ('b', ChA[4]);
    EXPECT_EQ('\0', ChA[5]);
    EXPECT_EQ('c', ChA[6]);
}

// B2 (same disease): TChA::AddBf used strncpy, which stops at NULs and zero-pads
TEST(BugfixBatch1, TChAAddBfKeepsBinaryContent)
{
    char Bf[5] = { 'q', '\0', 'r', '\0', 's' };
    TChA ChA;
    ChA.AddBf(Bf, 5);
    ASSERT_EQ(5, ChA.Len());
    EXPECT_EQ('q', ChA[0]);
    EXPECT_EQ('\0', ChA[1]);
    EXPECT_EQ('r', ChA[2]);
    EXPECT_EQ('\0', ChA[3]);
    EXPECT_EQ('s', ChA[4]);
}

// B5: with a negative MaxDiff (unordered proximity), a pair of positions that are
// truly adjacent across the modulo wrap (Pos1 near Modulo, Pos2 near 0) must match.
// The old test computed abs(Pos1 - Pos1) == 0, so the wrap branch degenerated to
// "Modulo <= -MaxDiff" and never fired for realistic MaxDiff values.
TEST(BugfixBatch1, ItemPosIntersectMatchesAcrossModuloWrap)
{
    // positions cycle every Modulo tokens; stored positions are 1..Modulo.
    // Pos1 = Modulo-2, Pos2 = 2: wrapped distance = (2 + Modulo) - (Modulo-2) = 4
    const int Modulo = TIndex::TQmGixItemPos::Modulo;
    {
        TIndex::TQmGixItemPos A((uint64)7); A.Add(Modulo - 2);
        TIndex::TQmGixItemPos B((uint64)7); B.Add(2);
        int TotalDiff = 0;
        TIndex::TQmGixItemPos R = A.Intersect(B, -5, TotalDiff);
        ASSERT_EQ(1, R.GetPosLen()) << "wrap-adjacent positions did not match";
        EXPECT_EQ(2, R.GetPos(0));
        EXPECT_EQ(4, TotalDiff);
    }
    // wrapped distance 15 must NOT match at -5
    {
        TIndex::TQmGixItemPos A((uint64)7); A.Add(Modulo - 13);
        TIndex::TQmGixItemPos B((uint64)7); B.Add(2);
        int TotalDiff = 0;
        TIndex::TQmGixItemPos R = A.Intersect(B, -5, TotalDiff);
        EXPECT_EQ(0, R.GetPosLen());
    }
    // the plain (non-wrap) negative-MaxDiff match still works
    {
        TIndex::TQmGixItemPos A((uint64)7); A.Add(10);
        TIndex::TQmGixItemPos B((uint64)7); B.Add(12);
        int TotalDiff = 0;
        TIndex::TQmGixItemPos R = A.Intersect(B, -5, TotalDiff);
        ASSERT_EQ(1, R.GetPosLen());
        EXPECT_EQ(12, R.GetPos(0));
        EXPECT_EQ(2, TotalDiff);
    }
}

// B1: the vector overload of TRecSet::Merge looped with the condition
// "RecSetV.Len()" - any non-empty input read past the end of the vector
TEST(BugfixBatch1, RecSetMergeVectorMergesAllSets)
{
    const TStr FPath = "./bugfix_merge/";
    FreshDir(FPath);
    EnsureQmEnv();
    const TStr SchemaStr =
        "[{ \"name\": \"MergeStore\","
        "   \"fields\": [ { \"name\": \"Name\", \"type\": \"string\", \"primary\": true } ]"
        "}]";
    {
        PBase Base = TStorage::NewBase(FPath, ParseJson(SchemaStr), 10000000, 10000000, true);
        TWPt<TStore> Store = Base->GetStoreByStoreNm("MergeStore");
        const uint64 Rec1 = Store->AddRec(ParseJson("{ \"Name\": \"r1\" }"));
        const uint64 Rec2 = Store->AddRec(ParseJson("{ \"Name\": \"r2\" }"));
        const uint64 Rec3 = Store->AddRec(ParseJson("{ \"Name\": \"r3\" }"));

        PRecSet MainRecSet = TRecSet::New(Store, TUInt64V::GetV(Rec1));
        TVec<PRecSet> MergeV;
        MergeV.Add(TRecSet::New(Store, TUInt64V::GetV(Rec2)));
        MergeV.Add(TRecSet::New(Store, TUInt64V::GetV(Rec3)));
        MainRecSet->Merge(MergeV);
        EXPECT_EQ(3, MainRecSet->GetRecs());
        TUInt64V MergedIdV; MainRecSet->GetRecIdV(MergedIdV);
        EXPECT_TRUE(MergedIdV.IsIn(Rec1));
        EXPECT_TRUE(MergedIdV.IsIn(Rec2));
        EXPECT_TRUE(MergedIdV.IsIn(Rec3));
        // empty vector is a no-op
        TVec<PRecSet> EmptyV;
        MainRecSet->Merge(EmptyV);
        EXPECT_EQ(3, MainRecSet->GetRecs());
        TStorage::SaveBase(Base);
    }
    TDir::DelNonEmptyDir(FPath);
}

// B6: deleting a record must never ADD words to the vocabulary - a word that is
// not in the vocabulary was never indexed, so there is nothing to delete for it
TEST(BugfixBatch1, DeleteDoesNotGrowVocabulary)
{
    const TStr FPath = "./bugfix_voc/";
    FreshDir(FPath);
    EnsureQmEnv();
    const TStr SchemaStr =
        "[{ \"name\": \"VocStore\","
        "   \"fields\": [ { \"name\": \"Name\", \"type\": \"string\", \"primary\": true },"
        "                 { \"name\": \"Val\", \"type\": \"string\" },"
        "                 { \"name\": \"Txt\", \"type\": \"string\" } ],"
        "   \"keys\": [ { \"field\": \"Val\", \"type\": \"value\" },"
        "               { \"field\": \"Txt\", \"type\": \"text\" } ]"
        "}]";
    {
        PBase Base = TStorage::NewBase(FPath, ParseJson(SchemaStr), 10000000, 10000000, true);
        TWPt<TStore> Store = Base->GetStoreByStoreNm("VocStore");
        const uint64 RecId = Store->AddRec(ParseJson(
            "{ \"Name\": \"a\", \"Val\": \"hello\", \"Txt\": \"quick brown fox\" }"));

        TWPt<TIndexVoc> IndexVoc = Base->GetIndexVoc();
        TWPt<TIndex> Index = Base->GetIndex();
        const int ValKeyId = IndexVoc->GetKeyId(Store->GetStoreId(), "Val");
        const int TxtKeyId = IndexVoc->GetKeyId(Store->GetStoreId(), "Txt");
        const uint64 ValWords = IndexVoc->GetWords(ValKeyId);
        const uint64 TxtWords = IndexVoc->GetWords(TxtKeyId);
        ASSERT_GE(ValWords, uint64(1));
        ASSERT_GE(TxtWords, uint64(3));

        // deleting values/text that were never indexed must not grow the vocabulary
        Index->DeleteValue(ValKeyId, TStr("neverseen"), RecId);
        TStrV UnseenV; UnseenV.Add("ghost1"); UnseenV.Add("ghost2");
        Index->DeleteValue(ValKeyId, UnseenV, RecId);
        Index->DeleteText(TxtKeyId, TStr("totally unseen tokens"), RecId);
        EXPECT_EQ(ValWords, IndexVoc->GetWords(ValKeyId));
        EXPECT_EQ(TxtWords, IndexVoc->GetWords(TxtKeyId));
        EXPECT_FALSE(IndexVoc->IsWordStr(ValKeyId, "neverseen"));
        EXPECT_FALSE(IndexVoc->IsWordStr(ValKeyId, "ghost1"));

        // deleting an indexed word still works and still does not change the vocabulary
        Index->DeleteValue(ValKeyId, TStr("hello"), RecId);
        EXPECT_EQ(ValWords, IndexVoc->GetWords(ValKeyId));
        TStorage::SaveBase(Base);
    }
    TDir::DelNonEmptyDir(FPath);
}

// B3 + B4 use a paged (TStorePbBlob) store with a mem-section field
namespace {
const char* PagedSchemaStr =
    "[{ \"name\": \"PagedStore\","
    "   \"options\": { \"type\": \"paged\" },"
    "   \"fields\": [ { \"name\": \"Name\", \"type\": \"string\", \"primary\": true },"
    "                 { \"name\": \"Num\", \"type\": \"int\" },"
    "                 { \"name\": \"MemNum\", \"type\": \"int\", \"store\": \"memory\" } ]"
    "}]";
}

// B3: an in-place update that throws half-way (mistyped value for a fixed-part
// field) must leave the live record untouched. The old code first flagged the
// field NULL in the live page buffer, so the throw left it nulled.
TEST(BugfixBatch1, InPlaceUpdateWithBadTypeKeepsField)
{
    const TStr FPath = "./bugfix_inplace/";
    FreshDir(FPath);
    EnsureQmEnv();
    {
        PBase Base = TStorage::NewBase(FPath, ParseJson(PagedSchemaStr), 10000000, 10000000, true);
        TWPt<TStore> Store = Base->GetStoreByStoreNm("PagedStore");
        ASSERT_EQ(TStr("TStorePbBlob"), Store->GetStoreType());
        const uint64 RecId = Store->AddRec(ParseJson("{ \"Name\": \"x\", \"Num\": 42, \"MemNum\": 7 }"));
        const int NumFieldId = Store->GetFieldId("Num");

        // sanity: a good in-place update works
        Store->UpdateRec(RecId, ParseJson("{ \"Num\": 43 }"));
        EXPECT_EQ(43, Store->GetFieldInt(RecId, NumFieldId));

        // a mistyped update must throw and leave the field with its old value, NOT null
        EXPECT_THROW(Store->UpdateRec(RecId, ParseJson("{ \"Num\": \"not-a-number\" }")), PExcept);
        EXPECT_FALSE(Store->IsFieldNull(RecId, NumFieldId))
            << "throwing in-place update left the field flagged null";
        EXPECT_EQ(43, Store->GetFieldInt(RecId, NumFieldId));
        TStorage::SaveBase(Base);
    }
    TDir::DelNonEmptyDir(FPath);
}

// B4: RunVerification on a paged store with a memory section must verify the
// mem-section records against the MEM blob (they used to be read from the disk
// blob, verifying garbage)
TEST(BugfixBatch1, PagedStoreVerificationReadsMemBlob)
{
    const TStr FPath = "./bugfix_verify/";
    FreshDir(FPath);
    EnsureQmEnv();
    {
        PBase Base = TStorage::NewBase(FPath, ParseJson(PagedSchemaStr), 10000000, 10000000, true);
        TWPt<TStore> Store = Base->GetStoreByStoreNm("PagedStore");
        for (int RecN = 0; RecN < 20; RecN++) {
            Store->AddRec(ParseJson(TStr::Fmt(
                "{ \"Name\": \"rec%d\", \"Num\": %d, \"MemNum\": %d }", RecN, RecN * 10, RecN)));
        }
        TStorePbBlob* PbStore = dynamic_cast<TStorePbBlob*>(Store());
        ASSERT_TRUE(PbStore != NULL);
        EXPECT_NO_THROW(PbStore->RunVerification());
        TStorage::SaveBase(Base);
    }
    TDir::DelNonEmptyDir(FPath);
}
