/* Copyright (C) Event Registry d.o.o. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 */

// Regression tests for the storage-layer fixes (2026-09-03):
//   - TStorePbBlobT: the GetPgBf per-record memo is invalidated by the
//     variable-length setters (SetFieldStr/StrV/IntV/...) whose Put relocates
//     the record, and per record inside the delete loops. Before, the memo kept
//     the pre-relocation pointer and the next read of that record hit a freed
//     (reusable) slot.
//   - TStoreImpl: AddRec / UpdateRec / the field setters mark the metadata dirty,
//     so the .GenericStore file (which also carries the serializators' string
//     codebooks) is rewritten on close for stores without a primary field.
//   - TPbBlobRecMapDense::TrimLeadingEmpty shifts in place (no second copy of a
//     >1 GB map) and, for large maps, only trims with hysteresis. Small maps keep
//     trimming immediately, so the "offset == first live id" semantics the
//     existing tests pin are unchanged.

#include <qminer.h>
#include <qminer_storage.h>
#include "gtest/gtest.h"

using namespace TQm;

namespace {

void FreshDir(const TStr& FPath)
{
    if (TDir::Exists(FPath)) { TDir::DelNonEmptyDir(FPath); }
    TDir::GenDir(FPath);
}

void EnsureQmEnv()
{
    if (!TQm::TEnv::IsInit()) { TQm::TEnv::Init(); }
    if (!TUnicodeDef::IsDef()) { TUnicodeDef::Load("./src/glib/bin/UnicodeDef.Bin"); }
}

// a long, non-repetitive string (Len chars)
TStr MakeLongStr(const int& Len, const int& Seed)
{
    TChA ChA;
    for (int ChN = 0; ChN < Len; ChN++) { ChA += char('a' + (ChN * 7 + Seed) % 26); }
    return TStr(ChA);
}

TPgBlobPt MakePt(const int64& N)
{
    return TPgBlobPt(int16(0), uint32(N), uint16(N % 1000));
}

} // namespace

