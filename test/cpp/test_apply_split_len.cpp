/**
 * Edge-case tests for TStorage::ApplyIndexKeySplitLen (qminer_storage.cpp):
 * applying per-index-key "splitLen" attributes from a store-definition JSON to
 * a base.
 *
 * GixSplitLenTests::SchemaSplitLenApplied covers the happy path (create +
 * re-apply + direct PutIndexKeySplitLen validation); here we pin down the
 * JSON-level behavior:
 *   - definitions without any splitLen are a strict no-op
 *   - single-object (non-array) definitions are accepted
 *   - unknown store / key / join names only fail when they carry a splitLen
 *   - zero / negative splitLen values are ignored (not applied, no throw)
 *   - splitLen on a non-index join is rejected
 *   - end-to-end: a tiny splitLen from the schema affects indexing/search, and
 *     since it is runtime-only, a reload without re-applying still works
 */

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

// plain schema (no splitLen anywhere): one indexed field and one index join
const TStr ApplySchemaStr =
    "[{ \"name\": \"AppItem\","
    "   \"fields\": [ { \"name\": \"Name\", \"type\": \"string\", \"primary\": true },"
    "                 { \"name\": \"Value\", \"type\": \"string\" } ],"
    "   \"joins\": [ { \"name\": \"hasRelated\", \"type\": \"index\", \"store\": \"AppItem\", \"storage\": \"tiny\" } ],"
    "   \"keys\": [ { \"field\": \"Value\", \"type\": \"value\", \"storage\": \"tiny\" } ]"
    "}]";

PBase NewApplyBase(const TStr& FPath)
{
    FreshDir(FPath);
    if (!TQm::TEnv::IsInit()) { TQm::TEnv::Init(); }
    PJsonVal SchemaVal = TJsonVal::GetValFromStr(ApplySchemaStr);
    return TStorage::NewBase(FPath, SchemaVal, 10000000, 10000000, true);
}

} // namespace

// a definition with no splitLen attributes must apply cleanly and change
// nothing; both the array form and a single store object must be accepted
TEST(QmApplySplitLenTests, DefWithoutSplitLenIsNoOpAndSingleObjectAccepted)
{
    const TStr FPath = "./apply_splitlen_noop/";
    PBase Base = NewApplyBase(FPath);
    PJsonVal SchemaVal = TJsonVal::GetValFromStr(ApplySchemaStr);
    // array form
    ASSERT_NO_THROW(TStorage::ApplyIndexKeySplitLen(Base, SchemaVal));
    // single store object (non-array) form
    PJsonVal StoreVal = SchemaVal->GetArrVal(0);
    ASSERT_NO_THROW(TStorage::ApplyIndexKeySplitLen(Base, StoreVal));
    // minimal definition: name only, no keys/joins at all
    PJsonVal NameOnlyVal = TJsonVal::GetValFromStr("[{ \"name\": \"AppItem\" }]");
    ASSERT_NO_THROW(TStorage::ApplyIndexKeySplitLen(Base, NameOnlyVal));
    TStorage::SaveBase(Base);
    // release the base BEFORE deleting its folder: ~TBase saves into FPath, and on
    // Linux the delete succeeds on open files, so a live base would then throw
    // inside a destructor (std::terminate). On Windows the delete just fails.
    Base.Clr();
    TDir::DelNonEmptyDir(FPath);
}

