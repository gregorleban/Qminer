#include <qminer.h>
#include <qminer_storage.h>

#include "gtest/gtest.h"

using namespace TQm;
using namespace TQm::TStorage;

static const uint64 StoreCacheSize = 100 * TInt::Mega;
static const uint64 IndexCacheSize = 100 * TInt::Mega;
static const TStr DataDir  = "./test/cpp/data/";
static const TStr DefFile  = "./test/cpp/files/store_batch_delete.def";

// ---- helpers ----------------------------------------------------------------

static PJsonVal MakeArticle(int Id, const TStr& Lang = "eng") {
    PJsonVal V = TJsonVal::NewObj();
    V->AddToObj("URI",      TStr::Fmt("bd_article_%d", Id));
    V->AddToObj("Title",    TStr::Fmt("title uniqueword%d common words here", Id));
    V->AddToObj("Body",     TStr::Fmt("body uniquebody%d text for article written in language %s", Id, Lang.CStr()));
    V->AddToObj("Language", Lang);
    V->AddToObj("DateTime", "2024-01-01T00:00:00");
    return V;
}

static PJsonVal MakeSource(int Id) {
    PJsonVal V = TJsonVal::NewObj();
    V->AddToObj("URI",   TStr::Fmt("bd_source_%d", Id));
    V->AddToObj("Title", TStr::Fmt("Source Number %d", Id));
    return V;
}

static PJsonVal MakeConcept(int Id) {
    PJsonVal V = TJsonVal::NewObj();
    V->AddToObj("URI",   TStr::Fmt("bd_concept_%d", Id));
    V->AddToObj("Label", TStr::Fmt("Concept %d", Id));
    return V;
}

// Search a text/value index key by word; return number of matching records.
static int SearchWord(const PBase& Base, const PStore& Store,
                      const TStr& FieldNm, const TStr& Word) {
    const int KeyId = Base->GetIndexVoc()->GetKeyId(Store->GetStoreId(), FieldNm);
    if (!Base->GetIndexVoc()->IsWordStr(KeyId, Word)) { return 0; }
    const uint64 WordId = Base->GetIndexVoc()->GetWordId(KeyId, Word);
    PRecSet Rs = Base->GetIndex()->SearchGix(Base, KeyId, WordId);
    return (int)Rs->GetRecs();
}

// Search a text_position index key by word; return number of matching records.
static int SearchWordPos(const PBase& Base, const PStore& Store,
                         const TStr& FieldNm, const TStr& Word) {
    const int KeyId = Base->GetIndexVoc()->GetKeyId(Store->GetStoreId(), FieldNm);
    if (!Base->GetIndexVoc()->IsWordStr(KeyId, Word)) { return 0; }
    TUInt64V WordIdV;
    WordIdV.Add(Base->GetIndexVoc()->GetWordId(KeyId, Word));
    TIntV MaxDiffV; // empty for single-word query
    PRecSet Rs = Base->GetIndex()->SearchTextPos(Base, KeyId, WordIdV, MaxDiffV);
    return (int)Rs->GetRecs();
}

// Number of articles linked to a concept via hasArticle join.
static int ConceptArticleCount(const PBase& Base,
                               const PStore& ConceptStore, uint64 ConceptId) {
    return (int)ConceptStore->GetRec(ConceptId).DoJoin(Base, "hasArticle")->GetRecs();
}

// ---- test init --------------------------------------------------------------

class TBatchDeleteEnv : public ::testing::Environment {
public:
    void SetUp() override {
        TUnicodeDef::Load("./src/glib/bin/UnicodeDef.Bin");
        TQm::TEnv::Init();
        TQm::TEnv::InitLogger(0, "std", true);
    }
};

static ::testing::Environment* const gEnv =
    ::testing::AddGlobalTestEnvironment(new TBatchDeleteEnv);

// ---- main test --------------------------------------------------------------

