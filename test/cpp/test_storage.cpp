#include <qminer.h>
#include <qminer_storage.h>

#include "gtest/gtest.h"

using namespace TQm;
using namespace TQm::TStorage;

const uint64 StoreCacheSize = 100 * TInt::Mega;
const uint64 IndexCacheSize = 100 * TInt::Mega;

PJsonVal MakeArticleSmall(const int& Id) {
    PJsonVal ArticleVal = TJsonVal::NewObj();
    ArticleVal->AddToObj("URI", TStr::Fmt("URI_%d", Id));
    ArticleVal->AddToObj("Url", "The Url");
    ArticleVal->AddToObj("Title", "The Title");
    ArticleVal->AddToObj("Body", "The Body");
    ArticleVal->AddToObj("int_v", TJsonVal::NewArr(TIntV::GetV(1, 2, 3)));
    ArticleVal->AddToObj("string", "tralala");
    ArticleVal->AddToObj("string_v", TJsonVal::NewArr(TStrV::GetV("a", "b", "c")));
    ArticleVal->AddToObj("float_v", TJsonVal::NewArr(TFltV::GetV(1.0, 2.0, 3.0)));
    return ArticleVal;
}

TStr MakeStr(const int& Len) {
    TStr Str = "1234567890";
    TChA ChA; while (ChA.Len() < Len) { ChA += Str; }
    return ChA;
}

PJsonVal MakeArticleBig(const int& Id, const int& FieldLen) {
    const TStr FieldStr = MakeStr(FieldLen);
    PJsonVal ArticleVal = TJsonVal::NewObj();
    ArticleVal->AddToObj("URI", TStr::Fmt("URI_%d", Id));
    ArticleVal->AddToObj("Url", FieldStr);
    ArticleVal->AddToObj("Title", FieldStr);
    ArticleVal->AddToObj("Body", FieldStr);
    ArticleVal->AddToObj("int_v", TJsonVal::NewArr(TIntV::GetV(1, 2, 3)));
    ArticleVal->AddToObj("string", "tralala");
    ArticleVal->AddToObj("string_v", TJsonVal::NewArr(TStrV::GetV("a", "b", "c")));
    ArticleVal->AddToObj("float_v", TJsonVal::NewArr(TFltV::GetV(1.0, 2.0, 3.0)));
    return ArticleVal;
}

PJsonVal MakeArticleBigMany(const int& Id, const int& FieldLen) {
    const TStr FieldStr = MakeStr(FieldLen);
    PJsonVal ArticleVal = TJsonVal::NewObj();
    ArticleVal->AddToObj("URI", TStr::Fmt("URI_%d", Id));
    ArticleVal->AddToObj("Url", FieldStr);
    ArticleVal->AddToObj("Title", FieldStr);
    ArticleVal->AddToObj("Body01", FieldStr);
    ArticleVal->AddToObj("Body02", FieldStr);
    ArticleVal->AddToObj("Body03", FieldStr);
    ArticleVal->AddToObj("Body04", FieldStr);
    ArticleVal->AddToObj("Body05", FieldStr);
    ArticleVal->AddToObj("Body06", FieldStr);
    ArticleVal->AddToObj("Body07", FieldStr);
    ArticleVal->AddToObj("Body08", FieldStr);
    ArticleVal->AddToObj("Body09", FieldStr);
    ArticleVal->AddToObj("Body10", FieldStr);
    return ArticleVal;
}

double FileSize() {
    return TFile::GetSize("./test/cpp/data/ArticlePgBlob.bin000") * 1.0;
}

TEST(Storage, TInit) {
    // guard against double-initialization: when the whole suite runs in one
    // process, earlier tests may have loaded/initialized these already
    if (!TUnicodeDef::IsDef()) {
        TUnicodeDef::Load("./src/glib/bin/UnicodeDef.Bin");
    }
    if (!TQm::TEnv::IsInit()) {
        TQm::TEnv::Init();
        TQm::TEnv::InitLogger(0, "std", true);
    }
}