// unknown store/key/join names must be rejected loudly (PExcept) - but only
// when they actually carry a splitLen; entries without splitLen are skipped
// before any name resolution, so stale defs with extra stores keep working
TEST(QmApplySplitLenTests, UnknownNamesOnlyRejectedWhenSplitLenPresent)
{
    const TStr FPath = "./apply_splitlen_unknown/";
    PBase Base = NewApplyBase(FPath);

    // unknown store WITH splitLen -> throw
    PJsonVal BadStoreVal = TJsonVal::GetValFromStr(
        "[{ \"name\": \"NoSuchStore\", \"keys\": [ { \"field\": \"Value\", \"splitLen\": 100 } ] }]");
    EXPECT_THROW(TStorage::ApplyIndexKeySplitLen(Base, BadStoreVal), PExcept);

    // unknown store WITHOUT splitLen -> skipped, no throw
    PJsonVal BadStoreNoLenVal = TJsonVal::GetValFromStr(
        "[{ \"name\": \"NoSuchStore\", \"keys\": [ { \"field\": \"Value\" } ] }]");
    ASSERT_NO_THROW(TStorage::ApplyIndexKeySplitLen(Base, BadStoreNoLenVal));

    // known store, unknown key field WITH splitLen -> throw
    PJsonVal BadKeyVal = TJsonVal::GetValFromStr(
        "[{ \"name\": \"AppItem\", \"keys\": [ { \"field\": \"NoSuchField\", \"splitLen\": 100 } ] }]");
    EXPECT_THROW(TStorage::ApplyIndexKeySplitLen(Base, BadKeyVal), PExcept);

    // known store, unknown join WITH splitLen -> throw (key "Join" + name not found)
    PJsonVal BadJoinVal = TJsonVal::GetValFromStr(
        "[{ \"name\": \"AppItem\", \"joins\": [ { \"name\": \"noSuchJoin\", \"type\": \"index\", \"splitLen\": 100 } ] }]");
    EXPECT_THROW(TStorage::ApplyIndexKeySplitLen(Base, BadJoinVal), PExcept);

    // the base must still be usable after the failed applications
    PJsonVal GoodVal = TJsonVal::GetValFromStr(
        "[{ \"name\": \"AppItem\", \"keys\": [ { \"field\": \"Value\", \"splitLen\": 100 } ],"
        "   \"joins\": [ { \"name\": \"hasRelated\", \"type\": \"index\", \"splitLen\": 100 } ] }]");
    ASSERT_NO_THROW(TStorage::ApplyIndexKeySplitLen(Base, GoodVal));
    TStorage::SaveBase(Base);
    // release the base BEFORE deleting its folder: ~TBase saves into FPath, and on
    // Linux the delete succeeds on open files, so a live base would then throw
    // inside a destructor (std::terminate). On Windows the delete just fails.
    Base.Clr();
    TDir::DelNonEmptyDir(FPath);
}

// zero and negative splitLen values are not valid overrides: they must be
// ignored (only values > 0 are applied), not applied and not thrown at -
// even when the store/key names would be invalid
TEST(QmApplySplitLenTests, NonPositiveSplitLenValuesAreIgnored)
{
    const TStr FPath = "./apply_splitlen_nonpos/";
    PBase Base = NewApplyBase(FPath);
    PJsonVal ZeroVal = TJsonVal::GetValFromStr(
        "[{ \"name\": \"AppItem\", \"keys\": [ { \"field\": \"Value\", \"splitLen\": 0 } ] }]");
    ASSERT_NO_THROW(TStorage::ApplyIndexKeySplitLen(Base, ZeroVal));
    PJsonVal NegVal = TJsonVal::GetValFromStr(
        "[{ \"name\": \"AppItem\", \"keys\": [ { \"field\": \"Value\", \"splitLen\": -50 } ],"
        "   \"joins\": [ { \"name\": \"hasRelated\", \"type\": \"index\", \"splitLen\": -1 } ] }]");
    ASSERT_NO_THROW(TStorage::ApplyIndexKeySplitLen(Base, NegVal));
    // non-positive values must be skipped before name validation kicks in
    PJsonVal BadNamesVal = TJsonVal::GetValFromStr(
        "[{ \"name\": \"NoSuchStore\", \"keys\": [ { \"field\": \"NoSuchField\", \"splitLen\": 0 } ] }]");
    ASSERT_NO_THROW(TStorage::ApplyIndexKeySplitLen(Base, BadNamesVal));
    TStorage::SaveBase(Base);
    // release the base BEFORE deleting its folder: ~TBase saves into FPath, and on
    // Linux the delete succeeds on open files, so a live base would then throw
    // inside a destructor (std::terminate). On Windows the delete just fails.
    Base.Clr();
    TDir::DelNonEmptyDir(FPath);
}

// splitLen only makes sense for index joins (field joins have no inverted-index
// key); a field join carrying splitLen must be rejected
TEST(QmApplySplitLenTests, SplitLenOnNonIndexJoinRejected)
{
    const TStr FPath = "./apply_splitlen_fieldjoin/";
    PBase Base = NewApplyBase(FPath);
    PJsonVal FieldJoinVal = TJsonVal::GetValFromStr(
        "[{ \"name\": \"AppItem\", \"joins\": [ { \"name\": \"parent\", \"type\": \"field\", \"splitLen\": 100 } ] }]");
    EXPECT_THROW(TStorage::ApplyIndexKeySplitLen(Base, FieldJoinVal), PExcept);
    // the same join without splitLen must be skipped without complaint
    PJsonVal FieldJoinNoLenVal = TJsonVal::GetValFromStr(
        "[{ \"name\": \"AppItem\", \"joins\": [ { \"name\": \"parent\", \"type\": \"field\" } ] }]");
    ASSERT_NO_THROW(TStorage::ApplyIndexKeySplitLen(Base, FieldJoinNoLenVal));
    TStorage::SaveBase(Base);
    // release the base BEFORE deleting its folder: ~TBase saves into FPath, and on
    // Linux the delete succeeds on open files, so a live base would then throw
    // inside a destructor (std::terminate). On Windows the delete just fails.
    Base.Clr();
    TDir::DelNonEmptyDir(FPath);
}

