/* Copyright (C) Event Registry d.o.o. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 */

#include <qminer.h>
#include <qminer_storage.h>

#include "gtest/gtest.h"

using namespace TQm;
using namespace TQm::TStorage;

// Tests for the selectable gix key-dictionary representation (TGixKeyDict):
// hash (default, legacy-format files) vs sorted parallel arrays + overlay.

namespace {

typedef TIntUInt64Pr TKdGixKey;
typedef TUInt TKdGixItem;
typedef TPt<TGix<TKdGixKey, TKdGixItem> > PKdGix;

static const int64 KdCacheSize = 10000000;

static void KdClearDir(const TStr& DirNm) {
    if (TDir::Exists(DirNm)) { TDir::DelNonEmptyDir(DirNm); }
    TDir::GenDir(DirNm);
}

// first int of a .Gix file: >= 0 for the legacy hash format, -1 for the
// marker-led sorted format
static int KdFirstIntOfFile(const TStr& FNm) {
    TFIn FIn(FNm);
    const TInt FirstInt(FIn);
    return FirstInt;
}

} // namespace

// create a sorted-dictionary gix, mix in-order and out-of-order keys, delete,
// update, reload - data and representation must survive
TEST(GixKeyDictTests, SortedDictCrudAndPersist) {
    const TStr FPath = "./gix_keydict_crud/";
    KdClearDir(FPath);

    TGixDefItemHandler<TKdGixKey, TKdGixItem> ItemHandler;
    const TStr GixFNm = TPath::Combine(FPath, "KeyDict.Gix");
    const int Keys = 200;
    const int Items = 150; // > splitLen, so itemsets split and blob pts move

    {
        PKdGix Gix = TGix<TKdGixKey, TKdGixItem>::New("KeyDict", FPath, faCreate,
            &ItemHandler, KdCacheSize, 100, true, 50, 200, gkdtSorted);
        EXPECT_EQ(Gix->GetKeyDictType(), gkdtSorted);
        // ascending keys extend the sorted arrays directly...
        for (int KeyN = 0; KeyN < Keys; KeyN++) {
            for (int ItemN = 0; ItemN < Items; ItemN++) {
                Gix->AddItem(TKdGixKey(7, KeyN), TUInt(ItemN));
            }
        }
        // ...while keys arriving out of order go through the overlay
        for (int ItemN = 0; ItemN < Items; ItemN++) {
            Gix->AddItem(TKdGixKey(3, 5), TUInt(ItemN));
            Gix->AddItem(TKdGixKey(3, 1), TUInt(ItemN));
        }
        ASSERT_EQ(Gix->GetKeys(), Keys + 2);
        // delete one key from the sorted region
        Gix->DeleteItemSet(TKdGixKey(7, 10));
        ASSERT_EQ(Gix->GetKeys(), Keys + 1);
    }
    // the file must carry the sorted-format marker
    EXPECT_EQ(KdFirstIntOfFile(GixFNm), -1);

    // reload and verify everything
    {
        PKdGix Gix = TGix<TKdGixKey, TKdGixItem>::New("KeyDict", FPath, faRdOnly,
            &ItemHandler, KdCacheSize, 100, true, 50, 200);
        EXPECT_EQ(Gix->GetKeyDictType(), gkdtSorted);
        ASSERT_EQ(Gix->GetKeys(), Keys + 1);
        EXPECT_FALSE(Gix->IsKey(TKdGixKey(7, 10)));
        for (int KeyN = 0; KeyN < Keys; KeyN++) {
            if (KeyN == 10) { continue; }
            TVec<TKdGixItem> ItemV; Gix->GetItemV(TKdGixKey(7, KeyN), ItemV);
            ASSERT_EQ(ItemV.Len(), Items);
            for (int ItemN = 0; ItemN < Items; ItemN++) { ASSERT_EQ((int) ItemV[ItemN].Val, ItemN); }
        }
        TVec<TKdGixItem> OverlayItemV; Gix->GetItemV(TKdGixKey(3, 1), OverlayItemV);
        EXPECT_EQ(OverlayItemV.Len(), Items);
        // key-id iteration covers all live keys exactly once
        TIntSet SeenKeyIdSet; int Seen = 0;
        int KeyId = Gix->FFirstKeyId();
        while (Gix->FNextKeyId(KeyId)) {
            EXPECT_TRUE(Gix->IsKeyId(KeyId));
            EXPECT_FALSE(SeenKeyIdSet.IsKey(KeyId)); SeenKeyIdSet.AddKey(KeyId);
            EXPECT_FALSE(Gix->GetKeyBlobPt(KeyId).Empty());
            Seen++;
        }
        EXPECT_EQ(Seen, Keys + 1);
    }

    // a deleted sorted-region key can be re-added (tombstone revival)
    {
        PKdGix Gix = TGix<TKdGixKey, TKdGixItem>::New("KeyDict", FPath, faUpdate,
            &ItemHandler, KdCacheSize, 100, true, 50, 200);
        Gix->AddItem(TKdGixKey(7, 10), TUInt(42));
        ASSERT_EQ(Gix->GetKeys(), Keys + 2);
        TVec<TKdGixItem> ItemV; Gix->GetItemV(TKdGixKey(7, 10), ItemV);
        ASSERT_EQ(ItemV.Len(), 1);
        EXPECT_EQ((int) ItemV[0].Val, 42);
    }

    TDir::DelNonEmptyDir(FPath);
}