// a relocating setter (the new value no longer fits in the record's slot) must
// not leave the GetPgBf memo pointing at the old slot: reads of the same record
// right after the setter, of both the changed and an unchanged field, and of a
// neighbour, must return the exact values - also after a close/reopen
TEST(StorageFixesTests, MemoStaleAfterSetFieldStr)
{
    const TStr FPath = "./storage_fixes_memo/";
    FreshDir(FPath);
    EnsureQmEnv();
    const TStr SchemaStr =
        "[{ \"name\": \"MemoStore\","
        "   \"options\": { \"type\": \"paged\" },"
        "   \"fields\": [ { \"name\": \"Name\", \"type\": \"string\", \"primary\": true },"
        "                 { \"name\": \"Text\", \"type\": \"string\" },"
        "                 { \"name\": \"Tags\", \"type\": \"string_v\" },"
        "                 { \"name\": \"Nums\", \"type\": \"int_v\" } ]"
        "}]";
    const int Recs = 50;
    const TStr LongStr = MakeLongStr(3000, 1);
    const TStr LongStr2 = MakeLongStr(4000, 2);
    // expected values per record, kept in sync with every setter call below
    TVec<TStr> TextV; TVec<TStrV> TagsV; TVec<TIntV> NumsV;
    for (int RecN = 0; RecN < Recs; RecN++) {
        TextV.Add(TStr::Fmt("text%d", RecN));
        TStrV Tags; Tags.Add(TStr::Fmt("tag%d", RecN)); TagsV.Add(Tags);
        TIntV Nums; Nums.Add(RecN); NumsV.Add(Nums);
    }
    TUInt64V RecIdV;
    {
        PBase Base = TStorage::NewBase(FPath, TJsonVal::GetValFromStr(SchemaStr), 10000000, 10000000, true);
        TWPt<TStore> Store = Base->GetStoreByStoreNm("MemoStore");
        const int NameId = Store->GetFieldId("Name");
        const int TextId = Store->GetFieldId("Text");
        const int TagsId = Store->GetFieldId("Tags");
        const int NumsId = Store->GetFieldId("Nums");
        for (int RecN = 0; RecN < Recs; RecN++) {
            RecIdV.Add(Store->AddRec(TJsonVal::GetValFromStr(TStr::Fmt(
                "{ \"Name\": \"rec%d\", \"Text\": \"text%d\", \"Tags\": [\"tag%d\"], \"Nums\": [%d] }",
                RecN, RecN, RecN, RecN))));
        }
        ASSERT_EQ(Recs, (int)Store->GetRecs());

        // SetFieldStr: prime the memo with the record, then grow it far past its slot
        const uint64 Rec10 = RecIdV[10]; const uint64 Rec11 = RecIdV[11];
        EXPECT_EQ(TStr("text10"), Store->GetFieldStr(Rec10, TextId));
        Store->SetFieldStr(Rec10, TextId, LongStr); TextV[10] = LongStr;
        EXPECT_EQ(LongStr, Store->GetFieldStr(Rec10, TextId));
        EXPECT_EQ(TStr("rec10"), Store->GetFieldStr(Rec10, NameId));
        EXPECT_EQ(TStr("text11"), Store->GetFieldStr(Rec11, TextId));
        EXPECT_EQ(TStr("rec11"), Store->GetFieldStr(Rec11, NameId));
        // shrink it again and grow the neighbour: the freed large slot is a
        // candidate for reuse, so a stale memo would now read the wrong record
        Store->SetFieldStr(Rec10, TextId, "short"); TextV[10] = "short";
        Store->SetFieldStr(Rec11, TextId, LongStr2); TextV[11] = LongStr2;
        EXPECT_EQ(TStr("short"), Store->GetFieldStr(Rec10, TextId));
        EXPECT_EQ(LongStr2, Store->GetFieldStr(Rec11, TextId));
        EXPECT_EQ(TStr("rec10"), Store->GetFieldStr(Rec10, NameId));
        EXPECT_EQ(TStr("rec11"), Store->GetFieldStr(Rec11, NameId));

        // SetFieldStrV goes through the same GetRecData/Put path
        const uint64 Rec20 = RecIdV[20];
        TStrV NewTags; for (int TagN = 0; TagN < 300; TagN++) { NewTags.Add(TStr::Fmt("tag-%d-%d", 20, TagN)); }
        EXPECT_EQ(TStr("text20"), Store->GetFieldStr(Rec20, TextId));
        Store->SetFieldStrV(Rec20, TagsId, NewTags); TagsV[20] = NewTags;
        TStrV GotTags; Store->GetFieldStrV(Rec20, TagsId, GotTags);
        EXPECT_TRUE(NewTags == GotTags);
        EXPECT_EQ(TStr("text20"), Store->GetFieldStr(Rec20, TextId));
        EXPECT_EQ(TStr("rec20"), Store->GetFieldStr(Rec20, NameId));

        // SetFieldIntV as well
        const uint64 Rec30 = RecIdV[30];
        TIntV NewNums; for (int NumN = 0; NumN < 1000; NumN++) { NewNums.Add(NumN * 3); }
        EXPECT_EQ(TStr("text30"), Store->GetFieldStr(Rec30, TextId));
        Store->SetFieldIntV(Rec30, NumsId, NewNums); NumsV[30] = NewNums;
        TIntV GotNums; Store->GetFieldIntV(Rec30, NumsId, GotNums);
        EXPECT_TRUE(NewNums == GotNums);
        EXPECT_EQ(TStr("text30"), Store->GetFieldStr(Rec30, TextId));
        EXPECT_EQ(TStr("rec30"), Store->GetFieldStr(Rec30, NameId));

        // every record still reads back exactly
        for (int RecN = 0; RecN < Recs; RecN++) {
            const uint64 RecId = RecIdV[RecN];
            EXPECT_EQ(TStr::Fmt("rec%d", RecN), Store->GetFieldStr(RecId, NameId)) << RecN;
            EXPECT_EQ(TextV[RecN], Store->GetFieldStr(RecId, TextId)) << RecN;
            TStrV Tags; Store->GetFieldStrV(RecId, TagsId, Tags);
            EXPECT_TRUE(TagsV[RecN] == Tags) << RecN;
            TIntV Nums; Store->GetFieldIntV(RecId, NumsId, Nums);
            EXPECT_TRUE(NumsV[RecN] == Nums) << RecN;
        }
        TStorage::SaveBase(Base);
    }
    // the relocated records persist correctly
    {
        PBase Base = TStorage::LoadBase(FPath, faRdOnly, 10000000, 10000000);
        TWPt<TStore> Store = Base->GetStoreByStoreNm("MemoStore");
        const int NameId = Store->GetFieldId("Name");
        const int TextId = Store->GetFieldId("Text");
        const int TagsId = Store->GetFieldId("Tags");
        const int NumsId = Store->GetFieldId("Nums");
        ASSERT_EQ(Recs, (int)Store->GetRecs());
        for (int RecN = 0; RecN < Recs; RecN++) {
            const uint64 RecId = RecIdV[RecN];
            ASSERT_TRUE(Store->IsRecId(RecId)) << RecN;
            EXPECT_EQ(TStr::Fmt("rec%d", RecN), Store->GetFieldStr(RecId, NameId)) << RecN;
            EXPECT_EQ(TextV[RecN], Store->GetFieldStr(RecId, TextId)) << RecN;
            TStrV Tags; Store->GetFieldStrV(RecId, TagsId, Tags);
            EXPECT_TRUE(TagsV[RecN] == Tags) << RecN;
            TIntV Nums; Store->GetFieldIntV(RecId, NumsId, Nums);
            EXPECT_TRUE(NumsV[RecN] == Nums) << RecN;
        }
    }
    TDir::DelNonEmptyDir(FPath);
}

