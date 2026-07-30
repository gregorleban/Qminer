/* Copyright (C) Event Registry d.o.o. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 */

#include <qminer.h>
#include <qminer_storage.h>

#include "gtest/gtest.h"

using namespace TQm;
using namespace TQm::TStorage;

// Tests for the in-place text reindex: TIndexVoc::CloneWithFreshWordVocs +
// TIndex::ReindexCopyGix (the qminer half of the ReindexIndex console action).
// The Title text key deliberately lives in the SMALL gix next to the hasConcept
// join key and the Source Title value key, so the rebuild has to merge rebuilt
// postings with untouched keys inside one gix file; the Body text_position key
// exercises the position gix, which it has to itself.

namespace {

static const uint64 RxStoreCacheSize = 100 * TInt::Mega;
static const uint64 RxIndexCacheSize = 100 * TInt::Mega;

static const char* ReindexSchemaStr =
    "[{ \"name\": \"Article\","
    "   \"fields\": [ { \"name\": \"URI\", \"type\": \"string\", \"primary\": true },"
    "                 { \"name\": \"Title\", \"type\": \"string\", \"store\": \"cache\" },"
    "                 { \"name\": \"Body\", \"type\": \"string\", \"store\": \"cache\" },"
    "                 { \"name\": \"Language\", \"type\": \"string\" } ],"
    "   \"joins\": [ { \"name\": \"hasConcept\", \"type\": \"index\", \"store\": \"Concept\", \"inverse\": \"hasArticle\", \"storage\": \"small\" } ],"
    "   \"keys\": [ { \"field\": \"Title\", \"type\": \"text\", \"storage\": \"small\" },"
    "               { \"field\": \"Body\", \"type\": \"text_position\" },"
    "               { \"field\": \"Language\", \"type\": \"value\", \"storage\": \"tiny\" } ],"
    "   \"options\": { \"type\": \"paged\" }"
    "},"
    "{  \"name\": \"Concept\","
    "   \"fields\": [ { \"name\": \"URI\", \"type\": \"string\", \"primary\": true },"
    "                 { \"name\": \"Label\", \"type\": \"string\" } ],"
    "   \"joins\": [ { \"name\": \"hasArticle\", \"type\": \"index\", \"store\": \"Article\", \"inverse\": \"hasConcept\", \"storage\": \"small\" } ],"
    "   \"options\": { \"type\": \"paged\" }"
    "}]";

// search a text/value key by a single word; 0 when the word is not even in the vocabulary
static int RxSearchWord(const PBase& Base, const TWPt<TStore>& Store,
        const TStr& KeyNm, const TStr& Word) {
    const int KeyId = Base->GetIndexVoc()->GetKeyId(Store->GetStoreId(), KeyNm);
    if (!Base->GetIndexVoc()->IsWordStr(KeyId, Word)) { return 0; }
    const uint64 WordId = Base->GetIndexVoc()->GetWordId(KeyId, Word);
    return (int) Base->GetIndex()->SearchGix(Base, KeyId, WordId)->GetRecs();
}

// search a text_position key: single word, or a phrase of adjacent words
static int RxSearchPos(const PBase& Base, const TWPt<TStore>& Store,
        const TStr& KeyNm, const TStrV& WordV) {
    const int KeyId = Base->GetIndexVoc()->GetKeyId(Store->GetStoreId(), KeyNm);
    TUInt64V WordIdV; TIntV MaxDiffV;
    for (int WordN = 0; WordN < WordV.Len(); WordN++) {
        if (!Base->GetIndexVoc()->IsWordStr(KeyId, WordV[WordN])) { return 0; }
        WordIdV.Add(Base->GetIndexVoc()->GetWordId(KeyId, WordV[WordN]));
        if (WordN > 0) { MaxDiffV.Add(1); }
    }
    return (int) Base->GetIndex()->SearchTextPos(Base, KeyId, WordIdV, MaxDiffV)->GetRecs();
}

static int RxSearchPos1(const PBase& Base, const TWPt<TStore>& Store,
        const TStr& KeyNm, const TStr& Word) {
    TStrV WordV; WordV.Add(Word);
    return RxSearchPos(Base, Store, KeyNm, WordV);
}

// number of articles joined to the given concept
static int RxConceptArticles(const PBase& Base, const TWPt<TStore>& ConceptStore, const uint64& ConceptId) {
    return (int) ConceptStore->GetRec(ConceptId).DoJoin(Base, "hasArticle")->GetRecs();
}

static void RxInitEnv() {
    if (!TUnicodeDef::IsDef()) { TUnicodeDef::Load("./src/glib/bin/UnicodeDef.Bin"); }
    if (!TQm::TEnv::IsInit()) { TQm::TEnv::Init(); }
}

// collect file names (without path) that belong to one gix instance (key hash +
// blob files, whose name TMBlobBs normalizes: "Index.GixPos" -> "Index_GixPos.mbb")
static void RxGetGixFileNames(const TStr& FPath, const TStr& GixNm, TStrV& FNmV) {
    FNmV.Clr();
    if (TFile::Exists(TPath::Combine(FPath, GixNm + ".Gix"))) {
        FNmV.Add(GixNm + ".Gix");
    }
    const TStr BlobNm = TStr::GetNrFMid(TStr(GixNm + ".GixDat").GetFMid());
    TFFile FFile(TPath::Combine(FPath, BlobNm + ".mbb*"), false);
    TStr FNm;
    while (FFile.Next(FNm)) {
        FNmV.Add(TDir::GetFileName(FNm));
    }
}

static void RxMoveFiles(const TStr& FromPath, const TStr& ToPath, const TStrV& FNmV) {
    for (int FNmN = 0; FNmN < FNmV.Len(); FNmN++) {
        TFile::Move(TPath::Combine(FromPath, FNmV[FNmN]), TPath::Combine(ToPath, FNmV[FNmN]));
    }
}

} // namespace

