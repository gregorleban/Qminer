/* Copyright (C) Event Registry d.o.o. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 */

// Regression test for TGixItemSet::Def() (gix.hpp): a lone negative-frequency
// posting must not survive Def().
//
// qminer implements partial deletes (TIndex::DeleteText / DeleteValue / DeleteJoin)
// as AddItem(RecId, -Fq); the sum merger cancels it against the record's positive
// posting. When no positive exists for that record (the delete re-tokenized the
// text with a different tokenizer than the one that indexed it, a double delete, a
// join count decremented past zero) the negative is lone. The local work-buffer
// merge keeps negatives on purpose (their positive may sit in a child vector), so
// after the 2026-07 change that dropped the always-on global merge from Def() a
// lone negative on a key with child vectors was pushed into a child as a permanent
// ghost posting: it showed up in query results with Fq < 0, and a later legitimate
// add of the same (key, RecId) summed to zero and vanished. Def() now runs the
// handler's global merge over the post-inject tail, which scrubs it.
#include <qminer.h>
#include "gtest/gtest.h"

using namespace TQm;

namespace {

void NegFqFreshDir(const TStr& FPath) {
    if (TDir::Exists(FPath)) { TDir::DelNonEmptyDir(FPath); }
    TDir::GenDir(FPath);
}

void NegFqEnsureQmEnv() {
    if (!TQm::TEnv::IsInit()) { TQm::TEnv::Init(); }
    if (!TUnicodeDef::IsDef()) { TUnicodeDef::Load("./src/glib/bin/UnicodeDef.Bin"); }
}

int CountNonPositiveFq(const PRecSet& RecSet) {
    int Cnt = 0;
    for (int N = 0; N < RecSet->GetRecs(); N++) {
        if (RecSet->GetRecFq(N) <= 0) { Cnt++; }
    }
    return Cnt;
}

bool HasRecId(const PRecSet& RecSet, const uint64& RecId, int& Fq) {
    for (int N = 0; N < RecSet->GetRecs(); N++) {
        if (RecSet->GetRecId(N) == RecId) { Fq = RecSet->GetRecFq(N); return true; }
    }
    return false;
}

const char* NegFqSchemaStr =
    "[{ \"name\": \"S\","
    "   \"fields\": [ { \"name\": \"Name\", \"type\": \"string\", \"primary\": true },"
    "                 { \"name\": \"Txt\", \"type\": \"string\" } ],"
    "   \"keys\": [ { \"field\": \"Txt\", \"type\": \"text\" } ]"
    "}]";

// index Recs records all sharing the word "common" on a key with a tiny split length,
// so the posting list has several child vectors
PBase NewNegFqBase(const TStr& FPath, const int& Recs) {
    NegFqFreshDir(FPath);
    NegFqEnsureQmEnv();
    PBase Base = TStorage::NewBase(FPath, TJsonVal::GetValFromStr(NegFqSchemaStr), 10000000, 10000000, true);
    TStorage::ApplyIndexKeySplitLen(Base, TJsonVal::GetValFromStr(
        "[{ \"name\": \"S\", \"keys\": [ { \"field\": \"Txt\", \"splitLen\": 8 } ] }]"));
    TWPt<TStore> Store = Base->GetStoreByStoreNm("S");
    for (int RecN = 0; RecN < Recs; RecN++) {
        Store->AddRec(TJsonVal::GetValFromStr(TStr::Fmt("{ \"Name\": \"r%d\", \"Txt\": \"common\" }", RecN)));
    }
    return Base;
}

} // namespace