// a non-paged store without a primary field: adding records (and later only
// updating codebook values) grows the string codebook, which is saved in
// .GenericStore - the file must be rewritten on close or the codebook ids
// written into the records have no strings after a reload
TEST(StorageFixesTests, CodebookSurvivesReloadWithoutPrimary)
{
    const TStr FPath = "./storage_fixes_codebook/";
    FreshDir(FPath);
    EnsureQmEnv();
    const TStr SchemaStr =
        "[{ \"name\": \"KindStore\","
        "   \"fields\": [ { \"name\": \"Kind\", \"type\": \"string\", \"codebook\": true },"
        "                 { \"name\": \"Num\", \"type\": \"int\" } ]"
        "}]";
    TStrV KindV; KindV.Add("alpha"); KindV.Add("beta"); KindV.Add("gamma");
    TUInt64V RecIdV;
    {
        PBase Base = TStorage::NewBase(FPath, TJsonVal::GetValFromStr(SchemaStr), 10000000, 10000000, true);
        TWPt<TStore> Store = Base->GetStoreByStoreNm("KindStore");
        ASSERT_FALSE(Store->HasRecNm());	// no primary field
        for (int RecN = 0; RecN < KindV.Len(); RecN++) {
            PJsonVal RecVal = TJsonVal::NewObj();
            RecVal->AddToObj("Kind", KindV[RecN]);
            RecVal->AddToObj("Num", RecN);
            RecIdV.Add(Store->AddRec(RecVal));
        }
        TStorage::SaveBase(Base);
    }
    {
        PBase Base = TStorage::LoadBase(FPath, faRdOnly, 10000000, 10000000);
        TWPt<TStore> Store = Base->GetStoreByStoreNm("KindStore");
        const int KindId = Store->GetFieldId("Kind");
        const int NumId = Store->GetFieldId("Num");
        ASSERT_EQ(KindV.Len(), (int)Store->GetRecs());
        for (int RecN = 0; RecN < KindV.Len(); RecN++) {
            EXPECT_EQ(KindV[RecN], Store->GetFieldStr(RecIdV[RecN], KindId)) << RecN;
            EXPECT_EQ(RecN, Store->GetFieldInt(RecIdV[RecN], NumId)) << RecN;
        }
    }
    // a session that only sets a new codebook value (no add) must persist it too
    {
        PBase Base = TStorage::LoadBase(FPath, faUpdate, 10000000, 10000000);
        TWPt<TStore> Store = Base->GetStoreByStoreNm("KindStore");
        const int KindId = Store->GetFieldId("Kind");
        Store->SetFieldStr(RecIdV[1], KindId, "delta"); KindV[1] = "delta";
        EXPECT_EQ(TStr("delta"), Store->GetFieldStr(RecIdV[1], KindId));
        TStorage::SaveBase(Base);
    }
    {
        PBase Base = TStorage::LoadBase(FPath, faRdOnly, 10000000, 10000000);
        TWPt<TStore> Store = Base->GetStoreByStoreNm("KindStore");
        const int KindId = Store->GetFieldId("Kind");
        for (int RecN = 0; RecN < KindV.Len(); RecN++) {
            EXPECT_EQ(KindV[RecN], Store->GetFieldStr(RecIdV[RecN], KindId)) << RecN;
        }
    }
    TDir::DelNonEmptyDir(FPath);
}