TEST(ReindexTests, ReindexTextKeysAndSwap) { try {
    const TStr FPath = "./test/cpp/data/reindex_base/";
    const TStr BuildFPath = "./test/cpp/data/reindex_build/";
    const TStr StageFPath = "./test/cpp/data/reindex_stage/";
    const TStr BackupFPath = "./test/cpp/data/reindex_backup/";
    if (TDir::Exists(FPath)) { TDir::DelNonEmptyDir(FPath); }
    if (TDir::Exists(BuildFPath)) { TDir::DelNonEmptyDir(BuildFPath); }
    if (TDir::Exists(StageFPath)) { TDir::DelNonEmptyDir(StageFPath); }
    if (TDir::Exists(BackupFPath)) { TDir::DelNonEmptyDir(BackupFPath); }
    TDir::GenDir(FPath);
    TDir::GenDir(BuildFPath);
    TDir::GenDir(StageFPath);
    TDir::GenDir(BackupFPath);

    RxInitEnv();

    const int Articles = 200;
    const int Concepts = 4;
    const int Langs = 2;

    // --- Phase 1: create the base; every title and body contains "junkword",
    // which the changed tokenizer will later drop as a stopword ----------------
    {
        PBase Base = NewBase(FPath, TJsonVal::GetValFromStr(ReindexSchemaStr),
            RxIndexCacheSize, RxStoreCacheSize, true);
        TWPt<TStore> ArticleStore = Base->GetStoreByStoreNm("Article");
        TWPt<TStore> ConceptStore = Base->GetStoreByStoreNm("Concept");
        for (int ConceptN = 0; ConceptN < Concepts; ConceptN++) {
            PJsonVal ConceptVal = TJsonVal::NewObj();
            ConceptVal->AddToObj("URI", TStr::Fmt("concept_%d", ConceptN));
            ConceptVal->AddToObj("Label", TStr::Fmt("Concept %d", ConceptN));
            ConceptStore->AddRec(ConceptVal);
        }
        for (int ArtN = 0; ArtN < Articles; ArtN++) {
            // even articles carry the phrase "phraseone phrasetwo" in order, odd
            // articles reversed - only the even ones match the adjacency query
            const char* Phrase = (ArtN % 2 == 0) ? "phraseone phrasetwo" : "phrasetwo phraseone";
            PJsonVal ArtVal = TJsonVal::NewObj();
            ArtVal->AddToObj("URI", TStr::Fmt("article_%d", ArtN));
            ArtVal->AddToObj("Title", TStr::Fmt("title uniqueword%d junkword common words", ArtN));
            ArtVal->AddToObj("Body", TStr::Fmt("body uniquebody%d junkword %s longer text of the article", ArtN, Phrase));
            ArtVal->AddToObj("Language", (ArtN % Langs == 0) ? "eng" : "deu");
            const uint64 ArtId = ArticleStore->AddRec(ArtVal);
            ArticleStore->AddJoin("hasConcept", ArtId, (uint64)(ArtN % Concepts));
        }

        // pre-reindex state: junkword is searchable everywhere
        EXPECT_EQ(RxSearchWord(Base, ArticleStore, "Title", "junkword"), Articles);
        EXPECT_EQ(RxSearchPos1(Base, ArticleStore, "Body", "junkword"), Articles);
        EXPECT_EQ(RxSearchWord(Base, ArticleStore, "Language", "eng"), Articles / Langs);
        TStrV PhraseV; PhraseV.Add("phraseone"); PhraseV.Add("phrasetwo");
        EXPECT_EQ(RxSearchPos(Base, ArticleStore, "Body", PhraseV), Articles / 2);

        SaveBase(Base);
    }

    // --- Phase 2: reopen read-only, change the tokenizer (junkword becomes a
    // stopword), rebuild the text keys, write the new files aside --------------
    TStrV RebuiltGixNmV;
    {
        PBase Base = LoadBase(FPath, faRdOnly, RxIndexCacheSize, RxStoreCacheSize);
        TWPt<TStore> ArticleStore = Base->GetStoreByStoreNm("Article");
        const PIndexVoc IndexVoc = Base->GetIndexVoc();
        const int TitleKeyId = IndexVoc->GetKeyId(ArticleStore->GetStoreId(), "Title");
        const int BodyKeyId = IndexVoc->GetKeyId(ArticleStore->GetStoreId(), "Body");

        // the "tokenizer change": same tokenizer type, junkword now a stopword
        // (mirrors TNewsBase::InitTokenizer, which re-sets the key tokenizers
        // with the current stopword file on every open)
        // the set stores uppercase words - THtml looks the uppercased token up directly
        const PTokenizer NewTokenizer = TTokenizers::THtmlUnicode::New(TSwSet::NewFromWords("JUNKWORD"));
        IndexVoc->PutTokenizer(TitleKeyId, NewTokenizer);
        IndexVoc->PutTokenizer(BodyKeyId, NewTokenizer);

        TIntSet TextKeyIdSet;
        TextKeyIdSet.AddKey(TitleKeyId);
        TextKeyIdSet.AddKey(BodyKeyId);
        PIndexVoc CloneVoc = IndexVoc->CloneWithFreshWordVocs(TextKeyIdSet);
        // the cleared vocabularies start empty, the others are shared
        EXPECT_EQ((int) CloneVoc->GetWords(TitleKeyId), 0);
        EXPECT_EQ((int) CloneVoc->GetWords(BodyKeyId), 0);
        const int LangKeyId = IndexVoc->GetKeyId(ArticleStore->GetStoreId(), "Language");
        EXPECT_EQ(CloneVoc->GetWords(LangKeyId), IndexVoc->GetWords(LangKeyId));

        // reindex the text fields into a stage index, in record order
        PIndex StageIndex = TIndex::New(StageFPath, faCreate, CloneVoc,
            10 * TInt::Mega, 10 * TInt::Mega, 10 * TInt::Mega, 10 * TInt::Mega,
            Base->GetIndex()->GetSplitLen());
        const int TitleFieldId = ArticleStore->GetFieldId("Title");
        const int BodyFieldId = ArticleStore->GetFieldId("Body");
        PStoreIter Iter = ArticleStore->GetIter();
        while (Iter->Next()) {
            const uint64 RecId = Iter->GetRecId();
            StageIndex->IndexText(TitleKeyId, ArticleStore->GetFieldStr(RecId, TitleFieldId), RecId);
            StageIndex->IndexTextPos(BodyKeyId, ArticleStore->GetFieldStr(RecId, BodyFieldId), RecId);
        }
        // the fresh vocabularies no longer contain the stopword
        EXPECT_FALSE(CloneVoc->IsWordStr(TitleKeyId, "junkword"));
        EXPECT_FALSE(CloneVoc->IsWordStr(BodyKeyId, "junkword"));
        EXPECT_TRUE(CloneVoc->IsWordStr(TitleKeyId, "common"));

        // combine live + stage into the build folder, with deep verification
        Base->GetIndex()->ReindexCopyGix(StageIndex, BuildFPath, TextKeyIdSet,
            10 * TInt::Mega, 1000, RebuiltGixNmV);
        // Title (text, small gix) and Body (text_position, pos gix) were rebuilt
        ASSERT_EQ(RebuiltGixNmV.Len(), 2);
        EXPECT_EQ(RebuiltGixNmV[0], "Index.GixSmall");
        EXPECT_EQ(RebuiltGixNmV[1], "Index.GixPos");

        // write the matching vocabulary next to the rebuilt gix files
        TFOut VocFOut(TPath::Combine(BuildFPath, "IndexVoc.dat"));
        CloneVoc->Save(VocFOut);
    }

    // --- Phase 3: swap the rebuilt files in (as the console action does) ------
    for (int GixNmN = 0; GixNmN < RebuiltGixNmV.Len(); GixNmN++) {
        TStrV OldFNmV; RxGetGixFileNames(FPath, RebuiltGixNmV[GixNmN], OldFNmV);
        EXPECT_GE(OldFNmV.Len(), 2);
        RxMoveFiles(FPath, BackupFPath, OldFNmV);
        TStrV NewFNmV; RxGetGixFileNames(BuildFPath, RebuiltGixNmV[GixNmN], NewFNmV);
        EXPECT_GE(NewFNmV.Len(), 2);
        RxMoveFiles(BuildFPath, FPath, NewFNmV);
    }
    TFile::Move(TPath::Combine(FPath, "IndexVoc.dat"), TPath::Combine(BackupFPath, "IndexVoc.dat"));
    TFile::Move(TPath::Combine(BuildFPath, "IndexVoc.dat"), TPath::Combine(FPath, "IndexVoc.dat"));

    // --- Phase 4: reload and verify ------------------------------------------
    {
        PBase Base = LoadBase(FPath, faRdOnly, RxIndexCacheSize, RxStoreCacheSize);
        TWPt<TStore> ArticleStore = Base->GetStoreByStoreNm("Article");
        TWPt<TStore> ConceptStore = Base->GetStoreByStoreNm("Concept");

        // the stopword is gone from vocabulary and postings of both text keys
        EXPECT_EQ(RxSearchWord(Base, ArticleStore, "Title", "junkword"), 0);
        EXPECT_EQ(RxSearchPos1(Base, ArticleStore, "Body", "junkword"), 0);
        const int TitleKeyId = Base->GetIndexVoc()->GetKeyId(ArticleStore->GetStoreId(), "Title");
        EXPECT_FALSE(Base->GetIndexVoc()->IsWordStr(TitleKeyId, "junkword"));

        // all other words are still searchable with the same results
        for (int ArtN = 0; ArtN < Articles; ArtN++) {
            ASSERT_EQ(RxSearchWord(Base, ArticleStore, "Title", TStr::Fmt("uniqueword%d", ArtN)), 1);
            ASSERT_EQ(RxSearchPos1(Base, ArticleStore, "Body", TStr::Fmt("uniquebody%d", ArtN)), 1);
        }
        EXPECT_EQ(RxSearchWord(Base, ArticleStore, "Title", "common"), Articles);
        // word frequencies were rebuilt along with the postings
        EXPECT_EQ((int) Base->GetIndexVoc()->GetWordFq(TitleKeyId,
            Base->GetIndexVoc()->GetWordId(TitleKeyId, "common")), Articles);

        // positions survived: the adjacency phrase still matches only the even articles
        TStrV PhraseV; PhraseV.Add("phraseone"); PhraseV.Add("phrasetwo");
        EXPECT_EQ(RxSearchPos(Base, ArticleStore, "Body", PhraseV), Articles / 2);

        // the untouched keys are intact: Language value key (tiny gix) and the
        // hasConcept/hasArticle join postings that share the small gix with Title
        EXPECT_EQ(RxSearchWord(Base, ArticleStore, "Language", "eng"), Articles / Langs);
        EXPECT_EQ(RxSearchWord(Base, ArticleStore, "Language", "deu"), Articles / Langs);
        for (int ConceptN = 0; ConceptN < Concepts; ConceptN++) {
            EXPECT_EQ(RxConceptArticles(Base, ConceptStore, (uint64) ConceptN), Articles / Concepts);
        }
    }

    TDir::DelNonEmptyDir(FPath);
    TDir::DelNonEmptyDir(BuildFPath);
    if (TDir::Exists(StageFPath)) { TDir::DelNonEmptyDir(StageFPath); }
    TDir::DelNonEmptyDir(BackupFPath);
} catch (PExcept E) { FAIL() << E->GetMsgStr().CStr(); } }