// a partial delete of a word that was never indexed for the record, on a key WITH
// child vectors: the negative must not appear in results, and a later legitimate
// add of that word for the record must be found with its real frequency
TEST(GixNegFqTests, LoneNegativeIsScrubbedOnKeyWithChildren)
{
    const TStr FPath = "./gix_negfq_children/";
    const int Recs = 100;
    PBase Base = NewNegFqBase(FPath, Recs);
    TWPt<TStore> Store = Base->GetStoreByStoreNm("S");
    ASSERT_EQ(Recs, Base->Search("{ \"$from\": \"S\", \"Txt\": \"common\" }")->GetRecs());

    const int KeyId = Base->GetIndexVoc()->GetKeyId(Store->GetStoreId(), "Txt");
    ASSERT_GE(KeyId, 0);
    // a record id above every indexed one - the negative lands beyond the last child
    const uint64 GhostRecId = 100000;

    Base->GetIndex()->DeleteText(KeyId, "common", GhostRecId);
    PRecSet AfterDel = Base->Search("{ \"$from\": \"S\", \"Txt\": \"common\" }");
    EXPECT_EQ(Recs, AfterDel->GetRecs());
    EXPECT_EQ(0, CountNonPositiveFq(AfterDel));
    int Fq = 0;
    EXPECT_FALSE(HasRecId(AfterDel, GhostRecId, Fq));

    // the same record now legitimately gets the word - it must be found, with Fq 1
    Base->GetIndex()->IndexText(KeyId, "common", GhostRecId);
    PRecSet AfterAdd = Base->Search("{ \"$from\": \"S\", \"Txt\": \"common\" }");
    EXPECT_EQ(Recs + 1, AfterAdd->GetRecs());
    EXPECT_TRUE(HasRecId(AfterAdd, GhostRecId, Fq));
    EXPECT_EQ(1, Fq);
    EXPECT_EQ(0, CountNonPositiveFq(AfterAdd));

    // a double delete of a real record: the first cancels the posting, the second is lone
    const uint64 RealRecId = Store->GetRecId(TStr("r5"));
    Base->GetIndex()->DeleteText(KeyId, "common", RealRecId);
    Base->GetIndex()->DeleteText(KeyId, "common", RealRecId);
    PRecSet AfterDouble = Base->Search("{ \"$from\": \"S\", \"Txt\": \"common\" }");
    EXPECT_EQ(Recs, AfterDouble->GetRecs());
    EXPECT_FALSE(HasRecId(AfterDouble, RealRecId, Fq));
    EXPECT_EQ(0, CountNonPositiveFq(AfterDouble));
    // and re-indexing it brings it back exactly once
    Base->GetIndex()->IndexText(KeyId, "common", RealRecId);
    PRecSet AfterReadd = Base->Search("{ \"$from\": \"S\", \"Txt\": \"common\" }");
    EXPECT_EQ(Recs + 1, AfterReadd->GetRecs());
    EXPECT_TRUE(HasRecId(AfterReadd, RealRecId, Fq));
    EXPECT_EQ(1, Fq);

    // the scrubbed state must also be what gets persisted
    TStorage::SaveBase(Base);
    Base.Clr();
    {
        PBase Loaded = TStorage::LoadBase(FPath, faRdOnly, 10000000, 10000000);
        PRecSet Reloaded = Loaded->Search("{ \"$from\": \"S\", \"Txt\": \"common\" }");
        EXPECT_EQ(Recs + 1, Reloaded->GetRecs());
        EXPECT_EQ(0, CountNonPositiveFq(Reloaded));
        EXPECT_TRUE(HasRecId(Reloaded, GhostRecId, Fq));
    }
    TDir::DelNonEmptyDir(FPath);
}

// the same on a key WITHOUT child vectors (a rare word only in the work buffer)
TEST(GixNegFqTests, LoneNegativeIsScrubbedOnChildlessKey)
{
    const TStr FPath = "./gix_negfq_childless/";
    PBase Base = NewNegFqBase(FPath, 20);
    TWPt<TStore> Store = Base->GetStoreByStoreNm("S");
    const int KeyId = Base->GetIndexVoc()->GetKeyId(Store->GetStoreId(), "Txt");
    const uint64 GhostRecId = 100000;

    Base->GetIndex()->DeleteText(KeyId, "rareword", GhostRecId);
    PRecSet AfterDel = Base->Search("{ \"$from\": \"S\", \"Txt\": \"rareword\" }");
    EXPECT_EQ(0, AfterDel->GetRecs());

    Base->GetIndex()->IndexText(KeyId, "rareword", GhostRecId);
    PRecSet AfterAdd = Base->Search("{ \"$from\": \"S\", \"Txt\": \"rareword\" }");
    int Fq = 0;
    EXPECT_EQ(1, AfterAdd->GetRecs());
    EXPECT_TRUE(HasRecId(AfterAdd, GhostRecId, Fq));
    EXPECT_EQ(1, Fq);

    Base.Clr();
    TDir::DelNonEmptyDir(FPath);
}