TEST(Storage, TStore1) {
    // return;

    TStr StoreJsonFNm = "./test/cpp/files/store1.def";
    PJsonVal StoreDefVal = TJsonVal::GetValFromStr(TStr::LoadTxt(StoreJsonFNm));

    int ArticleId = 0;
    uint64 Records = 0;
    const int ArticleFieldLen = 1000;
    const int Recs = 10000;
    const double PercDelete = 0.5;
    const double PercAdd = 1.0;
    const int Iters = 10;

    try {
        PBase Base = NewBase("./test/cpp/data/", StoreDefVal, IndexCacheSize,
            StoreCacheSize, true, TStrUInt64H(), TStrUInt64H(), false, 128, true);
        PStore ArticleStore = Base->GetStoreByStoreNm("Article");

        for (int i = 0; i < Recs; i++) {
            ArticleStore->AddRec(MakeArticleBig(ArticleId++, ArticleFieldLen));
        }
        printf("Added %d records\n", Recs);

        Records = ArticleStore->GetRecs();
        SaveBase(Base);
    } catch (PExcept Except) {
        printf("Exception: %s\n", Except->GetMsgStr().CStr());
    }

    const double RecSize = FileSize() / Records;
    printf("File size: %.0fMB    %.0f / article\n", FileSize() / (1024.0*1024.0), FileSize() / Records);

    for (int IterN = 0; IterN < Iters; IterN++) {
        printf("Iter: %d\n", IterN);

        try {
            PBase Base = LoadBase("./test/cpp/data/", faUpdate, IndexCacheSize,
                StoreCacheSize, TStrUInt64H(), TStrUInt64H(), false, 128);
            PStore ArticleStore = Base->GetStoreByStoreNm("Article");

            PRecSet UpdateSet = ArticleStore->GetRndRecs((int)(PercDelete * Recs));
            TUInt64V UpdateRecIdV; UpdateSet->GetRecIdV(UpdateRecIdV);
            const TStr Field1Str = MakeStr(ArticleFieldLen + 1);
            const TStr Field2Str = MakeStr(ArticleFieldLen - 1);
            for (const uint64 RecId : UpdateRecIdV) {
                ArticleStore->SetFieldNmStr(RecId, "Title", Field1Str);
                ArticleStore->SetFieldNmStr(RecId, "Body", Field2Str);
                ArticleStore->SetFieldNmNull(RecId, "string");
                ArticleStore->SetFieldNmNull(RecId, "int_v");
                ArticleStore->SetFieldNmStr(RecId, "string", "tralala");
            }

            Records = ArticleStore->GetRecs();
            SaveBase(Base);
        } catch (PExcept Except) {
            printf("Exception: %s\n", Except->GetMsgStr().CStr());
        }
        printf("File size: %.0fMB  ...  %.1fx\n", FileSize() / (1024.0*1024.0), (FileSize() / Records) / RecSize);

        try {
            PBase Base = LoadBase("./test/cpp/data/", faUpdate, IndexCacheSize,
                StoreCacheSize, TStrUInt64H(), TStrUInt64H(), false, 128);
            PStore ArticleStore = Base->GetStoreByStoreNm("Article");

            PRecSet DeleteSet = ArticleStore->GetRndRecs((int)(PercDelete * Recs));
            TUInt64V DelRecIdV; DeleteSet->GetRecIdV(DelRecIdV);
            ArticleStore->DeleteRecs(DelRecIdV);

            Records = ArticleStore->GetRecs();
            SaveBase(Base);
        } catch (PExcept Except) {
            printf("Exception: %s\n", Except->GetMsgStr().CStr());
        }

        printf("File size: %.0fMB  ...  %.1fx\n", FileSize() / (1024.0*1024.0), (FileSize() / Records) / RecSize);

        try {
            PBase Base = LoadBase("./test/cpp/data/", faUpdate, IndexCacheSize,
                StoreCacheSize, TStrUInt64H(), TStrUInt64H(), false, 128);
            PStore ArticleStore = Base->GetStoreByStoreNm("Article");

            for (int i = 0; i < (int)(PercAdd * Recs); i++) {
                ArticleStore->AddRec(MakeArticleBig(ArticleId++, ArticleFieldLen));
            }

            Records = ArticleStore->GetRecs();
            SaveBase(Base);
        } catch (PExcept Except) {
            printf("Exception: %s\n", Except->GetMsgStr().CStr());
        }

        printf("File size: %.0fMB  ...  %.1fx\n", FileSize() / (1024.0*1024.0), (FileSize() / Records) / RecSize);
    }
}