TEST(ReindexTests, CloneWithFreshWordVocsGuardsSharedVoc) { try {
    const TStr FPath = "./test/cpp/data/reindex_sharedvoc/";
    if (TDir::Exists(FPath)) { TDir::DelNonEmptyDir(FPath); }
    TDir::GenDir(FPath);

    RxInitEnv();

    // two value keys share one named vocabulary
    const char* SchemaStr =
        "[{ \"name\": \"Item\","
        "   \"fields\": [ { \"name\": \"Name\", \"type\": \"string\", \"primary\": true },"
        "                 { \"name\": \"ValueA\", \"type\": \"string\" },"
        "                 { \"name\": \"ValueB\", \"type\": \"string\" } ],"
        "   \"keys\": [ { \"field\": \"ValueA\", \"type\": \"value\", \"vocabulary\": \"SharedVoc\", \"storage\": \"small\" },"
        "               { \"field\": \"ValueB\", \"type\": \"value\", \"vocabulary\": \"SharedVoc\", \"storage\": \"small\" } ]"
        "}]";
    // scoped so the base is closed before its directory is deleted below
    {
        PBase Base = NewBase(FPath, TJsonVal::GetValFromStr(SchemaStr),
            RxIndexCacheSize, RxStoreCacheSize, true);
        TWPt<TStore> Store = Base->GetStoreByStoreNm("Item");
        const PIndexVoc IndexVoc = Base->GetIndexVoc();
        const int KeyAId = IndexVoc->GetKeyId(Store->GetStoreId(), "ValueA");

        // clearing only ValueA's vocabulary would leave ValueB's postings with
        // dangling word ids - the clone must refuse
        TIntSet KeyIdSet; KeyIdSet.AddKey(KeyAId);
        EXPECT_THROW(IndexVoc->CloneWithFreshWordVocs(KeyIdSet), PExcept);

        // clearing both sharers together is fine
        const int KeyBId = IndexVoc->GetKeyId(Store->GetStoreId(), "ValueB");
        KeyIdSet.AddKey(KeyBId);
        PIndexVoc CloneVoc = IndexVoc->CloneWithFreshWordVocs(KeyIdSet);
        EXPECT_EQ((int) CloneVoc->GetWords(KeyAId), 0);

        SaveBase(Base);
    }

    TDir::DelNonEmptyDir(FPath);
} catch (PExcept E) { FAIL() << E->GetMsgStr().CStr(); } }