// TPbBlobRecMapDense::TrimLeadingEmpty: small maps trim immediately (offset =
// first live id, as the existing tests pin); a map above the hysteresis
// threshold defers the trim until enough leading slots are empty, and then
// compacts them all at once - every live id stays addressable throughout
TEST(StorageFixesTests, DenseTrimHysteresis)
{
    // small map: immediate trim, offset advances to the first live id
    {
        TStorage::TPbBlobRecMapDense Map;
        for (int64 N = 0; N < 10; N++) { Map.AddDat((uint64)N, MakePt(N)); }
        Map.DelKey(0); Map.DelKey(1); Map.DelKey(2);
        EXPECT_EQ(int64(3), Map.TrimLeadingEmpty());
        EXPECT_EQ(uint64(3), Map.GetRecIdOffset());
        EXPECT_EQ(int64(7), Map.Slots());
        EXPECT_EQ(int64(7), Map.Len());
        EXPECT_FALSE(Map.IsKey(2));
        for (int64 N = 3; N < 10; N++) {
            ASSERT_TRUE(Map.IsKey((uint64)N)) << N;
            EXPECT_TRUE(Map.GetDat((uint64)N) == MakePt(N)) << N;
        }
        EXPECT_EQ(int64(0), Map.TrimLeadingEmpty());
        // adding after the trim keeps the offset
        Map.AddDat(10, MakePt(10));
        EXPECT_EQ(uint64(3), Map.GetRecIdOffset());
        EXPECT_TRUE(Map.GetDat(10) == MakePt(10));
    }
    // large map (> 1<<20 slots): a few leading deletes do not trim yet
    {
        const int64 Live = 100;
        const int64 FarId = (int64(1) << 20) + 50;
        TStorage::TPbBlobRecMapDense Map;
        for (int64 N = 0; N < Live; N++) { Map.AddDat((uint64)N, MakePt(N)); }
        Map.AddDat((uint64)FarId, MakePt(FarId));	// gap-fills up to FarId
        ASSERT_EQ(FarId + 1, Map.Slots());
        ASSERT_EQ(Live + 1, Map.Len());
        for (int64 N = 0; N < 10; N++) { Map.DelKey((uint64)N); }
        EXPECT_EQ(int64(0), Map.TrimLeadingEmpty());	// deferred: 10 < Slots/16
        EXPECT_EQ(uint64(0), Map.GetRecIdOffset());
        EXPECT_EQ(FarId + 1, Map.Slots());
        EXPECT_EQ(uint64(10), Map.GetFirstRecId());
        for (int64 N = 10; N < Live; N++) {
            ASSERT_TRUE(Map.IsKey((uint64)N)) << N;
            EXPECT_TRUE(Map.GetDat((uint64)N) == MakePt(N)) << N;
        }
        EXPECT_TRUE(Map.GetDat((uint64)FarId) == MakePt(FarId));
        // once the leading empty run is >= Slots/16, everything is trimmed in one go
        for (int64 N = 10; N < Live; N++) { Map.DelKey((uint64)N); }
        EXPECT_EQ(FarId, Map.TrimLeadingEmpty());
        EXPECT_EQ((uint64)FarId, Map.GetRecIdOffset());
        EXPECT_EQ(int64(1), Map.Slots());
        EXPECT_EQ(int64(1), Map.Len());
        EXPECT_FALSE(Map.IsKey((uint64)(Live - 1)));
        ASSERT_TRUE(Map.IsKey((uint64)FarId));
        EXPECT_TRUE(Map.GetDat((uint64)FarId) == MakePt(FarId));
        EXPECT_EQ((uint64)FarId, Map.GetFirstRecId());
        EXPECT_EQ((uint64)FarId, Map.GetLastRecId());
        TUInt64V KeyV; Map.GetKeyV(KeyV);
        ASSERT_EQ(1, KeyV.Len());
        EXPECT_EQ((uint64)FarId, (uint64)KeyV[0]);
        // ids keep growing after the trim
        Map.AddDat((uint64)FarId + 1, MakePt(FarId + 1));
        EXPECT_EQ(int64(2), Map.Slots());
        EXPECT_TRUE(Map.GetDat((uint64)FarId + 1) == MakePt(FarId + 1));
    }
}