// convert an existing hash gix to the sorted representation (and back) by
// rewriting only the .Gix file - the posting blobs stay in place
TEST(GixKeyDictTests, ConvertKeyDictFileBothWays) {
    const TStr FPath = "./gix_keydict_convert/";
    KdClearDir(FPath);

    TGixDefItemHandler<TKdGixKey, TKdGixItem> ItemHandler;
    const TStr GixFNm = TPath::Combine(FPath, "KeyDict.Gix");
    const int Keys = 300;
    const int Items = 60;

    // build a default (hash) gix; keys added in random-ish order
    {
        PKdGix Gix = TGix<TKdGixKey, TKdGixItem>::New("KeyDict", FPath, faCreate,
            &ItemHandler, KdCacheSize, 100, true, 50, 200);
        EXPECT_EQ(Gix->GetKeyDictType(), gkdtHash);
        for (int KeyN = 0; KeyN < Keys; KeyN++) {
            const int ShuffledKeyN = (KeyN * 37) % Keys;
            for (int ItemN = 0; ItemN < Items; ItemN++) {
                Gix->AddItem(TKdGixKey(1, ShuffledKeyN), TUInt(ItemN));
            }
        }
    }
    // hash-type files keep the legacy format (readable by older binaries)
    EXPECT_GE(KdFirstIntOfFile(GixFNm), 0);

    // convert to sorted: write the converted dictionary aside, then swap it in
    {
        PKdGix Gix = TGix<TKdGixKey, TKdGixItem>::New("KeyDict", FPath, faRdOnly,
            &ItemHandler, KdCacheSize, 100, true, 50, 200);
        Gix->SaveKeyDictFileAsType(TPath::Combine(FPath, "KeyDict.Gix.new"), gkdtSorted);
    }
    TFile::Del(GixFNm, true);
    TFile::Move(TPath::Combine(FPath, "KeyDict.Gix.new"), GixFNm);
    EXPECT_EQ(KdFirstIntOfFile(GixFNm), -1);

    // the converted gix opens as sorted with identical data, and stays writable
    {
        PKdGix Gix = TGix<TKdGixKey, TKdGixItem>::New("KeyDict", FPath, faUpdate,
            &ItemHandler, KdCacheSize, 100, true, 50, 200);
        EXPECT_EQ(Gix->GetKeyDictType(), gkdtSorted);
        ASSERT_EQ(Gix->GetKeys(), Keys);
        for (int KeyN = 0; KeyN < Keys; KeyN++) {
            TVec<TKdGixItem> ItemV; Gix->GetItemV(TKdGixKey(1, KeyN), ItemV);
            ASSERT_EQ(ItemV.Len(), Items);
        }
        // a late write into the converted gix (lands in the overlay)
        Gix->AddItem(TKdGixKey(0, 99999), TUInt(7));
        // and a write into an existing key (in-place blob pointer update)
        Gix->AddItem(TKdGixKey(1, 5), TUInt(Items));
    }
    {
        PKdGix Gix = TGix<TKdGixKey, TKdGixItem>::New("KeyDict", FPath, faRdOnly,
            &ItemHandler, KdCacheSize, 100, true, 50, 200);
        ASSERT_EQ(Gix->GetKeys(), Keys + 1);
        TVec<TKdGixItem> LateItemV; Gix->GetItemV(TKdGixKey(0, 99999), LateItemV);
        ASSERT_EQ(LateItemV.Len(), 1);
        TVec<TKdGixItem> GrownItemV; Gix->GetItemV(TKdGixKey(1, 5), GrownItemV);
        ASSERT_EQ(GrownItemV.Len(), Items + 1);

        // convert back to hash the same way
        Gix->SaveKeyDictFileAsType(TPath::Combine(FPath, "KeyDict.Gix.new"), gkdtHash);
    }
    TFile::Del(GixFNm, true);
    TFile::Move(TPath::Combine(FPath, "KeyDict.Gix.new"), GixFNm);
    EXPECT_GE(KdFirstIntOfFile(GixFNm), 0);
    {
        PKdGix Gix = TGix<TKdGixKey, TKdGixItem>::New("KeyDict", FPath, faRdOnly,
            &ItemHandler, KdCacheSize, 100, true, 50, 200);
        EXPECT_EQ(Gix->GetKeyDictType(), gkdtHash);
        ASSERT_EQ(Gix->GetKeys(), Keys + 1);
        TVec<TKdGixItem> ItemV; Gix->GetItemV(TKdGixKey(1, 5), ItemV);
        ASSERT_EQ(ItemV.Len(), Items + 1);
    }

    TDir::DelNonEmptyDir(FPath);
}