TEST(Storage, TStore2) {
    TStr StoreJsonFNm = "./test/cpp/files/store1.def";
    PJsonVal StoreDefVal = TJsonVal::GetValFromStr(TStr::LoadTxt(StoreJsonFNm));

    try {
        PBase Base = NewBase("./test/cpp/data/", StoreDefVal, IndexCacheSize,
            StoreCacheSize, true, TStrUInt64H(), TStrUInt64H(), false, 128, true);
        PStore ArticleStore = Base->GetStoreByStoreNm("Article");

        ArticleStore->AddRec(MakeArticleBig(123, 10000));

        printf("Records: %d\n", (int)ArticleStore->GetRecs());

        SaveBase(Base);
    } catch (PExcept Except) {
        printf("Exception: %s\n", Except->GetMsgStr().CStr());
    }

    try {
        PBase Base = LoadBase("./test/cpp/data/", faUpdate, IndexCacheSize,
            StoreCacheSize, TStrUInt64H(), TStrUInt64H(), false, 128);
        PStore ArticleStore = Base->GetStoreByStoreNm("Article");

        TUInt64V DelRecIdV; ArticleStore->GetAllRecs()->GetRecIdV(DelRecIdV);
        ArticleStore->DeleteRecs(DelRecIdV);
        printf("Records: %d\n", (int)ArticleStore->GetRecs());

        SaveBase(Base);
    } catch (PExcept Except) {
        printf("Exception: %s\n", Except->GetMsgStr().CStr());
    }
}

TEST(Storage, TStore3) {
    TStr StoreJsonFNm = "./test/cpp/files/store1.def";
    PJsonVal StoreDefVal = TJsonVal::GetValFromStr(TStr::LoadTxt(StoreJsonFNm));

    try {
        PBase Base = NewBase("./test/cpp/data/", StoreDefVal, IndexCacheSize,
            StoreCacheSize, true, TStrUInt64H(), TStrUInt64H(), false, 128, true);
        PStore ArticleStore = Base->GetStoreByStoreNm("ArticleMany");

        ArticleStore->AddRec(MakeArticleBigMany(123, 500));

        printf("Records: %d\n", (int)ArticleStore->GetRecs());

        SaveBase(Base);
    } catch (PExcept Except) {
        printf("Exception: %s\n", Except->GetMsgStr().CStr());
    }
}