// dense-map paged store: deleting the leading records trims the map in place;
// every remaining record stays readable by its (unchanged) id, the record set
// is right, and the ids survive a close/reopen
TEST(StorageFixesTests, DenseTrimNoReallocKeepsSemantics)
{
    const TStr FPath = "./storage_fixes_dense/";
    FreshDir(FPath);
    EnsureQmEnv();
    const TStr SchemaStr =
        "[{ \"name\": \"DenseStore\","
        "   \"options\": { \"type\": \"paged\", \"recIdMap\": \"dense\" },"
        "   \"fields\": [ { \"name\": \"Name\", \"type\": \"string\", \"primary\": true },"
        "                 { \"name\": \"Num\", \"type\": \"int\" } ]"
        "}]";
    const int Recs = 40;
    TUInt64V RecIdV;
    // checks that records [FirstLive, Recs) are exactly the live set
    auto CheckLive = [&](const TWPt<TStore>& Store, const int& FirstLive) {
        const int NameId = Store->GetFieldId("Name");
        const int NumId = Store->GetFieldId("Num");
        ASSERT_EQ(Recs - FirstLive, (int)Store->GetRecs());
        EXPECT_EQ((uint64)RecIdV[FirstLive], Store->GetFirstRecId());
        for (int RecN = 0; RecN < FirstLive; RecN++) { EXPECT_FALSE(Store->IsRecId(RecIdV[RecN])) << RecN; }
        for (int RecN = FirstLive; RecN < Recs; RecN++) {
            const uint64 RecId = RecIdV[RecN];
            ASSERT_TRUE(Store->IsRecId(RecId)) << RecN;
            EXPECT_EQ(TStr::Fmt("rec%d", RecN), Store->GetFieldStr(RecId, NameId)) << RecN;
            EXPECT_EQ(RecN, Store->GetFieldInt(RecId, NumId)) << RecN;
            EXPECT_EQ(RecId, Store->GetRecId(TStr::Fmt("rec%d", RecN))) << RecN;
        }
        TUInt64V AllRecIdV; Store->GetAllRecs()->GetRecIdV(AllRecIdV);
        ASSERT_EQ(Recs - FirstLive, AllRecIdV.Len());
        for (int RecN = FirstLive; RecN < Recs; RecN++) {
            EXPECT_EQ((uint64)RecIdV[RecN], (uint64)AllRecIdV[RecN - FirstLive]) << RecN;
        }
    };
    {
        PBase Base = TStorage::NewBase(FPath, TJsonVal::GetValFromStr(SchemaStr), 10000000, 10000000, true);
        TWPt<TStore> Store = Base->GetStoreByStoreNm("DenseStore");
        EXPECT_EQ(TStr("TStorePbBlobDense"), Store->GetStoreType());
        for (int RecN = 0; RecN < Recs; RecN++) {
            RecIdV.Add(Store->AddRec(TJsonVal::GetValFromStr(TStr::Fmt("{ \"Name\": \"rec%d\", \"Num\": %d }", RecN, RecN))));
        }
        CheckLive(Store, 0);
        // rolling delete of the oldest records, twice (the second one trims an
        // already-trimmed map)
        TUInt64V DelRecIdV;
        for (int RecN = 0; RecN < 5; RecN++) { DelRecIdV.Add(RecIdV[RecN]); }
        Store->DeleteRecs(DelRecIdV);
        CheckLive(Store, 5);
        DelRecIdV.Clr();
        for (int RecN = 5; RecN < 8; RecN++) { DelRecIdV.Add(RecIdV[RecN]); }
        Store->DeleteRecs(DelRecIdV);
        CheckLive(Store, 8);
        TStorage::SaveBase(Base);
    }
    // the trimmed map (and its id offset) persists: same ids after a reload
    {
        PBase Base = TStorage::LoadBase(FPath, faRdOnly, 10000000, 10000000);
        TWPt<TStore> Store = Base->GetStoreByStoreNm("DenseStore");
        EXPECT_EQ(TStr("TStorePbBlobDense"), Store->GetStoreType());
        CheckLive(Store, 8);
    }
    // the store stays writable and the id counter continues after the trim
    {
        PBase Base = TStorage::LoadBase(FPath, faUpdate, 10000000, 10000000);
        TWPt<TStore> Store = Base->GetStoreByStoreNm("DenseStore");
        const uint64 NewRecId = Store->AddRec(TJsonVal::GetValFromStr(
            TStr::Fmt("{ \"Name\": \"rec%d\", \"Num\": %d }", Recs, Recs)));
        EXPECT_EQ((uint64)RecIdV.Last() + 1, NewRecId);
        RecIdV.Add(NewRecId);
        EXPECT_EQ(Recs - 8 + 1, (int)Store->GetRecs());
        TStorage::SaveBase(Base);
    }
    {
        PBase Base = TStorage::LoadBase(FPath, faRdOnly, 10000000, 10000000);
        TWPt<TStore> Store = Base->GetStoreByStoreNm("DenseStore");
        ASSERT_EQ(Recs - 8 + 1, (int)Store->GetRecs());
        EXPECT_TRUE(Store->IsRecId(RecIdV.Last()));
        EXPECT_EQ(TStr::Fmt("rec%d", Recs), Store->GetFieldStr(RecIdV.Last(), Store->GetFieldId("Name")));
        EXPECT_EQ((uint64)RecIdV[8], Store->GetFirstRecId());
    }
    TDir::DelNonEmptyDir(FPath);
}