// CopyTo into a sorted-dictionary destination builds the compact arrays
// directly (keys arrive in ascending order), with identical data - the path
// the defrag/reindex rebuilds use to preserve the representation
TEST(GixKeyDictTests, CopyToSortedDestination) {
    const TStr SrcFPath = "./gix_keydict_copy_src/";
    const TStr DestFPath = "./gix_keydict_copy_dest/";
    KdClearDir(SrcFPath);
    KdClearDir(DestFPath);

    TGixDefItemHandler<TKdGixKey, TKdGixItem> ItemHandler;
    const int Keys = 100;
    const int Items = 120;
    {
        PKdGix SrcGix = TGix<TKdGixKey, TKdGixItem>::New("KeyDict", SrcFPath, faCreate,
            &ItemHandler, KdCacheSize, 100, true, 50, 200);
        for (int KeyN = 0; KeyN < Keys; KeyN++) {
            for (int ItemN = 0; ItemN < Items; ItemN++) {
                SrcGix->AddItem(TKdGixKey(2, (KeyN * 61) % Keys), TUInt(ItemN));
            }
        }
    }
    {
        PKdGix SrcGix = TGix<TKdGixKey, TKdGixItem>::New("KeyDict", SrcFPath, faRdOnly,
            &ItemHandler, KdCacheSize, 100, true, 50, 200);
        PKdGix DestGix = TGix<TKdGixKey, TKdGixItem>::New("KeyDict", DestFPath, faCreate,
            &ItemHandler, KdCacheSize, 100, true, 50, 200, gkdtSorted);
        SrcGix->CopyTo(*DestGix);
        EXPECT_TRUE(SrcGix->VerifySample(*DestGix, Keys));
    }
    {
        PKdGix DestGix = TGix<TKdGixKey, TKdGixItem>::New("KeyDict", DestFPath, faRdOnly,
            &ItemHandler, KdCacheSize, 100, true, 50, 200);
        EXPECT_EQ(DestGix->GetKeyDictType(), gkdtSorted);
        ASSERT_EQ(DestGix->GetKeys(), Keys);
        TVec<TKdGixItem> ItemV; DestGix->GetItemV(TKdGixKey(2, 42), ItemV);
        ASSERT_EQ(ItemV.Len(), Items);
    }

    TDir::DelNonEmptyDir(SrcFPath);
    TDir::DelNonEmptyDir(DestFPath);
}