TEST(Storage, TStore4) {
    TStr StoreJsonFNm = "./test/cpp/files/store1.def";
    PJsonVal StoreDefVal = TJsonVal::GetValFromStr(TStr::LoadTxt(StoreJsonFNm));

    try {
        PBase Base = NewBase("./test/cpp/data/", StoreDefVal, IndexCacheSize,
            StoreCacheSize, true, TStrUInt64H(), TStrUInt64H(), false, 128, true);
        PStore ArticleStore = Base->GetStoreByStoreNm("Article");

        const int ArticleId = ArticleStore->AddRec(MakeArticleBig(123, 10000));

        ArticleStore->SetFieldNmStr(ArticleId, "string", MakeStr(1));
        ArticleStore->SetFieldNmStr(ArticleId, "string", MakeStr(11));
        ArticleStore->SetFieldNmStr(ArticleId, "string", MakeStr(111));
        ArticleStore->SetFieldNmStr(ArticleId, "string", MakeStr(1111));
        ArticleStore->SetFieldNmStr(ArticleId, "string", MakeStr(11111));
        ArticleStore->SetFieldNmStr(ArticleId, "string", MakeStr(1111));
        ArticleStore->SetFieldNmStr(ArticleId, "string", MakeStr(111));
        ArticleStore->SetFieldNmStr(ArticleId, "string", MakeStr(11));
        ArticleStore->SetFieldNmStr(ArticleId, "string", MakeStr(1));

        ArticleStore->SetFieldNmStrV(ArticleId, "string_v", TStrV::GetV("A"));
        ArticleStore->SetFieldNmStrV(ArticleId, "string_v", TStrV::GetV("A", "B"));
        ArticleStore->SetFieldNmStrV(ArticleId, "string_v", TStrV::GetV("A", "B", "C"));
        ArticleStore->SetFieldNmStrV(ArticleId, "string_v", TStrV::GetV("A", "B", "C", "D"));
        ArticleStore->SetFieldNmStrV(ArticleId, "string_v", TStrV::GetV("A", "B", "C", "D", "E"));
        ArticleStore->SetFieldNmStrV(ArticleId, "string_v", TStrV::GetV("A", "B", "C", "D"));
        ArticleStore->SetFieldNmStrV(ArticleId, "string_v", TStrV::GetV("A", "B", "C"));
        ArticleStore->SetFieldNmStrV(ArticleId, "string_v", TStrV::GetV("A", "B"));
        ArticleStore->SetFieldNmStrV(ArticleId, "string_v", TStrV::GetV("A"));

        ArticleStore->SetFieldNmIntV(ArticleId, "int_v", TIntV::GetV(1));
        ArticleStore->SetFieldNmIntV(ArticleId, "int_v", TIntV::GetV(1, 2));
        ArticleStore->SetFieldNmIntV(ArticleId, "int_v", TIntV::GetV(1, 2, 3));
        ArticleStore->SetFieldNmIntV(ArticleId, "int_v", TIntV::GetV(1, 2, 3, 4));
        ArticleStore->SetFieldNmIntV(ArticleId, "int_v", TIntV::GetV(1, 2, 3, 4, 5));
        ArticleStore->SetFieldNmIntV(ArticleId, "int_v", TIntV::GetV(1, 2, 3, 4));
        ArticleStore->SetFieldNmIntV(ArticleId, "int_v", TIntV::GetV(1, 2, 3));
        ArticleStore->SetFieldNmIntV(ArticleId, "int_v", TIntV::GetV(1, 2));
        ArticleStore->SetFieldNmIntV(ArticleId, "int_v", TIntV::GetV(1));

        ArticleStore->SetFieldNmFltV(ArticleId, "float_v", TFltV::GetV(1.0));
        ArticleStore->SetFieldNmFltV(ArticleId, "float_v", TFltV::GetV(1.0, 2.0));
        ArticleStore->SetFieldNmFltV(ArticleId, "float_v", TFltV::GetV(1.0, 2.0, 3.0));
        ArticleStore->SetFieldNmFltV(ArticleId, "float_v", TFltV::GetV(1.0, 2.0, 3.0, 4.0));
        ArticleStore->SetFieldNmFltV(ArticleId, "float_v", TFltV::GetV(1.0, 2.0, 3.0, 4.0, 5.0));
        ArticleStore->SetFieldNmFltV(ArticleId, "float_v", TFltV::GetV(1.0, 2.0, 3.0, 4.0));
        ArticleStore->SetFieldNmFltV(ArticleId, "float_v", TFltV::GetV(1.0, 2.0, 3.0));
        ArticleStore->SetFieldNmFltV(ArticleId, "float_v", TFltV::GetV(1.0, 2.0));
        ArticleStore->SetFieldNmFltV(ArticleId, "float_v", TFltV::GetV(1.0));

        printf("Records: %d\n", (int)ArticleStore->GetRecs());

        SaveBase(Base);
    } catch (PExcept Except) {
        printf("Exception: %s\n", Except->GetMsgStr().CStr());
    }

    try {
        PBase Base = LoadBase("./test/cpp/data/", faUpdate, IndexCacheSize,
            StoreCacheSize, TStrUInt64H(), TStrUInt64H(), false, 128);
        PStore ArticleStore = Base->GetStoreByStoreNm("Article");

        const uint64 ArticleId = 0;

        ArticleStore->SetFieldNmStr(ArticleId, "string", MakeStr(1));
        ArticleStore->SetFieldNmStr(ArticleId, "string", MakeStr(11));
        ArticleStore->SetFieldNmStr(ArticleId, "string", MakeStr(111));
        ArticleStore->SetFieldNmNull(ArticleId, "string");
        ArticleStore->SetFieldNmStr(ArticleId, "string", MakeStr(1111));
        ArticleStore->SetFieldNmStr(ArticleId, "string", MakeStr(11111));
        ArticleStore->SetFieldNmNull(ArticleId, "string");
        ArticleStore->SetFieldNmStr(ArticleId, "string", MakeStr(1111));
        ArticleStore->SetFieldNmStr(ArticleId, "string", MakeStr(111));
        ArticleStore->SetFieldNmNull(ArticleId, "string");
        ArticleStore->SetFieldNmStr(ArticleId, "string", MakeStr(11));
        ArticleStore->SetFieldNmStr(ArticleId, "string", MakeStr(1));

        ArticleStore->SetFieldNmStrV(ArticleId, "string_v", TStrV::GetV("A"));
        ArticleStore->SetFieldNmStrV(ArticleId, "string_v", TStrV::GetV("A", "B"));
        ArticleStore->SetFieldNmNull(ArticleId, "string_v");
        ArticleStore->SetFieldNmStrV(ArticleId, "string_v", TStrV::GetV("A", "B", "C"));
        ArticleStore->SetFieldNmStrV(ArticleId, "string_v", TStrV::GetV("A", "B", "C", "D"));
        ArticleStore->SetFieldNmNull(ArticleId, "string_v");
        ArticleStore->SetFieldNmStrV(ArticleId, "string_v", TStrV::GetV("A", "B", "C", "D", "E"));
        ArticleStore->SetFieldNmStrV(ArticleId, "string_v", TStrV::GetV("A", "B", "C", "D"));
        ArticleStore->SetFieldNmNull(ArticleId, "string_v");
        ArticleStore->SetFieldNmStrV(ArticleId, "string_v", TStrV::GetV("A", "B", "C"));
        ArticleStore->SetFieldNmStrV(ArticleId, "string_v", TStrV::GetV("A", "B"));
        ArticleStore->SetFieldNmStrV(ArticleId, "string_v", TStrV::GetV("A"));

        ArticleStore->SetFieldNmIntV(ArticleId, "int_v", TIntV::GetV(1));
        ArticleStore->SetFieldNmIntV(ArticleId, "int_v", TIntV::GetV(1, 2));
        ArticleStore->SetFieldNmNull(ArticleId, "int_v");
        ArticleStore->SetFieldNmIntV(ArticleId, "int_v", TIntV::GetV(1, 2, 3));
        ArticleStore->SetFieldNmIntV(ArticleId, "int_v", TIntV::GetV(1, 2, 3, 4));
        ArticleStore->SetFieldNmIntV(ArticleId, "int_v", TIntV::GetV(1, 2, 3, 4, 5));
        ArticleStore->SetFieldNmNull(ArticleId, "int_v");
        ArticleStore->SetFieldNmIntV(ArticleId, "int_v", TIntV::GetV(1, 2, 3, 4));
        ArticleStore->SetFieldNmIntV(ArticleId, "int_v", TIntV::GetV(1, 2, 3));
        ArticleStore->SetFieldNmIntV(ArticleId, "int_v", TIntV::GetV(1, 2));
        ArticleStore->SetFieldNmNull(ArticleId, "int_v");
        ArticleStore->SetFieldNmIntV(ArticleId, "int_v", TIntV::GetV(1));

        ArticleStore->SetFieldNmFltV(ArticleId, "float_v", TFltV::GetV(1.0));
        ArticleStore->SetFieldNmFltV(ArticleId, "float_v", TFltV::GetV(1.0, 2.0));
        ArticleStore->SetFieldNmNull(ArticleId, "float_v");
        ArticleStore->SetFieldNmFltV(ArticleId, "float_v", TFltV::GetV(1.0, 2.0, 3.0));
        ArticleStore->SetFieldNmNull(ArticleId, "float_v");
        ArticleStore->SetFieldNmFltV(ArticleId, "float_v", TFltV::GetV(1.0, 2.0, 3.0, 4.0));
        ArticleStore->SetFieldNmFltV(ArticleId, "float_v", TFltV::GetV(1.0, 2.0, 3.0, 4.0, 5.0));
        ArticleStore->SetFieldNmFltV(ArticleId, "float_v", TFltV::GetV(1.0, 2.0, 3.0, 4.0));
        ArticleStore->SetFieldNmFltV(ArticleId, "float_v", TFltV::GetV(1.0, 2.0, 3.0));
        ArticleStore->SetFieldNmNull(ArticleId, "float_v");
        ArticleStore->SetFieldNmFltV(ArticleId, "float_v", TFltV::GetV(1.0, 2.0));
        ArticleStore->SetFieldNmFltV(ArticleId, "float_v", TFltV::GetV(1.0));

        SaveBase(Base);
    } catch (PExcept Except) {
        printf("Exception: %s\n", Except->GetMsgStr().CStr());
    }

    try {
        PBase Base = LoadBase("./test/cpp/data/", faUpdate, IndexCacheSize,
            StoreCacheSize, TStrUInt64H(), TStrUInt64H(), false, 128);
        PStore ArticleStore = Base->GetStoreByStoreNm("Article");

        TUInt64V DelRecIdV; ArticleStore->GetAllRecs()->GetRecIdV(DelRecIdV);
        ArticleStore->DeleteRecs(DelRecIdV);
        printf("Records: %d\n", (int)ArticleStore->GetRecs());

        SaveBase(Base);
    } catch (PExcept Except) {
        printf("Exception: %s\n", Except->GetMsgStr().CStr());
    }

}