TEST(TBatchDelete, Basic) { try {
    const int TotalArticles = 20;
    const int TotalSources  = 3;
    const int TotalConcepts = 4;
    // articles 0..9 will be batch-deleted; 10..19 survive
    const int DeleteCount   = 10;

    PJsonVal StoreDefVal = TJsonVal::GetValFromStr(TStr::LoadTxt(DefFile));

    // --- Phase 1: create database, add records and joins ---------------------
    PBase Base = NewBase(DataDir, StoreDefVal, IndexCacheSize, StoreCacheSize,
                            true, TStrUInt64H(), TStrUInt64H(), false, 128, true);
    PStore ArticleStore = Base->GetStoreByStoreNm("Article");
    PStore SourceStore  = Base->GetStoreByStoreNm("Source");
    PStore ConceptStore = Base->GetStoreByStoreNm("Concept");

    for (int i = 0; i < TotalSources;  i++) { SourceStore->AddRec(MakeSource(i)); }
    for (int i = 0; i < TotalConcepts; i++) { ConceptStore->AddRec(MakeConcept(i)); }

    for (int i = 0; i < TotalArticles; i++) {
        const TStr Lang = (i % 2 == 0) ? "eng" : "deu";
        const uint64 ArtId = ArticleStore->AddRec(MakeArticle(i, Lang));
        const uint64 SrcId = SourceStore->GetRecId(TStr::Fmt("bd_source_%d", i % TotalSources));
        const uint64 ConId = ConceptStore->GetRecId(TStr::Fmt("bd_concept_%d", i % TotalConcepts));
        ArticleStore->AddJoin("hasSource",  ArtId, SrcId);
        ArticleStore->AddJoin("hasConcept", ArtId, ConId);
    }

    ASSERT_EQ((int)ArticleStore->GetRecs(), TotalArticles);
    ASSERT_EQ((int)SourceStore->GetRecs(),  TotalSources);
    ASSERT_EQ((int)ConceptStore->GetRecs(), TotalConcepts);

    // all unique title words must be in the index
    for (int i = 0; i < TotalArticles; i++) {
        ASSERT_GE(SearchWord(Base, ArticleStore, "Title", TStr::Fmt("uniqueword%d", i)), 1);
    }
    // all unique body words must be in the text_position index
    for (int i = 0; i < TotalArticles; i++) {
        ASSERT_GE(SearchWordPos(Base, ArticleStore, "Body", TStr::Fmt("uniquebody%d", i)), 1);
    }
    // language value index
    ASSERT_EQ(SearchWord(Base, ArticleStore, "Language", "eng"), TotalArticles / 2);
    ASSERT_EQ(SearchWord(Base, ArticleStore, "Language", "deu"), TotalArticles / 2);

    // concept 0 is used by articles 0, 4, 8, 12, 16 → 5
    uint64 Con0 = ConceptStore->GetRecId("bd_concept_0");
    ASSERT_EQ(ConceptArticleCount(Base, ConceptStore, Con0), TotalArticles / TotalConcepts);

    TUInt64V DelIdV;
    for (int i = 0; i < DeleteCount; i++) {
        const TStr Uri = TStr::Fmt("bd_article_%d", i);
        ASSERT_TRUE(ArticleStore->IsRecNm(Uri));
        DelIdV.Add(ArticleStore->GetRecId(Uri));
    }

    ArticleStore->BatchDeleteRecs(DelIdV);
    //SaveBase(Base);

    // store counts
    ASSERT_EQ((int)ArticleStore->GetRecs(), TotalArticles - DeleteCount);

    // the deleted articles should not be returned by the iterator
    PStoreIter Iter = ArticleStore->GetIter();
    for (int N = 0; Iter->Next(); N++) {
        const uint64 ArticleId = Iter->GetRecId();
		ASSERT_FALSE(DelIdV.IsIn(ArticleId));
    }

    // deleted articles gone from store
    for (int i = 0; i < DeleteCount; i++) {
        ASSERT_FALSE(ArticleStore->IsRecId(DelIdV[i]));
        ASSERT_FALSE(ArticleStore->IsRecNm(TStr::Fmt("bd_article_%d", i)));
    }

    // surviving articles still present
    for (int i = DeleteCount; i < TotalArticles; i++) {
        ASSERT_TRUE(ArticleStore->IsRecNm(TStr::Fmt("bd_article_%d", i)));
    }

    // text index cleaned up for deleted articles
    for (int i = 0; i < DeleteCount; i++) {
        ASSERT_EQ(SearchWord(Base, ArticleStore, "Title", TStr::Fmt("uniqueword%d", i)), 0);
    }
    // text_position (Body) index cleaned up for deleted articles
    for (int i = 0; i < DeleteCount; i++) {
        ASSERT_EQ(SearchWordPos(Base, ArticleStore, "Body", TStr::Fmt("uniquebody%d", i)), 0);
    }

    // text index intact for surviving articles
    for (int i = DeleteCount; i < TotalArticles; i++) {
        ASSERT_GE(SearchWord(Base, ArticleStore, "Title", TStr::Fmt("uniqueword%d", i)), 1);
    }
    // text_position (Body) index intact for surviving articles
    for (int i = DeleteCount; i < TotalArticles; i++) {
        ASSERT_GE(SearchWordPos(Base, ArticleStore, "Body", TStr::Fmt("uniquebody%d", i)), 1);
    }

    // language index: 5 eng deleted (articles 0,2,4,6,8), 5 deu deleted (1,3,5,7,9)
    ASSERT_EQ(SearchWord(Base, ArticleStore, "Language", "eng"), TotalArticles / 2 - DeleteCount / 2);
    ASSERT_EQ(SearchWord(Base, ArticleStore, "Language", "deu"), TotalArticles / 2 - DeleteCount / 2);

    // source inverse join must not contain any deleted article ID
    for (int s = 0; s < TotalSources; s++) {
        const uint64 SrcId = SourceStore->GetRecId(TStr::Fmt("bd_source_%d", s));
        PRecSet SrcArts = SourceStore->GetRec(SrcId).DoJoin(Base, "hasArticle");
        TUInt64V SrcArtIds; SrcArts->GetRecIdV(SrcArtIds);
        for (int d = 0; d < DelIdV.Len(); d++) {
            ASSERT_FALSE(SrcArtIds.IsIn(DelIdV[d]));
        }
    }

    // concept 0 had articles 0,4,8,12,16; articles 0,4,8 are deleted → 2 remain
    ASSERT_EQ(ConceptArticleCount(Base, ConceptStore, Con0), 2);

    // re-add with a different language so we can verify index has new entries
    for (int i = 0; i < DeleteCount; i++) {
        const uint64 NewId = ArticleStore->AddRec(MakeArticle(i, "fra"));
        const uint64 ConId = ConceptStore->GetRecId(TStr::Fmt("bd_concept_%d", i % TotalConcepts));
        ArticleStore->AddJoin("hasConcept", NewId, ConId);
    }

    ASSERT_EQ((int)ArticleStore->GetRecs(), TotalArticles);
    // URI lookup uses in-memory primary-field map — works before flush
    for (int i = 0; i < DeleteCount; i++) {
        ASSERT_TRUE(ArticleStore->IsRecNm(TStr::Fmt("bd_article_%d", i)));
    }

    // new records in text index
    for (int i = 0; i < DeleteCount; i++) {
        ASSERT_GE(SearchWord(Base, ArticleStore, "Title", TStr::Fmt("uniqueword%d", i)), 1);
    }
    // new records in text_position (Body) index
    for (int i = 0; i < DeleteCount; i++) {
        ASSERT_GE(SearchWordPos(Base, ArticleStore, "Body", TStr::Fmt("uniquebody%d", i)), 1);
    }

    // new records under the "fra" language key
    ASSERT_EQ(SearchWord(Base, ArticleStore, "Language", "fra"), DeleteCount);

    // old surviving articles still searchable
    for (int i = DeleteCount; i < TotalArticles; i++) {
        ASSERT_GE(SearchWord(Base, ArticleStore, "Title", TStr::Fmt("uniqueword%d", i)), 1);
    }
    // old surviving articles still in text_position (Body) index
    for (int i = DeleteCount; i < TotalArticles; i++) {
        ASSERT_GE(SearchWordPos(Base, ArticleStore, "Body", TStr::Fmt("uniquebody%d", i)), 1);
    }

    // concept 0 article count restored to original (0,4,8 re-added + 12,16 remain = 5)
    Con0 = ConceptStore->GetRecId("bd_concept_0");
    ASSERT_EQ(ConceptArticleCount(Base, ConceptStore, Con0), TotalArticles / TotalConcepts);


    printf("TBatchDeleteBasic: all assertions passed.\n");
} catch (PExcept& Ex) { FAIL() << Ex->GetMsgStr().CStr(); } }

// ---- calling BatchDeleteRecs with already-deleted IDs must not crash --------

TEST(TBatchDelete, Idempotent) { try {
    PJsonVal StoreDefVal = TJsonVal::GetValFromStr(TStr::LoadTxt(DefFile));
    PBase Base = NewBase(DataDir, StoreDefVal, IndexCacheSize, StoreCacheSize,
                         true, TStrUInt64H(), TStrUInt64H(), false, 128, true);
    PStore ArticleStore = Base->GetStoreByStoreNm("Article");

    TUInt64V IdV;
    for (int i = 0; i < 5; i++) { IdV.Add(ArticleStore->AddRec(MakeArticle(200 + i))); }
    ASSERT_EQ((int)ArticleStore->GetRecs(), 5);

    // first delete
    ArticleStore->BatchDeleteRecs(IdV);
    ASSERT_EQ((int)ArticleStore->GetRecs(), 0);

    // calling again with the same (now stale) IDs must not crash
    ArticleStore->BatchDeleteRecs(IdV);
    ASSERT_EQ((int)ArticleStore->GetRecs(), 0);

    printf("TBatchDeleteIdempotent: all assertions passed.\n");
} catch (PExcept& Ex) { FAIL() << Ex->GetMsgStr().CStr(); } }