// full-base conversion, as the ConvertIndexKeyDict console action performs it:
// write the converted .Gix files aside, swap them in with the base closed,
// reload and search - value keys, text keys and joins must be intact
TEST(GixKeyDictTests, BaseIndexConvertAndSearch) { try {
    const TStr FPath = "./test/cpp/data/keydict_base/";
    const TStr BuildFPath = "./test/cpp/data/keydict_build/";
    if (TDir::Exists(FPath)) { TDir::DelNonEmptyDir(FPath); }
    if (TDir::Exists(BuildFPath)) { TDir::DelNonEmptyDir(BuildFPath); }
    TDir::GenDir(FPath);
    TDir::GenDir(BuildFPath);

    if (!TUnicodeDef::IsDef()) { TUnicodeDef::Load("./src/glib/bin/UnicodeDef.Bin"); }
    if (!TQm::TEnv::IsInit()) { TQm::TEnv::Init(); }

    const char* SchemaStr =
        "[{ \"name\": \"Doc\","
        "   \"fields\": [ { \"name\": \"Name\", \"type\": \"string\", \"primary\": true },"
        "                 { \"name\": \"Body\", \"type\": \"string\" },"
        "                 { \"name\": \"Lang\", \"type\": \"string\" } ],"
        "   \"keys\": [ { \"field\": \"Body\", \"type\": \"text_position\" },"
        "               { \"field\": \"Lang\", \"type\": \"value\", \"storage\": \"tiny\" } ]"
        "}]";
    const int Docs = 300;
    {
        PBase Base = NewBase(FPath, TJsonVal::GetValFromStr(SchemaStr), 10000000, 10000000, true);
        for (int DocN = 0; DocN < Docs; DocN++) {
            PJsonVal DocVal = TJsonVal::NewObj();
            DocVal->AddToObj("Name", TStr::Fmt("doc%d", DocN));
            DocVal->AddToObj("Body", TStr::Fmt("body uniqueword%d shared words", DocN));
            DocVal->AddToObj("Lang", (DocN % 2 == 0) ? "eng" : "deu");
            Base->AddRec("Doc", DocVal);
        }
        SaveBase(Base);
    }
    // convert all gixes to the sorted representation, writing the files aside
    TStrV KeyDictFNmV;
    {
        PBase Base = LoadBase(FPath, faRdOnly, 10000000, 10000000);
        const TWPt<TIndex> Index = Base->GetIndex();
        TStrV GixNmV; GixNmV.Add("full"); GixNmV.Add("small"); GixNmV.Add("tiny"); GixNmV.Add("pos");
        TStrV PrefixV; PrefixV.Add("Index.GixFull"); PrefixV.Add("Index.GixSmall"); PrefixV.Add("Index.GixTiny"); PrefixV.Add("Index.GixPos");
        for (int GixN = 0; GixN < GixNmV.Len(); GixN++) {
            EXPECT_EQ(Index->GetGixKeyDictType(GixNmV[GixN]), gkdtHash);
            const TStr KeyDictFNm = PrefixV[GixN] + ".Gix";
            Index->SaveGixKeyDictAsType(GixNmV[GixN], TPath::Combine(BuildFPath, KeyDictFNm), gkdtSorted);
            KeyDictFNmV.Add(KeyDictFNm);
        }
    }
    // swap the .Gix files in (base closed); blob files untouched
    for (int FNmN = 0; FNmN < KeyDictFNmV.Len(); FNmN++) {
        TFile::Del(TPath::Combine(FPath, KeyDictFNmV[FNmN]), true);
        TFile::Move(TPath::Combine(BuildFPath, KeyDictFNmV[FNmN]), TPath::Combine(FPath, KeyDictFNmV[FNmN]));
    }
    // reload and verify searches on the converted index
    {
        PBase Base = LoadBase(FPath, faRdOnly, 10000000, 10000000);
        const TWPt<TIndex> Index = Base->GetIndex();
        EXPECT_EQ(Index->GetGixKeyDictType("tiny"), gkdtSorted);
        EXPECT_EQ(Index->GetGixKeyDictType("pos"), gkdtSorted);
        TWPt<TStore> Store = Base->GetStoreByStoreNm("Doc");
        const PIndexVoc IndexVoc = Base->GetIndexVoc();
        // value key (tiny gix)
        const int LangKeyId = IndexVoc->GetKeyId(Store->GetStoreId(), "Lang");
        PRecSet EngRecSet = Index->SearchGix(Base, LangKeyId, IndexVoc->GetWordId(LangKeyId, "eng"));
        EXPECT_EQ(EngRecSet->GetRecs(), Docs / 2);
        // text_position key (pos gix)
        const int BodyKeyId = IndexVoc->GetKeyId(Store->GetStoreId(), "Body");
        TUInt64V WordIdV; WordIdV.Add(IndexVoc->GetWordId(BodyKeyId, "shared"));
        PRecSet SharedRecSet = Index->SearchTextPos(Base, BodyKeyId, WordIdV, TIntV());
        EXPECT_EQ(SharedRecSet->GetRecs(), Docs);
        for (int DocN = 0; DocN < Docs; DocN += 50) {
            TUInt64V UniqueWordIdV; UniqueWordIdV.Add(IndexVoc->GetWordId(BodyKeyId, TStr::Fmt("uniqueword%d", DocN)));
            EXPECT_EQ(Index->SearchTextPos(Base, BodyKeyId, UniqueWordIdV, TIntV())->GetRecs(), 1);
        }
    }

    TDir::DelNonEmptyDir(FPath);
    TDir::DelNonEmptyDir(BuildFPath);
} catch (PExcept E) { FAIL() << E->GetMsgStr().CStr(); } }