// end-to-end: a very small splitLen from the schema forces child-vector
// chunking inside the live index while records are added, and searches must
// stay correct. because the override is runtime-only, reloading the base
// WITHOUT re-applying (falling back to the default split length) must also
// read the previously chunked data correctly.
TEST(QmApplySplitLenTests, TinySplitLenAffectsIndexingAndReloadWithoutApplyWorks)
{
    const TStr FPath = "./apply_splitlen_e2e/";
    FreshDir(FPath);
    if (!TQm::TEnv::IsInit()) { TQm::TEnv::Init(); }
    // key with a tiny splitLen: 300 records over 3 values = 100 items per gix
    // key, chunked into children of 20 while indexing
    const TStr SchemaStr =
        "[{ \"name\": \"E2eItem\","
        "   \"fields\": [ { \"name\": \"Name\", \"type\": \"string\", \"primary\": true },"
        "                 { \"name\": \"Value\", \"type\": \"string\" } ],"
        "   \"keys\": [ { \"field\": \"Value\", \"type\": \"value\", \"storage\": \"tiny\", \"splitLen\": 20 } ]"
        "}]";
    PJsonVal SchemaVal = TJsonVal::GetValFromStr(SchemaStr);
    const int Recs = 300;
    const int Values = 3;

    // create (NewBase applies the schema splitLen automatically) and fill
    {
        PBase Base = TStorage::NewBase(FPath, SchemaVal, 10000000, 10000000, true);
        for (int RecN = 0; RecN < Recs; RecN++) {
            PJsonVal RecVal = TJsonVal::NewObj();
            RecVal->AddToObj("Name", TStr::Fmt("rec%d", RecN));
            RecVal->AddToObj("Value", TStr::Fmt("v%d", RecN % Values));
            Base->AddRec("E2eItem", RecVal);
        }
        for (int ValueN = 0; ValueN < Values; ValueN++) {
            PRecSet RecSet = Base->Search(TStr::Fmt("{ \"$from\": \"E2eItem\", \"Value\": \"v%d\" }", ValueN));
            EXPECT_EQ(Recs / Values, RecSet->GetRecs()) << "wrong search count for v" << ValueN;
        }
        TStorage::SaveBase(Base);
    }

    // reload WITHOUT ApplyIndexKeySplitLen: the index falls back to its default
    // split length but must read the small persisted children correctly; adding
    // more records must also keep the index consistent
    {
        PBase Base = TStorage::LoadBase(FPath, faUpdate, 10000000, 10000000);
        for (int ValueN = 0; ValueN < Values; ValueN++) {
            PRecSet RecSet = Base->Search(TStr::Fmt("{ \"$from\": \"E2eItem\", \"Value\": \"v%d\" }", ValueN));
            EXPECT_EQ(Recs / Values, RecSet->GetRecs()) << "wrong count after reload without apply";
        }
        for (int RecN = Recs; RecN < Recs + 30; RecN++) {
            PJsonVal RecVal = TJsonVal::NewObj();
            RecVal->AddToObj("Name", TStr::Fmt("rec%d", RecN));
            RecVal->AddToObj("Value", TStr::Fmt("v%d", RecN % Values));
            Base->AddRec("E2eItem", RecVal);
        }
        TStorage::SaveBase(Base);
    }

    // reload WITH the splitLen re-applied (the intended production pattern)
    {
        PBase Base = TStorage::LoadBase(FPath, faRdOnly, 10000000, 10000000);
        TStorage::ApplyIndexKeySplitLen(Base, SchemaVal);
        for (int ValueN = 0; ValueN < Values; ValueN++) {
            PRecSet RecSet = Base->Search(TStr::Fmt("{ \"$from\": \"E2eItem\", \"Value\": \"v%d\" }", ValueN));
            EXPECT_EQ((Recs + 30) / Values, RecSet->GetRecs()) << "wrong count after reload with apply";
        }
    }
    TDir::DelNonEmptyDir(FPath);
}
