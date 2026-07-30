/**
 * Copyright (c) 2015, Jozef Stefan Institute, Quintelligence d.o.o. and contributors
 * All rights reserved.
 *
 * This source code is licensed under the FreeBSD license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "qminer_storage.h"

namespace TQm {

namespace TStorage {

///////////////////////////////
/// Schema description of index key
TStr TIndexKeyEx::GetKeyType() const {
    return
        IsValue()    ? "value" :
        IsText()     ? "text" :
        IsTextPos()  ? "text_position" :
        IsLocation() ? "location" :
                       "linear";
}

///////////////////////////////
/// Store schema definition.
TStoreSchema::TMaps TStoreSchema::Maps;

TStoreSchema::TMaps::TMaps() {
    // field-type map
    FieldTypeMap.AddDat("byte") = oftByte;
    FieldTypeMap.AddDat("int") = oftInt;
    FieldTypeMap.AddDat("int16") = oftInt16;
    FieldTypeMap.AddDat("int64") = oftInt64;
    FieldTypeMap.AddDat("int_v") = oftIntV;
    FieldTypeMap.AddDat("uint") = oftUInt;
    FieldTypeMap.AddDat("uint16") = oftUInt16;
    FieldTypeMap.AddDat("uint64") = oftUInt64;
    FieldTypeMap.AddDat("string") = oftStr;
    FieldTypeMap.AddDat("string_v") = oftStrV;
    FieldTypeMap.AddDat("bool") = oftBool;
    FieldTypeMap.AddDat("float") = oftFlt;
    FieldTypeMap.AddDat("sfloat") = oftSFlt;
    FieldTypeMap.AddDat("float_pair") = oftFltPr;
    FieldTypeMap.AddDat("float_v") = oftFltV;
    FieldTypeMap.AddDat("datetime") = oftTm;
    FieldTypeMap.AddDat("num_sp_v") = oftNumSpV;
    FieldTypeMap.AddDat("bow_sp_v") = oftBowSpV;
    FieldTypeMap.AddDat("blob") = oftTMem;
    FieldTypeMap.AddDat("json") = oftJson;

    // time-window units
    TimeWindowUnitMap.AddDat("second",                             1000);
    TimeWindowUnitMap.AddDat("minute",                        60 * 1000);
    TimeWindowUnitMap.AddDat("hour",                     60 * 60 * 1000);
    TimeWindowUnitMap.AddDat("day",                 24 * 60 * 60 * 1000);
    TimeWindowUnitMap.AddDat("week",            7 * 24 * 60 * 60 * 1000);
    TimeWindowUnitMap.AddDat("month",  uint64(30) * 24 * 60 * 60 * 1000);
    TimeWindowUnitMap.AddDat("year",  uint64(365) * 24 * 60 * 60 * 1000);
}

TFieldDesc TStoreSchema::ParseFieldDesc(const TWPt<TBase>& Base, const PJsonVal& FieldVal) {
    // assert necessary stuff there
    QmAssertR(FieldVal->IsObjKey("name"), "Missing field name");
    QmAssertR(FieldVal->IsObjKey("type"), "Missing field type");
    // parse out
    TStr FieldName = FieldVal->GetObjKey("name")->GetStr();
    TStr FieldTypeStr = FieldVal->GetObjKey("type")->GetStr();
    const bool NullableP = FieldVal->GetObjBool("null", false);
    const bool PrimaryP = FieldVal->GetObjBool("primary", false);
    const bool CodebookP = FieldVal->GetObjBool("codebook", false);
    // validate
    Base->AssertValidNm(FieldName);
    QmAssertR(Maps.FieldTypeMap.IsKey(FieldTypeStr), "Unsupported field type " + FieldTypeStr);
    // map field type to enum
    TFieldType FieldType = (TFieldType)Maps.FieldTypeMap.GetDat(FieldTypeStr).Val;
    // done
    return TFieldDesc(Base, FieldName, FieldType,  PrimaryP, NullableP, false, CodebookP);
}

TFieldDescEx TStoreSchema::ParseFieldDescEx(const PJsonVal& FieldVal) {
    TFieldDescEx FieldDescEx;
    // parse flags
    FieldDescEx.SmallStringP = FieldVal->GetObjBool("shortstring", false);
    FieldDescEx.CodebookP = FieldVal->GetObjBool("codebook", false);
    // load default value (if available)
    if (FieldVal->IsObjKey("default")) {
        FieldDescEx.DefaultVal = FieldVal->GetObjKey("default");
    }
    // get storage place (cache or memory)

    if (FieldVal->IsObjKey("store")) {
        TStr StoreLocStr = FieldVal->GetObjStr("store");
        if (StoreLocStr == "memory") {
            FieldDescEx.FieldStoreLoc = slMemory;
        } else if (StoreLocStr == "cache") {
            FieldDescEx.FieldStoreLoc = slDisk;
        } else {
            throw TQmExcept::New(TStr::Fmt("Unsupported 'store' flag for field: %s", StoreLocStr.CStr()));
        }
    } else {
        FieldDescEx.FieldStoreLoc = DefaultFieldStoreLoc;
    }
    // done
    return FieldDescEx;
}

TJoinDescEx TStoreSchema::ParseJoinDescEx(const PJsonVal& JoinVal) {
    // assert necessary stuff there
    QmAssertR(JoinVal->IsObjKey("name"), "Missing join name");
    QmAssertR(JoinVal->IsObjKey("type"), "Missing join type");
    QmAssertR(JoinVal->IsObjKey("store"), "Missing join store");
    // parse parameters
    TStr JoinName = JoinVal->GetObjStr("name");
    TStr JoinType = JoinVal->GetObjStr("type", "index");
    TStr JoinStore = JoinVal->GetObjStr("store");
    // get extra description
    TJoinDescEx JoinDescEx;
    JoinDescEx.JoinName = JoinName;
    JoinDescEx.JoinStoreName = JoinStore;
    // get join type
    if (JoinType == "index") {
        JoinDescEx.JoinType = osjtIndex;
    } else if (JoinType == "field") {
        JoinDescEx.JoinType = osjtField;
    } else {
        throw TQmExcept::New("Unsupported join type '" + JoinType + "'");
    }
    // get inverse join
    if (JoinVal->IsObjKey("inverse")) {
        JoinDescEx.InverseJoinName = JoinVal->GetObjStr("inverse");
    }
    // parse storage parameters for join
    TStr StorageStr = JoinVal->GetObjStr("storage", "full");
    if (StorageStr == "full") {
        JoinDescEx.GixType = oikgtFull;
        JoinDescEx.RecIdFieldType = oftUInt64;
        JoinDescEx.FreqFieldType = oftInt;
    } else if (StorageStr == "small") {
        JoinDescEx.GixType = oikgtSmall;
        JoinDescEx.RecIdFieldType = oftUInt;
        JoinDescEx.FreqFieldType = oftInt16;
    } else if (StorageStr == "tiny") {
        JoinDescEx.GixType = oikgtTiny;
        JoinDescEx.RecIdFieldType = oftUInt;
        JoinDescEx.FreqFieldType = oftUndef;
    } else {
        // only field joins support more complex type combinations
        QmAssertR(JoinDescEx.JoinType == osjtField, "Invalid storage definition '" + StorageStr + "' for index join");
        JoinDescEx.GixType = oikgtUndef;
        // parse types
        TStrV PartV; StorageStr.SplitOnAllCh('-', PartV);
        if (PartV.Len() == 2) {
            // make sure we know of the types
            QmAssertR(Maps.FieldTypeMap.IsKey(PartV[0]), "Unkown field join record type '" + PartV[0] + "'");
            QmAssertR(Maps.FieldTypeMap.IsKey(PartV[1]), "Unkown field join frequency type '" + PartV[1] + "'");
            // remember the types
            JoinDescEx.RecIdFieldType = (TFieldType)Maps.FieldTypeMap.GetDat(PartV[0]).Val;
            JoinDescEx.FreqFieldType = (TFieldType)Maps.FieldTypeMap.GetDat(PartV[1]).Val;
        } else if (PartV.Len() == 1) {
            // check we know of the type
            QmAssertR(Maps.FieldTypeMap.IsKey(PartV[0]), "Unkown field join record type '" + PartV[0] + "'");
            // remember record type
            JoinDescEx.RecIdFieldType = (TFieldType)Maps.FieldTypeMap.GetDat(PartV[0]).Val;
            // no field for frequency, value is 1 by default
            JoinDescEx.FreqFieldType = TFieldType::oftUndef;
        } else {
            // we got invalid parts desc
            throw TQmExcept::New("Unkown join storage definiton  '" + StorageStr + "' for index join");
        }
    }
    // field-join only - check flags where fields are stored
    if (JoinDescEx.JoinType == osjtField) {
        if (JoinVal->IsObjKey("storage_location")) {
            TStr StoreLocStr = JoinVal->GetObjStr("storage_location");
            if (StoreLocStr == "memory") {
                JoinDescEx.FieldStoreLoc = slMemory;
            } else if (StoreLocStr == "cache") {
                JoinDescEx.FieldStoreLoc = slDisk;
            } else {
                throw TQmExcept::New(TStr::Fmt("Unsupported 'storage_location' flag for join: %s", StoreLocStr.CStr()));
            }
        } else {
            JoinDescEx.FieldStoreLoc = DefaultFieldStoreLoc;
        }
    }
    // done
    return JoinDescEx;
}

TIndexKeyEx TStoreSchema::ParseIndexKeyEx(const PJsonVal& IndexKeyVal) {
    // check for mandatory fields
    QmAssertR(IndexKeyVal->IsObjKey("field"), "Missing key-index field");
    QmAssertR(IndexKeyVal->IsObjKey("type"), "Missing key-index type");
    // parse out indexed field
    TIndexKeyEx IndexKeyEx;
    IndexKeyEx.FieldName = IndexKeyVal->GetObjStr("field");
    // check if it is a valid field name
    QmAssertR(FieldH.IsKey(IndexKeyEx.FieldName),
        "Unknown target field for key-index: '" + IndexKeyEx.FieldName + "'");
    // get field type to avoid further lookups when indexing
    TFieldType FieldType = FieldH.GetDat(IndexKeyEx.FieldName).GetFieldType();
    // parse out key name, use field name as default
    IndexKeyEx.KeyIndexName = IndexKeyVal->GetObjStr("name", IndexKeyEx.FieldName);
    // get and parse key type
    TStr KeyTypeStr = IndexKeyVal->GetObjStr("type");
    if (KeyTypeStr == "value") {
        IndexKeyEx.KeyType = oiktValue;
    } else if (KeyTypeStr == "text") {
        IndexKeyEx.KeyType = oiktText;
    } else if (KeyTypeStr == "text_position") {
        IndexKeyEx.KeyType = oiktTextPos;
    } else if (KeyTypeStr == "location") {
        IndexKeyEx.KeyType = oiktLocation;
    } else if (KeyTypeStr == "linear") {
        IndexKeyEx.KeyType = oiktLinear;
    } else {
        throw TQmExcept::New("Unknown key type '" + KeyTypeStr  + "' for field '" + IndexKeyEx.FieldName + "'");
    }
    // parse out gix type (default is full)
    TStr StorageStr = IndexKeyVal->GetObjStr("storage", "full");
    if (StorageStr == "full") {
        IndexKeyEx.GixType = oikgtFull;
    } else if (StorageStr == "small") {
        IndexKeyEx.GixType = oikgtSmall;
    } else if (StorageStr == "tiny") {
        IndexKeyEx.GixType = oikgtTiny;
    } else {
        throw TQmExcept::New("Unkown gix storage type '" + StorageStr + "' for field '" + IndexKeyEx.FieldName + "'");
    }
    // check field type and index type match
    if (FieldType == oftStr && IndexKeyEx.IsValue()) {
    } else if (FieldType == oftStr && IndexKeyEx.IsText()) {
    } else if (FieldType == oftStr && IndexKeyEx.IsTextPos()) {
    } else if (FieldType == oftStrV && IndexKeyEx.IsValue()) {
    } else if (FieldType == oftTm && IndexKeyEx.IsValue()) {
    } else if (FieldType == oftFltPr && IndexKeyEx.IsLocation()) {
    } else if (FieldType == oftByte && IndexKeyEx.IsLinear()) {
    } else if (FieldType == oftInt && IndexKeyEx.IsLinear()) {
    } else if (FieldType == oftInt16 && IndexKeyEx.IsLinear()) {
    } else if (FieldType == oftInt64 && IndexKeyEx.IsLinear()) {
    } else if (FieldType == oftUInt && IndexKeyEx.IsLinear()) {
    } else if (FieldType == oftUInt16 && IndexKeyEx.IsLinear()) {
    } else if (FieldType == oftUInt64 && IndexKeyEx.IsLinear()) {
    } else if (FieldType == oftTm && IndexKeyEx.IsLinear()) {
    } else if (FieldType == oftFlt && IndexKeyEx.IsLinear()) {
    } else if (FieldType == oftSFlt && IndexKeyEx.IsLinear()) {
    } else {
        // not supported, lets complain about it...
        throw TQmExcept::New("Indexing '" + KeyTypeStr + "' not supported for field '" + IndexKeyEx.FieldName + "'");
    }
    // get and parse sort type
    if (IndexKeyVal->IsObjKey("sort")) {
        // check if we can even handle sort for this key
        if (!IndexKeyEx.IsValue()) {
            throw TQmExcept::New("Sort only possible for keys of type 'value' and not '" + KeyTypeStr + "'");
        }
        // we can, parse out how we sort the values
        TStr SortTypeStr = IndexKeyVal->GetObjStr("sort");
        if (SortTypeStr == "string") {
            IndexKeyEx.SortType = oikstByStr;
        } else if (SortTypeStr == "id") {
            IndexKeyEx.SortType = oikstById;
        } else if (SortTypeStr == "number") {
            IndexKeyEx.SortType = oikstByFlt;
        } else {
            throw TQmExcept::New("Unsupported sort type " + SortTypeStr);
        }
    } else if (IndexKeyEx.IsLinear()) {
        // sort type depends on the field type, used by btree
        if (FieldType == oftInt) {
            IndexKeyEx.SortType = oikstAsInt;
        } else if (FieldType == oftByte) {
            IndexKeyEx.SortType = oikstAsByte;
        } else if (FieldType == oftInt16) {
            IndexKeyEx.SortType = oikstAsInt16;
        } else if (FieldType == oftInt64) {
            IndexKeyEx.SortType = oikstAsInt64;
        } else if (FieldType == oftUInt) {
            IndexKeyEx.SortType = oikstAsUInt;
        } else if (FieldType == oftUInt16) {
            IndexKeyEx.SortType = oikstAsUInt16;
        } else if (FieldType == oftUInt64) {
            IndexKeyEx.SortType = oikstAsUInt64;
        } else if (FieldType == oftTm) {
            IndexKeyEx.SortType = oikstAsTm;
        } else if (FieldType == oftFlt) {
            IndexKeyEx.SortType = oikstAsFlt;
        } else if (FieldType == oftSFlt) {
            IndexKeyEx.SortType = oikstAsSFlt;
        }
    } else {
        IndexKeyEx.SortType = oikstUndef;
    }
    // parse out word vocabulary
    IndexKeyEx.WordVocName = IndexKeyVal->GetObjStr("vocabulary", "");
    // parse out tokenizer
    if (IndexKeyEx.IsText() || IndexKeyEx.IsTextPos()) {
        if (IndexKeyVal->IsObjKey("tokenizer")) {
            PJsonVal TokenizerVal = IndexKeyVal->GetObjKey("tokenizer");
            QmAssertR(TokenizerVal->IsObjKey("type"),
                "Missing tokenizer type " + TokenizerVal->SaveStr());
            const TStr& TypeNm = TokenizerVal->GetObjStr("type");
            IndexKeyEx.Tokenizer = TTokenizer::New(TypeNm, TokenizerVal);
        } else {
            IndexKeyEx.Tokenizer = TTokenizers::THtmlUnicode::New();
        }
    }
    return IndexKeyEx;
}

TStoreSchema::TStoreSchema(const TWPt<TBase>& Base, const PJsonVal& StoreVal) : StoreId(0), HasStoreIdP(false), DefaultFieldStoreLoc(slMemory), DenseRecIdMapP(false) {
    QmAssertR(StoreVal->IsObj(), "Invalid JSON for store definition.");
    // get store name
    QmAssertR(StoreVal->IsObjKey("name"), "Missing store name.");
    StoreName = StoreVal->GetObjStr("name");
    BlockSizeMem = 1000;
    // get additional options
    if (StoreVal->IsObjKey("options")) {
        PJsonVal options = StoreVal->GetObjKey("options");
        if (options->IsObjKey("type")) {
            StoreType = options->GetObjStr("type");
        }
        if (options->IsObjKey("recIdMap")) {
            const TStr RecIdMapStr = options->GetObjStr("recIdMap");
            if (RecIdMapStr == "dense") {
                DenseRecIdMapP = true;
            } else if (RecIdMapStr == "hash") {
                DenseRecIdMapP = false;
            } else {
                throw TQmExcept::New(TStr::Fmt("Unsupported 'recIdMap' flag for store %s: %s (expected 'dense' or 'hash')", StoreName.CStr(), RecIdMapStr.CStr()));
            }
        }
        if (options->IsObjKey("storage_location")) {
            TStr StoreLocStr = options->GetObjStr("storage_location");
            if (StoreLocStr == "memory") {
                DefaultFieldStoreLoc = slMemory;
            } else if (StoreLocStr == "cache") {
                DefaultFieldStoreLoc = slDisk;
            } else {
                throw TQmExcept::New(TStr::Fmt("Unsupported 'storage_location' flag for store %s: %s", StoreName.CStr(), StoreLocStr.CStr()));
            }
        }
        // parse block size
        BlockSizeMem = MAX(1, options->GetObjInt("block_size_mem", BlockSizeMem));
    }
    // get id (optional)
    if (StoreVal->IsObjKey("id")) {
        const int _StoreId = StoreVal->GetObjInt("id");
        QmAssertR(_StoreId >= 0 && _StoreId < (int)TEnv::GetMxStores(), "Store " + StoreName + " ID out of range");
        StoreId = (uint)_StoreId;
        HasStoreIdP = true;
    }
    // fields
    QmAssertR(StoreVal->IsObjKey("fields"), "Missing field list.");
    PJsonVal FieldDefs = StoreVal->GetObjKey("fields");
    QmAssertR(FieldDefs->IsArr(), "Bad field list.");

    // parser fields
    for (int FieldN = 0; FieldN < FieldDefs->GetArrVals(); FieldN++) {
        PJsonVal FieldDef = FieldDefs->GetArrVal(FieldN);
        // prase basic field description
        TFieldDesc FieldDesc = ParseFieldDesc(Base, FieldDef);
        QmAssertR(!FieldH.IsKey(FieldDesc.GetFieldNm()), "Duplicate field name " + FieldDesc.GetFieldNm() + " in store " + StoreName);
        FieldH.AddDat(FieldDesc.GetFieldNm(), FieldDesc);
        // prase extended field description required for serialization
        TFieldDescEx FieldDescEx = ParseFieldDescEx(FieldDef);
        FieldExH.AddDat(FieldDesc.GetFieldNm(), FieldDescEx);
    }

    // parse keys
    if (StoreVal->IsObjKey("keys")) {
        PJsonVal KeyDefs = StoreVal->GetObjKey("keys");
        QmAssertR(KeyDefs->IsArr(), "Bad key list.");
        for (int KeyN = 0; KeyN < KeyDefs->GetArrVals(); KeyN++) {
            PJsonVal KeyDef = KeyDefs->GetArrVal(KeyN);
            TIndexKeyEx IndexKeyDesc = ParseIndexKeyEx(KeyDef);
            IndexKeyExV.Add(IndexKeyDesc);
        }
    }

    // joins
    if (StoreVal->IsObjKey("joins")) {
        PJsonVal JoinDefs = StoreVal->GetObjKey("joins");
        QmAssertR(JoinDefs->IsArr(), "Bad join list.");
        for (int JoinN = 0; JoinN < JoinDefs->GetArrVals(); JoinN++) {
            PJsonVal JoinDef = JoinDefs->GetArrVal(JoinN);
            // parse join
            TJoinDescEx JoinDescEx = ParseJoinDescEx(JoinDef);
            // add new field in case of field join
            if (JoinDescEx.JoinType == osjtField) {
                // we create two fields for each field join: record Id and frequency
                TStr JoinRecFieldNm = JoinDescEx.JoinName + "Id";
                TStr JoinFqFieldNm = JoinDescEx.JoinName + "Fq";
                // prepare join field descriptions
                FieldH.AddDat(JoinRecFieldNm, TFieldDesc(Base, JoinRecFieldNm, JoinDescEx.RecIdFieldType, false, true, true, false));
                FieldExH.AddDat(JoinRecFieldNm, TFieldDescEx(JoinDescEx.FieldStoreLoc, false, false));
                // prepare extended field description
                if (JoinDescEx.FreqFieldType != oftUndef) {
                    FieldExH.AddDat(JoinFqFieldNm, TFieldDescEx(JoinDescEx.FieldStoreLoc, false, false));
                    FieldH.AddDat(JoinFqFieldNm, TFieldDesc(Base, JoinFqFieldNm, JoinDescEx.FreqFieldType, false, true, true, false));
                }
            }
            // remember join
            JoinDescExV.Add(JoinDescEx);
        }
    }

    // parse window size
    if (StoreVal->IsObjKey("window")) {
        // window size defined in number of records
        WndDesc.WindowType = swtLength;
        // parse size
        PJsonVal WindowSize = StoreVal->GetObjKey("window");
        QmAssertR(WindowSize->IsNum(), "Bad window size parameter.");
        WndDesc.WindowSize = WindowSize->GetUInt64();
    } else if (StoreVal->IsObjKey("timeWindow")) {
        // time-defined window, parse out details
        WndDesc.WindowType = swtTime;
        // parse parameters
        PJsonVal TimeWindow = StoreVal->GetObjKey("timeWindow");
        QmAssertR(TimeWindow->IsObj(), "Bad timeWindow parameter.");
        // get window duration
        QmAssertR(TimeWindow->IsObjKey("duration"), "Missing duration parameter.");
        uint64 WindowSize = TimeWindow->GetObjUInt64("duration");
        // get duration unit
        TStr UnitStr = TimeWindow->GetObjStr("unit", "second");
        // check we know of the unit
        QmAssertR(Maps.TimeWindowUnitMap.IsKey(UnitStr),
            "Unsupported timeWindow length unit type: " + UnitStr);
        // set time duration in milliseconds
        const uint64 FactorMSecs = Maps.TimeWindowUnitMap.GetDat(UnitStr);
        WndDesc.WindowSize = WindowSize * FactorMSecs;
        // get field giving the tact for time
        if (TimeWindow->IsObjKey("field")) {
            WndDesc.TimeFieldNm = TimeWindow->GetObjStr("field");
            WndDesc.InsertP = false;
        } else {
            // no time field, create one which takes insert-time value
            TFieldDesc FieldDesc(Base, TStoreWndDesc::SysInsertedAtFieldName, oftTm, false, false, true, false);
            FieldH.AddDat(FieldDesc.GetFieldNm(), FieldDesc);

            TFieldDescEx FieldDescEx;
            FieldDescEx.FieldStoreLoc = slDisk;
            FieldDescEx.SmallStringP = false;
            FieldDescEx.CodebookP = false;
            FieldExH.AddDat(FieldDesc.GetFieldNm(), FieldDescEx);

            WndDesc.TimeFieldNm = FieldDesc.GetFieldNm();
            WndDesc.InsertP = true;
        }
    }
}

void TStoreSchema::ParseSchema(const TWPt<TBase>& Base, const PJsonVal& SchemaVal, TStoreSchemaV& SchemaV) {
    if (SchemaVal->IsArr()) {
        for (int SchemaN = 0; SchemaN < SchemaVal->GetArrVals(); SchemaN++) {
            SchemaV.Add(TStoreSchema(Base, SchemaVal->GetArrVal(SchemaN)));
        }
    } else if (SchemaVal->IsObjKey("stores")) {
        ParseSchema(Base, SchemaVal->GetObjKey("stores"), SchemaV);
    } else {
        SchemaV.Add(TStoreSchema(Base, SchemaVal));
    }
}

void TStoreSchema::ValidateSchema(const TWPt<TBase>& Base, TStoreSchemaV& SchemaV) {
    TStrH StoreNameH;
    TUIntV ReqStoreIdV;

    for (int SchemaN = 0; SchemaN < SchemaV.Len(); SchemaN++){
        TStoreSchema& Schema = SchemaV[SchemaN];
        // unique store names
        TStr StoreName = Schema.StoreName;
        QmAssertR(!Base->IsStoreNm(StoreName), "Store already exists: " + StoreName);
        QmAssertR(!StoreNameH.IsKey(StoreName), "Duplicate store name: " + StoreName);
        StoreNameH.AddDat(StoreName, 0);
        // check unique store ids
        if (SchemaV[SchemaN].HasStoreIdP) {
            uint ReqStoreId = Schema.StoreId;
            for (int ReqStoreIdN = 0; ReqStoreIdN < ReqStoreIdV.Len(); ReqStoreIdN++) {
                QmAssertR(ReqStoreId != ReqStoreIdV[ReqStoreIdN],
                    "Duplicate store id " + TInt::GetStr(ReqStoreId));
            }
            ReqStoreIdV.Add(ReqStoreId);
        }
        // valid field names inside the store and primary key
        TStr PrimaryFieldName;
        int FieldKeyId = Schema.FieldH.FFirstKeyId();
        while (Schema.FieldH.FNextKeyId(FieldKeyId)) {
            TStr FieldName = Schema.FieldH[FieldKeyId].GetFieldNm();
            const TFieldDesc& FieldDesc = Schema.FieldH[FieldKeyId];
            Base->AssertValidNm(FieldName);
            // determine primary field matches constraints
            if (FieldDesc.IsPrimary()) {
                // more than one field is marked as "primary"
                QmAssertR(PrimaryFieldName.Empty(), "More than one field is marked as primary: " + PrimaryFieldName + " and " + FieldDesc.GetFieldNm());
                // fields marked as "primary" must be strings
                QmAssertR(FieldDesc.IsStr() || FieldDesc.IsInt() || FieldDesc.IsUInt64() || FieldDesc.IsFlt() || FieldDesc.IsTm(),
                    "Field marked as primary must be of type string, int, uint64, float or time: " + FieldDesc.GetFieldNm());
                // fields marked as "primary" must be strings
                QmAssertR(!FieldDesc.IsNullable(), "Filed marked as primary cannot be nullable: " + FieldDesc.GetFieldNm());
                // all fine, mark it as primary
                PrimaryFieldName = FieldName;
            }
        }

        // check that window parameter for field is valid
        if (Schema.WndDesc.WindowType == swtTime) {
            const TStr& WndFieldName = Schema.WndDesc.TimeFieldNm;
            QmAssertR(Schema.FieldH.IsKey(WndFieldName), "Field " + WndFieldName +
                " should be used as the source for window is store " + StoreName +
                ", but it doesn't exist.");
            const TFieldDesc& FieldDesc = Schema.FieldH.GetDat(WndFieldName);
            QmAssertR(FieldDesc.IsTm(), "Field " + WndFieldName +
                " should be used as the source for window is store " + StoreName +
                ", but it is not of datetime type.");
            QmAssertR(!FieldDesc.IsNullable(), "Field " + WndFieldName +
                " should be used as the source for window is store " + StoreName +
                ", but it is nullable.");
        }
        // joins
        TStrH JoinNameH;
        for (int JoinN = 0; JoinN < Schema.JoinDescExV.Len(); JoinN++){
            TStr JoinName = Schema.JoinDescExV[JoinN].JoinName;
            QmAssertR(!JoinNameH.IsKey(JoinName),
                "Duplicate join name " + JoinName + " in store " + StoreName);
            JoinNameH.AddKey(JoinName);
            // check if the other store exists
            TStr JoinStoreName = Schema.JoinDescExV[JoinN].JoinStoreName;
            // check if a store exists with that name in the schemas given
            bool FoundP = false;
            for (int SchemaN = 0; SchemaN < SchemaV.Len(); SchemaN++){
                if (SchemaV[SchemaN].StoreName == JoinStoreName){
                    FoundP = true;
                    break;
                }
            }
            // check for an existing store with that name
            if(!FoundP) {
                for (int StoreN = 0; StoreN < Base->GetStores(); StoreN++){
                    if (Base->GetStoreByStoreN(StoreN)->GetStoreNm() == JoinStoreName) {
                        FoundP = true;
                        break;
                    }
                }
            }
            QmAssertR(FoundP, "Illegal join " + JoinName + " in store " +
                StoreName + " - joined store " + JoinStoreName + " not found");
        }
    }
}

///////////////////////////////
// In-memory storage
TInMemStorage::TInMemStorage(const TStr& _FNm, const PBlobBs& _BlobStorage, const int& _BlockSize):
    FNm(_FNm), Access(faCreate), BlobStorage(_BlobStorage), BlockSize(_BlockSize) { }

TInMemStorage::TInMemStorage(const TStr& _FNm, const PBlobBs& _BlobStorage, const TFAccess& _FAccess,
        const bool& LazyP): FNm(_FNm), Access(_FAccess), BlobStorage(_BlobStorage) {

    // load data
    TFIn FIn(FNm);
    BlobPtV.Load(FIn); // load vector
    // load rest
    TInt64 cnt;
    cnt.Load(FIn);
    FirstValOffset.Load(FIn);
    FirstValOffsetMem.Load(FIn);
    BlockSize.Load(FIn);

    for (int64 i = 0; i < cnt; i++) {
        ValV.Add(); // empty (non-loaded) data
        DirtyV.Add(isdfNotLoaded); // init dirty flags
    }
    if (!LazyP) {
        LoadAll();
    }
}

TInMemStorage::~TInMemStorage() {
    if (Access != faRdOnly) {
        // store dirty vectors
        for (int i = 0; i < ValV.Len(); i++) {
            SaveRec(i);
        }
        // save vector
        TFOut FOut(FNm);
        BlobPtV.Save(FOut);
        // save rest
        TInt64(ValV.Len()).Save(FOut);
        FirstValOffset.Save(FOut);
        FirstValOffsetMem.Save(FOut);
        BlockSize.Save(FOut);
    }
}

/// Utility method for loading specific record
void TInMemStorage::LoadRec(int64 RecN) const {
    if (DirtyV[RecN] != isdfNotLoaded) { return; }
    const int64 ii = RecN / BlockSize;
    TMem mem;
    TMem::LoadMem(BlobStorage->GetBlob(BlobPtV[ii]), mem);
    PSIn in = mem.GetSIn();
    for (int64 j = ii*BlockSize; j < DirtyV.Len() && j < (ii + 1)*BlockSize; j++) {
        if (DirtyV[j] == isdfNotLoaded) {
            DirtyV[j] = isdfClean;
            ValV[j].Load(in);
        } else {
            TMem mem2;
            mem2.Load(in);
        }
    }
}

/// Utility method for storing specific record
int TInMemStorage::SaveRec(int RecN) {
    int res = 0;
    switch (DirtyV[RecN]) {
    case isdfNew:
    case isdfDirty:
        {
            res++;
            const int ii = RecN / BlockSize;
            TMOut mem;
            for (int j = ii*BlockSize; j < DirtyV.Len() && j < (ii + 1)*BlockSize; j++) {
                ValV[j].Save(mem);
                DirtyV[j] = isdfClean;
            }
            while (BlobPtV.Len() <= ii) {
                BlobPtV.Add();
            }
            if (BlobPtV[ii].Empty()) {
                BlobPtV[ii] = BlobStorage->PutBlob(mem.GetSIn());
            } else {
                int ReleasedSize;
                BlobPtV[ii] = BlobStorage->PutBlob(BlobPtV[ii], mem.GetSIn(), ReleasedSize);
            }
        }
        break;
    case isdfClean:
    case isdfNotLoaded: break;
    }
    return res;
}

void TInMemStorage::AssertReadOnly() const {
    QmAssertR(((Access == faCreate) || (Access == faUpdate)), FNm + " opened in Read-Only mode!");
}

bool TInMemStorage::IsValId(const uint64& ValId) const {
    return
        (ValId >= FirstValOffsetMem.Val + FirstValOffset) &&
        (ValId < FirstValOffsetMem.Val + ValV.Len());
}

void TInMemStorage::GetVal(const uint64& ValId, TMem& Val) const {
    uint64 i = ValId - FirstValOffsetMem;
    LoadRec(i);
    Val = ValV[i];
}

uint64 TInMemStorage::AddVal(const TMem& Val) {
    uint64 res = ValV.Add(Val);
    DirtyV.Add(isdfNew);
    if (ValV.Len() % BlockSize == 1) {
        BlobPtV.Add();
    }
    return res + FirstValOffsetMem;
}

void TInMemStorage::SetVal(const uint64& ValId, const TMem& Val) {
    AssertReadOnly();
    ValV[ValId - FirstValOffsetMem] = Val;
    uchar& flag = DirtyV[ValId - FirstValOffsetMem];
    if (flag == isdfNew) { } // new remains new
    else { flag = isdfDirty; } // set as dirty
}

void TInMemStorage::DelVals(int Vals) {
    if (Vals > 0) {
        int ValsTrue = 0;
        for (ValsTrue = 0; ValsTrue < Vals && ValsTrue + (int64)FirstValOffset.Val<ValV.Len(); ValsTrue++) {
            ValV[ValsTrue + FirstValOffset].Clr();
        }
        int blocks_to_delete = ((int)FirstValOffset + ValsTrue) / BlockSize;
        int vals_to_delete = blocks_to_delete * BlockSize;

        if (vals_to_delete > 0) {
            ValV.Del(0, vals_to_delete - 1);
            DirtyV.Del(0, vals_to_delete - 1);
            for (int i = 0; i < blocks_to_delete; i++) {
                if (!BlobPtV[i].Empty()) {
                    BlobStorage->DelBlob(BlobPtV[i]);
                }
            }
            BlobPtV.Del(0, blocks_to_delete - 1);
        }
        FirstValOffset += ValsTrue - vals_to_delete;
        FirstValOffsetMem += vals_to_delete;
    }
}

uint64 TInMemStorage::Len() const {
    return ValV.Len() - FirstValOffset;
}

uint64 TInMemStorage::GetFirstValId() const {
    return FirstValOffsetMem + FirstValOffset;
}

uint64 TInMemStorage::GetLastValId() const {
    return FirstValOffsetMem + ValV.Len() - 1;
}

int TInMemStorage::PartialFlush(int WndInMsec) {
    TTmStopWatch sw(true);
    int res = 0;
    for (int i = 0; i< ValV.Len(); i++) {
        if (sw.GetMSecInt() > WndInMsec)
            break;
        res += SaveRec(i);
    }
    return res;
}

void TInMemStorage::LoadAll() {
    for (int i = 0; i < ValV.Len(); i++) {
        LoadRec(i);
    }
}

///////////////////////////////
// Field serialization parameters
void TRecSerializator::TFieldSerialDesc::Save(TSOut& SOut) const {
    FieldId.Save(SOut);
    TInt(StoreLoc).Save(SOut);
    NullMapByte.Save(SOut);
    NullMapMask.Save(SOut);
    FixedPartP.Save(SOut);
    Offset.Save(SOut);
    CodebookP.Save(SOut);
    SmallStringP.Save(SOut);
    DefaultVal.Save(SOut);
}

void TRecSerializator::TFieldSerialDesc::Load(TSIn& SIn) {
    FieldId.Load(SIn);
    StoreLoc = TStoreLoc(TInt(SIn).Val);
    NullMapByte = TUCh(SIn);
    NullMapMask = TUCh(SIn);
    FixedPartP.Load(SIn);
    Offset.Load(SIn);
    CodebookP.Load(SIn);
    SmallStringP.Load(SIn);
    DefaultVal = PJsonVal(SIn);
}

/// Flag if field is not TOAST-ed
const char TRecSerializator::ToastNo = 'n';
/// Flag if field is TOAST-ed
const char TRecSerializator::ToastYes = 'y';

///////////////////////////////
// Serialization and de-serialization of records to TMem

TStr TRecSerializator::GetErrorMsg(const TMem& RecMem, const TFieldSerialDesc& FieldSerialDesc) const {
    return TStr::Fmt("FPO:%d VIPO:%d VCPO:%d|L:%d|FID:%d NMP:%d NMM:%d FP:%s O:%d",
        FixedPartOffset.Val, VarIndexPartOffset.Val, VarContentPartOffset.Val,
        RecMem.Len(), FieldSerialDesc.FieldId.Val, (int)FieldSerialDesc.NullMapByte.Val,
        (int)FieldSerialDesc.NullMapMask.Val, FieldSerialDesc.FixedPartP ? "T" : "F",
        FieldSerialDesc.Offset);
}
TStr TRecSerializator::GetErrorMsg(const char* Bf, const int& BfL, const TFieldSerialDesc& FieldSerialDesc) const {
    return TStr::Fmt("FPO:%d VIPO:%d VCPO:%d|L:%d|FID:%d NMP:%d NMM:%d FP:%s O:%d",
        FixedPartOffset.Val, VarIndexPartOffset.Val, VarContentPartOffset.Val,
        BfL, FieldSerialDesc.FieldId.Val, (int)FieldSerialDesc.NullMapByte.Val,
        (int)FieldSerialDesc.NullMapMask.Val, FieldSerialDesc.FixedPartP ? "T" : "F",
        FieldSerialDesc.Offset);
}
const TRecSerializator::TFieldSerialDesc& TRecSerializator::GetFieldSerialDesc(const int& FieldId) const {
    QmAssertR(FieldIdToSerialDescIdH.IsKey(FieldId),
        "Field with ID not found: " + TInt::GetStr(FieldId));
    return FieldSerialDescV[FieldIdToSerialDescIdH.GetDat(FieldId)];
}

//////////////////////

char* TRecSerializator::GetLocationFixed(const TMemBase& RecMem,
        const TFieldSerialDesc& FieldSerialDesc) const {
    return GetLocationFixed(RecMem.GetBf(), RecMem.Len(), FieldSerialDesc);
}

int TRecSerializator::GetOffsetVar(const TMemBase& RecMem,
        const TFieldSerialDesc& FieldSerialDesc) const {
    return GetOffsetVar(RecMem.GetBf(), RecMem.Len(), FieldSerialDesc);
}

char* TRecSerializator::GetLocationVar(const TMemBase& RecMem,
        const TFieldSerialDesc& FieldSerialDesc) const {
    return GetLocationVar(RecMem.GetBf(), RecMem.Len(), FieldSerialDesc);
}

int TRecSerializator::GetVarPartBfLen(const TMemBase& RecMem,
        const TFieldSerialDesc& FieldSerialDesc) {
    return GetVarPartBfLen(RecMem.GetBf(), RecMem.Len(), FieldSerialDesc);
}

//////////////////////////

/// finds location inside the buffer for fixed-width fields
char* TRecSerializator::GetLocationFixed(TThinMIn min, const TFieldSerialDesc& FieldSerialDesc) const {
    return GetLocationFixed(min.GetBfAddrChar(), min.Len(), FieldSerialDesc);
}
/// finds location inside the buffer for variable-width fields
int TRecSerializator::GetOffsetVar(TThinMIn min, const TFieldSerialDesc& FieldSerialDesc) const {
    return GetOffsetVar(min.GetBfAddrChar(), min.Len(), FieldSerialDesc);
}
/// finds location inside the buffer for variable-width fields
char* TRecSerializator::GetLocationVar(TThinMIn min, const TFieldSerialDesc& FieldSerialDesc) const {
    return GetLocationVar(min.GetBfAddrChar(), min.Len(), FieldSerialDesc);
}
/// calculates length of buffer where given var-length field is stored
int TRecSerializator::GetVarPartBfLen(TThinMIn min, const TFieldSerialDesc& FieldSerialDesc) {
    return GetVarPartBfLen(min.GetBfAddrChar(), min.Len(), FieldSerialDesc);
}

/////////////////////////

char* TRecSerializator::GetLocationFixed(char* Bf, const int& BfL,
    const TFieldSerialDesc& FieldSerialDesc) const {

    char* bf = Bf + FixedPartOffset + FieldSerialDesc.Offset;
    AssertR(bf < Bf + BfL, GetErrorMsg(Bf, BfL, FieldSerialDesc));
    return bf;
}

int TRecSerializator::GetOffsetVar(const char* Bf, const int& BfL,
    const TFieldSerialDesc& FieldSerialDesc) const {

    int Offset = *((int*)(Bf + VarIndexPartOffset + FieldSerialDesc.Offset));
    AssertR(VarContentPartOffset + Offset < BfL, GetErrorMsg(Bf, BfL, FieldSerialDesc));
    return VarContentPartOffset + Offset;
}

char* TRecSerializator::GetLocationVar(char* Bf, const int& BfL,
    const TFieldSerialDesc& FieldSerialDesc) const {

    int Offset = *((int*)(Bf + VarIndexPartOffset + FieldSerialDesc.Offset));
    char* bf2 = Bf + VarContentPartOffset + Offset;
    AssertR(bf2 < Bf + BfL, GetErrorMsg(Bf, BfL, FieldSerialDesc));
    return bf2;
}

int TRecSerializator::GetVarPartBfLen(const char* Bf, const int& BfL,
    const TFieldSerialDesc& FieldSerialDesc) {
    const char* bf = (Bf + VarIndexPartOffset.Val + FieldSerialDesc.Offset);
    int Offset1 = *((int*)bf);
    int Offset2 = -1;
    if (VarIndexPartOffset + FieldSerialDesc.Offset + (int)sizeof(int) < VarContentPartOffset) {
        Offset2 = *((int*)(bf + sizeof(int)));
    } else {
        Offset2 = BfL - VarContentPartOffset;
    }
    AssertR(Offset2 - Offset1 >= 0, GetErrorMsg(Bf, BfL, FieldSerialDesc));
    return Offset2 - Offset1;
}

///////////////////////////////

void TRecSerializator::SetLocationVar(TMem& RecMem,
        const TFieldSerialDesc& FieldSerialDesc, const int& VarOffset) const {
    SetLocationVar(RecMem.GetBf(), RecMem.Len(), FieldSerialDesc, VarOffset);
}

void TRecSerializator::SetFieldNull(TMem& RecMem,
        const TFieldSerialDesc& FieldSerialDesc, const bool& NullP) const {
    SetFieldNull(RecMem.GetBf(), RecMem.Len(), FieldSerialDesc, NullP);
}

/// set content offset for specified variable field
void TRecSerializator::SetLocationVar(char* Bf, const int& BfL, const TFieldSerialDesc& FieldSerialDesc, const int& VarOffset) const {
    AssertR(VarIndexPartOffset + FieldSerialDesc.Offset <= (BfL - 4), GetErrorMsg(Bf, BfL, FieldSerialDesc));
    *((int*)(Bf + VarIndexPartOffset + FieldSerialDesc.Offset)) = VarOffset;
}
/// sets or un-sets NULL flag for specified field
void TRecSerializator::SetFieldNull(char* Bf, const int& BfL, const TFieldSerialDesc& FieldSerialDesc, const bool& NullP) const {
    char* bf = Bf + FieldSerialDesc.NullMapByte;
    AssertR(bf < Bf + BfL, GetErrorMsg(Bf, BfL, FieldSerialDesc));
    if (NullP) {
        *bf |= FieldSerialDesc.NullMapMask;
    } else {
        *bf &= ~FieldSerialDesc.NullMapMask;
    }
}

void TRecSerializator::SetFieldNull(char* Bf, const int& BfL, const int& FieldId, const bool& NullP) {
    SetFieldNull(Bf, BfL, GetFieldSerialDesc(FieldId), NullP);
}

void TRecSerializator::SetFieldByte(char* Bf, const int& BfL,
    const TFieldSerialDesc& FieldSerialDesc, const uchar& Byte) {

    char* bf = GetLocationFixed(Bf, BfL, FieldSerialDesc);
    *((uchar*)bf) = Byte;
    // set the null field to false
    SetFieldNull(Bf, BfL, FieldSerialDesc, false);
}
void TRecSerializator::SetFieldInt(char* Bf, const int& BfL,
    const TFieldSerialDesc& FieldSerialDesc, const int& Int) {

    char* bf = GetLocationFixed(Bf, BfL, FieldSerialDesc);
    *((int*)bf) = Int;
    // set the null field to false
    SetFieldNull(Bf, BfL, FieldSerialDesc, false);
}
void TRecSerializator::SetFieldInt16(char* Bf, const int& BfL,
    const TFieldSerialDesc& FieldSerialDesc, const int16& Int16) {

    char* bf = GetLocationFixed(Bf, BfL, FieldSerialDesc);
    *((int16*)bf) = Int16;
    // set the null field to false
    SetFieldNull(Bf, BfL, FieldSerialDesc, false);
}
void TRecSerializator::SetFieldInt64(char* Bf, const int& BfL,
    const TFieldSerialDesc& FieldSerialDesc, const int64& Int64) {

    char* bf = GetLocationFixed(Bf, BfL, FieldSerialDesc);
    *((int64*)bf) = Int64;
    // set the null field to false
    SetFieldNull(Bf, BfL, FieldSerialDesc, false);
}

void TRecSerializator::SetFieldUInt(char* Bf, const int& BfL,
    const TFieldSerialDesc& FieldSerialDesc, const uint& UInt) {

    char* bf = GetLocationFixed(Bf, BfL, FieldSerialDesc);
    *((uint*)bf) = UInt;
    // set the null field to false
    SetFieldNull(Bf, BfL, FieldSerialDesc, false);
}
void TRecSerializator::SetFieldUInt16(char* Bf, const int& BfL,
    const TFieldSerialDesc& FieldSerialDesc, const uint16& UInt16) {

    char* bf = GetLocationFixed(Bf, BfL, FieldSerialDesc);
    *((uint16*)bf) = UInt16;
    // set the null field to false
    SetFieldNull(Bf, BfL, FieldSerialDesc, false);
}
void TRecSerializator::SetFieldUInt64(char* Bf, const int& BfL,
    const TFieldSerialDesc& FieldSerialDesc, const uint64& UInt64) {

    char* bf = GetLocationFixed(Bf, BfL, FieldSerialDesc);
    *((uint64*)bf) = UInt64;
    // set the null field to false
    SetFieldNull(Bf, BfL, FieldSerialDesc, false);
}

void TRecSerializator::SetFieldStr(char* Bf, const int& BfL,
    const TFieldSerialDesc& FieldSerialDesc, const TStr& Str) {

    char* bf = GetLocationFixed(Bf, BfL, FieldSerialDesc);
    const int StrId = CodebookH.AddKey(Str);
    *((int*)bf) = StrId;
    // set the null field to false
    SetFieldNull(Bf, BfL, FieldSerialDesc, false);
}

void TRecSerializator::SetFieldBool(char* Bf, const int& BfL,
    const TFieldSerialDesc& FieldSerialDesc, const bool& Bool) {

    char* bf = GetLocationFixed(Bf, BfL, FieldSerialDesc);
    *((bool*)bf) = Bool;
    // set the null field to false
    SetFieldNull(Bf, BfL, FieldSerialDesc, false);
}

void TRecSerializator::SetFieldFlt(char* Bf, const int& BfL,
    const TFieldSerialDesc& FieldSerialDesc, const double& Flt) {

    char* bf = GetLocationFixed(Bf, BfL, FieldSerialDesc);
    *((double*)bf) = Flt;
    // set the null field to false
    SetFieldNull(Bf, BfL, FieldSerialDesc, false);
}
void TRecSerializator::SetFieldSFlt(char* Bf, const int& BfL,
    const TFieldSerialDesc& FieldSerialDesc, const float& SFlt) {

    char* bf = GetLocationFixed(Bf, BfL, FieldSerialDesc);
    *((float*)bf) = SFlt;
    // set the null field to false
    SetFieldNull(Bf, BfL, FieldSerialDesc, false);
}

void TRecSerializator::SetFieldFltPr(char* Bf, const int& BfL,
    const TFieldSerialDesc& FieldSerialDesc, const TFltPr& FltPr) {

    char* bf = GetLocationFixed(Bf, BfL, FieldSerialDesc);
    *((double*)bf) = FltPr.Val1.Val;
    *(((double*)bf) + 1) = FltPr.Val2.Val;
    // set the null field to false
    SetFieldNull(Bf, BfL, FieldSerialDesc, false);
}

void TRecSerializator::SetFieldTm(char* Bf, const int& BfL,
    const TFieldSerialDesc& FieldSerialDesc, const TTm& Tm) {

    char* bf = GetLocationFixed(Bf, BfL, FieldSerialDesc);
    uint64 TmMSecs = TTm::GetMSecsFromTm(Tm);
    *((uint64*)bf) = TmMSecs;
    // set the null field to false
    SetFieldNull(Bf, BfL, FieldSerialDesc, false);
}

void TRecSerializator::SetFieldTmMSecs(char* Bf, const int& BfL,
    const TFieldSerialDesc& FieldSerialDesc, const uint64& TmMSecs) {

    char* bf = GetLocationFixed(Bf, BfL, FieldSerialDesc);
    *((uint64*)bf) = TmMSecs;
    // set the null field to false
    SetFieldNull(Bf, BfL, FieldSerialDesc, false);
}

void TRecSerializator::SetFixedJsonVal(char* Bf, const int& BfL,
    const TFieldSerialDesc& FieldSerialDesc, const TFieldDesc& FieldDesc,
    const PJsonVal& JsonVal) {

    // call type-appropriate setter
    switch (FieldDesc.GetFieldType()) {
    case oftByte:
        QmAssertR(JsonVal->IsNum(), "Provided JSon data field " + FieldDesc.GetFieldNm() + " is not numeric.");
        SetFieldByte(Bf, BfL, FieldSerialDesc, (uchar)JsonVal->GetUInt64());
        break;
    case oftInt:
        QmAssertR(JsonVal->IsNum(), "Provided JSon data field " + FieldDesc.GetFieldNm() + " is not numeric.");
        SetFieldInt(Bf, BfL, FieldSerialDesc, JsonVal->GetInt());
        break;
    case oftInt16:
        QmAssertR(JsonVal->IsNum(), "Provided JSon data field " + FieldDesc.GetFieldNm() + " is not numeric.");
        SetFieldInt16(Bf, BfL, FieldSerialDesc, (int16)JsonVal->GetInt());
        break;
    case oftInt64:
        QmAssertR(JsonVal->IsNum(), "Provided JSon data field " + FieldDesc.GetFieldNm() + " is not numeric.");
        SetFieldInt64(Bf, BfL, FieldSerialDesc, (int64)JsonVal->GetNum());
        break;
    case oftUInt:
        QmAssertR(JsonVal->IsNum(), "Provided JSon data field " + FieldDesc.GetFieldNm() + " is not numeric.");
        SetFieldUInt(Bf, BfL, FieldSerialDesc, (uint)JsonVal->GetUInt64());
        break;
    case oftUInt16:
        QmAssertR(JsonVal->IsNum(), "Provided JSon data field " + FieldDesc.GetFieldNm() + " is not numeric.");
        SetFieldUInt16(Bf, BfL, FieldSerialDesc, (uint16)JsonVal->GetUInt64());
        break;
    case oftUInt64:
        QmAssertR(JsonVal->IsNum(), "Provided JSon data field " + FieldDesc.GetFieldNm() + " is not numeric.");
        SetFieldUInt64(Bf, BfL, FieldSerialDesc, (uint64)JsonVal->GetUInt64());
        break;
    case oftStr:
        // this string should be encoded using a codebook
        QmAssertR(JsonVal->IsStr(), "Provided JSon data field " + FieldDesc.GetFieldNm() + " is not string.");
        SetFieldStr(Bf, BfL, FieldSerialDesc, JsonVal->GetStr());
        break;
    case oftBool:
        QmAssertR(JsonVal->IsBool(), "Provided JSon data field " + FieldDesc.GetFieldNm() + " is not boolean.");
        SetFieldBool(Bf, BfL, FieldSerialDesc, JsonVal->GetBool());
        break;
    case oftFlt:
        QmAssertR(JsonVal->IsNum(), "Provided JSon data field " + FieldDesc.GetFieldNm() + " is not numeric.");
        SetFieldFlt(Bf, BfL, FieldSerialDesc, JsonVal->GetNum());
        break;
    case oftSFlt:
        QmAssertR(JsonVal->IsNum(), "Provided JSon data field " + FieldDesc.GetFieldNm() + " is not numeric.");
        SetFieldSFlt(Bf, BfL, FieldSerialDesc, (float)JsonVal->GetNum());
        break;
    case oftFltPr: {
        // make sure it's array of length two
        QmAssertR(JsonVal->IsArr(), "Provided JSon data field " + FieldDesc.GetFieldNm() + " is not array.");
        QmAssertR(JsonVal->GetArrVals() == 2, "Provided JSon data field " + FieldDesc.GetFieldNm() + " is not array - expected 2 fields.");
        PJsonVal JsonVal1 = JsonVal->GetArrVal(0);
        PJsonVal JsonVal2 = JsonVal->GetArrVal(1);
        // make sure both elements are numeric
        QmAssertR(JsonVal1->IsNum(), "The first element in the JSon array in data field " + FieldDesc.GetFieldNm() + " is not numeric.");
        QmAssertR(JsonVal2->IsNum(), "The second element in the JSon array in data field " + FieldDesc.GetFieldNm() + " is not numeric.");
        // update
        SetFieldFltPr(Bf, BfL, FieldSerialDesc, TFltPr(JsonVal1->GetNum(), JsonVal2->GetNum()));
        break;
    }
    case oftTm: {
        QmAssertR(JsonVal->IsStr() || JsonVal->IsNum(), "Provided JSon data field " + FieldDesc.GetFieldNm() + " is not a number or a string that represents DateTime.");
        if (JsonVal->IsStr()) {
            TStr TmStr = JsonVal->GetStr();
            TTm Tm = TTm::GetTmFromWebLogDateTimeStr(TmStr, '-', ':', '.', 'T');
            SetFieldTm(Bf, BfL, FieldSerialDesc, Tm);
        } else {
            uint64 WinMSecs = TTm::GetWinMSecsFromUnixMSecs(JsonVal->GetInt64());
            TTm Tm = TTm::GetTmFromMSecs(WinMSecs);
            SetFieldTm(Bf, BfL, FieldSerialDesc, Tm);
        }
        break;
    }
    default:
        throw TQmExcept::New("Unsupported JSon data type for DB storage (fixed part): " + FieldDesc.GetFieldTypeStr());
    }
}

/////////////////////

/// Fixed-length field setter
void TRecSerializator::SetFieldByte(char* Bf, const int& BfL, const int& FieldId, const uchar& Byte) {
    SetFieldByte(Bf, BfL, GetFieldSerialDesc(FieldId), Byte);
}
/// Fixed-length field setter
void TRecSerializator::SetFieldInt(char* Bf, const int& BfL, const int& FieldId, const int& Int) {
    SetFieldInt(Bf, BfL, GetFieldSerialDesc(FieldId), Int);
}
/// Fixed-length field setter
void TRecSerializator::SetFieldInt16(char* Bf, const int& BfL, const int& FieldId, const int16& Int16) {
    SetFieldInt16(Bf, BfL, GetFieldSerialDesc(FieldId), Int16);
}
/// Fixed-length field setter
void TRecSerializator::SetFieldInt64(char* Bf, const int& BfL, const int& FieldId, const int64& Int64) {
    SetFieldInt64(Bf, BfL, GetFieldSerialDesc(FieldId), Int64);
}
/// Fixed-length field setter
void TRecSerializator::SetFieldUInt(char* Bf, const int& BfL, const int& FieldId, const uint& UInt) {
    SetFieldUInt(Bf, BfL, GetFieldSerialDesc(FieldId), UInt);
}
/// Fixed-length field setter
void TRecSerializator::SetFieldUInt16(char* Bf, const int& BfL, const int& FieldId, const uint16& UInt16) {
    SetFieldUInt16(Bf, BfL, GetFieldSerialDesc(FieldId), UInt16);
}
/// Fixed-length field setter
void TRecSerializator::SetFieldUInt64(char* Bf, const int& BfL, const int& FieldId, const uint64& UInt64) {
    SetFieldUInt64(Bf, BfL, GetFieldSerialDesc(FieldId), UInt64);
}
/// Fixed-length field setter
void TRecSerializator::SetFieldStr(char* Bf, const int& BfL, const int& FieldId, const TStr& Str) {
    SetFieldStr(Bf, BfL, GetFieldSerialDesc(FieldId), Str);
}
/// Fixed-length field setter
void TRecSerializator::SetFieldBool(char* Bf, const int& BfL, const int& FieldId, const bool& Bool) {
    SetFieldBool(Bf, BfL, GetFieldSerialDesc(FieldId), Bool);
}
/// Fixed-length field setter
void TRecSerializator::SetFieldFlt(char* Bf, const int& BfL, const int& FieldId, const double& Flt) {
    SetFieldFlt(Bf, BfL, GetFieldSerialDesc(FieldId), Flt);
}
/// Fixed-length field setter
void TRecSerializator::SetFieldSFlt(char* Bf, const int& BfL, const int& FieldId, const float& Flt) {
    SetFieldSFlt(Bf, BfL, GetFieldSerialDesc(FieldId), Flt);
}
/// Fixed-length field setter
void TRecSerializator::SetFieldFltPr(char* Bf, const int& BfL, const int& FieldId, const TFltPr& FltPr) {
    SetFieldFltPr(Bf, BfL, GetFieldSerialDesc(FieldId), FltPr);
}
/// Fixed-length field setter
void TRecSerializator::SetFieldTm(char* Bf, const int& BfL, const int& FieldId, const TTm& Tm) {
    SetFieldTm(Bf, BfL, GetFieldSerialDesc(FieldId), Tm);
}
/// Fixed-length field setter
void TRecSerializator::SetFieldTmMSecs(char* Bf, const int& BfL, const int& FieldId, const uint64& TmMSecs) {
    SetFieldTmMSecs(Bf, BfL, GetFieldSerialDesc(FieldId), TmMSecs);
}

/// Destructor that calls parent
TRecSerializator::TToastWatcher::~TToastWatcher() {
    for (int i = 0; i < Parent->ToastPtToDel.Len(); i++) {
        Parent->Toaster->DelToastVal(Parent->ToastPtToDel[i]);
    }
    Parent->ToastPtToDel.Clr();
}

/////////////////////

/// Check this temporary buffer if it must be TOAST-ed
void TRecSerializator::CheckToast(TMOut& SOut, const int& Offset) {
    if (!UseToast) return;
    if (SOut.Len() <= MxToastLen) return;

    // ok, perform TOAST-ing
    // Store serialized data into store
    TMemBase mb(SOut.GetBfAddr() + Offset + 1, SOut.Len() - Offset - 1, false);
    TPgBlobPt Pt = Toaster->ToastVal(mb);
    // rewind SOut back to Offset
    SOut.Seek(Offset);
    SOut.PutCh(ToastYes);
    // Serialize
    SOut.AppendBf(&Pt, sizeof(TPgBlobPt));
}

/// Collect byte offsets (within the serialized record) of the blob pointers of
/// all TOAST-ed field values. Used when relocating records to a new blob storage.
void TRecSerializator::GetToastBlobPtOffsets(const TMemBase& RecMem, TIntV& OffsetV) const {
    OffsetV.Clr();
    if (!UseToast) { return; }
    for (int FieldSerialDescN = 0; FieldSerialDescN < FieldSerialDescV.Len(); FieldSerialDescN++) {
        const TFieldSerialDesc& FieldSerialDesc = FieldSerialDescV[FieldSerialDescN];
        if (FieldSerialDesc.FixedPartP) { continue; }
        if (IsFieldNull(RecMem, FieldSerialDesc.FieldId)) { continue; }
        char* Bf = GetLocationVar(RecMem, FieldSerialDesc);
        if (*Bf == ToastYes) {
            // the blob pointer is stored right after the TOAST marker character
            OffsetV.Add((int) (Bf + 1 - RecMem.GetBf()));
        }
    }
}

/// Check if given field value is currently TOAST-ed and delete it
void TRecSerializator::CheckToastDel(const TMemBase& InRecMem, const TFieldSerialDesc& FieldSerialDesc) {
    if (UseToast && !FieldSerialDesc.FixedPartP) {
        if (!IsFieldNull(InRecMem, FieldSerialDesc.FieldId)) {
            char* Bf = GetLocationVar(InRecMem, FieldSerialDesc);
            char c = *Bf;
            if (c == ToastYes) {
                Bf++;
                TPgBlobPt Pt;
                Pt = *((TPgBlobPt*)Bf);
                ToastPtToDel.Add(Pt);
            }
        }
    }
}

void TRecSerializator::SetFieldIntV(TMem& RecMem, TMOut& SOut,
        const TFieldSerialDesc& FieldSerialDesc, const TIntV& IntV) {

    // location of the new variable-length value is at the end of current output stream
    int VarContentOffset = SOut.Len();
    // update it's location in the variable-index
    SetLocationVar(RecMem, FieldSerialDesc, VarContentOffset);
    // update value
    if (UseToast) { SOut.PutCh(ToastNo); }
    IntV.Save(SOut);
    // Perform TOAST-ing if needed
    CheckToast(SOut, VarContentOffset);
}

void TRecSerializator::SetFieldStr(TMem& RecMem, TMOut& SOut,
        const TFieldSerialDesc& FieldSerialDesc, const TStr& Str) {

    // location of the new variable-length value is at the end of current output stream
    int VarContentOffset = SOut.Len();
    // update it's location in the variable-index
    SetLocationVar(RecMem, FieldSerialDesc, VarContentOffset);
    // update value
    if (UseToast) { SOut.PutCh(ToastNo); }
    Str.Save(SOut, FieldSerialDesc.SmallStringP);
    // Perform TOAST-ing if needed
    CheckToast(SOut, VarContentOffset);
}

void TRecSerializator::SetFieldStrV(TMem& RecMem, TMOut& SOut,
        const TFieldSerialDesc& FieldSerialDesc, const TStrV& StrV) {

    // location of the new variable-length value is at the end of current output stream
    int VarContentOffset = SOut.Len();
    // update it's location in the variable-index
    SetLocationVar(RecMem, FieldSerialDesc, VarContentOffset);
    // update value
    if (UseToast) { SOut.PutCh(ToastNo); }
    StrV.Save(SOut);
    // Perform TOAST-ing if needed
    CheckToast(SOut, VarContentOffset);
}

void TRecSerializator::SetFieldFltV(TMem& RecMem, TMOut& SOut,
        const TFieldSerialDesc& FieldSerialDesc, const TFltV& FltV) {

    // location of the new variable-length value is at the end of current output stream
    int VarContentOffset = SOut.Len();
    // update it's location in the variable-index
    SetLocationVar(RecMem, FieldSerialDesc, VarContentOffset);
    // update value
    if (UseToast) { SOut.PutCh(ToastNo); }
    FltV.Save(SOut);
    // Perform TOAST-ing if needed
    CheckToast(SOut, VarContentOffset);
}

void TRecSerializator::SetFieldNumSpV(TMem& RecMem, TMOut& SOut,
        const TFieldSerialDesc& FieldSerialDesc, const TIntFltKdV& SpV) {

    // location of the new variable-length value is at the end of current output stream
    int VarContentOffset = SOut.Len();
    // update it's location in the variable-index
    SetLocationVar(RecMem, FieldSerialDesc, VarContentOffset);
    // update value
    if (UseToast) { SOut.PutCh(ToastNo); }
    SpV.Save(SOut);
    // Perform TOAST-ing if needed
    CheckToast(SOut, VarContentOffset);
}

void TRecSerializator::SetFieldBowSpV(TMem& RecMem, TMOut& SOut,
        const TFieldSerialDesc& FieldSerialDesc, const PBowSpV& SpV) {

    // location of the new variable-length value is at the end of current output stream
    int VarContentOffset = SOut.Len();
    // update it's location in the variable-index
    SetLocationVar(RecMem, FieldSerialDesc, VarContentOffset);
    // update value
    if (UseToast) { SOut.PutCh(ToastNo); }
    SpV->Save(SOut);
    // Perform TOAST-ing if needed
    CheckToast(SOut, VarContentOffset);
}
void TRecSerializator::SetFieldTMem(TMem& RecMem, TMOut& SOut,
    const TFieldSerialDesc& FieldSerialDesc, const TMem& Mem) {

    // location of the new variable-length value is at the end of current output stream
    int VarContentOffset = SOut.Len();
    // update it's location in the variable-index
    SetLocationVar(RecMem, FieldSerialDesc, VarContentOffset);
    // update value
    if (UseToast) { SOut.PutCh(ToastNo); }
    Mem.Save(SOut);
    // Perform TOAST-ing if needed
    CheckToast(SOut, VarContentOffset);
}
void TRecSerializator::SetFieldJsonVal(TMem& RecMem, TMOut& SOut,
    const TFieldSerialDesc& FieldSerialDesc, const PJsonVal& Json) {

    // location of the new variable-length value is at the end of current output stream
    int VarContentOffset = SOut.Len();
    // update it's location in the variable-index
    SetLocationVar(RecMem, FieldSerialDesc, VarContentOffset);
    // update value
    if (UseToast) { SOut.PutCh(ToastNo); }
    TJsonVal::GetStrFromVal(Json).Save(SOut);
    // Perform TOAST-ing if needed
    CheckToast(SOut, VarContentOffset);
}

void TRecSerializator::SetVarJsonVal(TMem& RecMem, TMOut& SOut,
        const TFieldSerialDesc& FieldSerialDesc, const TFieldDesc& FieldDesc,
        const PJsonVal& JsonVal) {

    // call type-appropriate setter
    switch (FieldDesc.GetFieldType()) {
        case oftIntV: {
            QmAssertR(JsonVal->IsArr(), "Provided JSon data field " + FieldDesc.GetFieldNm() + " is not array.");
            TIntV IntV; JsonVal->GetArrIntV(IntV);
            SetFieldIntV(RecMem, SOut, FieldSerialDesc, IntV);
            break;
        }
        case oftStr: {
            QmAssertR(JsonVal->IsStr(), "Provided JSon data field " + FieldDesc.GetFieldNm() + " is not string.");
            TStr Str = JsonVal->GetStr();
            SetFieldStr(RecMem, SOut, FieldSerialDesc, Str);
            break;
        }
        case oftStrV: {
            QmAssertR(JsonVal->IsArr(), "Provided JSon data field " + FieldDesc.GetFieldNm() + " is not array.");
            TStrV StrV; JsonVal->GetArrStrV(StrV);
            SetFieldStrV(RecMem, SOut, FieldSerialDesc, StrV);
            break;
        }
        case oftFltV: {
            QmAssertR(JsonVal->IsArr(), "Provided JSon data field " + FieldDesc.GetFieldNm() + " is not array.");
            TFltV FltV; JsonVal->GetArrNumV(FltV);
            SetFieldFltV(RecMem, SOut, FieldSerialDesc, FltV);
            break;
        }
        case oftBowSpV:
            throw TQmExcept::New("Parsing of BowSpV from JSon not yet implemented");
        case oftNumSpV: {
            QmAssertR(JsonVal->IsArr(), "Provided JSon data field " + FieldDesc.GetFieldNm() + " is not array.");
            TIntFltKdV NumSpV; JsonVal->GetArrNumSpV(NumSpV);
            SetFieldNumSpV(RecMem, SOut, FieldSerialDesc, NumSpV);
            break;
        }
        case oftTMem: {
            QmAssertR(JsonVal->IsStr(), "Provided JSon data field " + FieldDesc.GetFieldNm() + " is not string.");
            TMem Mem;
            TStr::Base64Decode(JsonVal->GetStr(), Mem);
            SetFieldTMem(RecMem, SOut, FieldSerialDesc, Mem);
            break;
        }
        case oftJson: {
            SetFieldJsonVal(RecMem, SOut, FieldSerialDesc, JsonVal);
            break;
        }
        default:
            throw TQmExcept::New("Unsupported JSon data type for DB storage (variable part) - " + FieldDesc.GetFieldTypeStr());
    }
}

void TRecSerializator::CopyFieldVar(const TMemBase& InRecMem, TMem& FixedMem,
        TMOut& VarSOut, const TFieldSerialDesc& FieldSerialDesc) {

    // make sure we are copying variable field
    QmAssert(!FieldSerialDesc.FixedPartP);
    // set new offset location
    int VarContentOffset = VarSOut.Len();
    SetLocationVar(FixedMem, FieldSerialDesc, VarContentOffset);
    // just copy other variable fields
    int OldVarLength = GetVarPartBfLen(InRecMem, FieldSerialDesc);
    if (OldVarLength > 0) {
        // get location of old variable
        char* OldVarBf = GetLocationVar(InRecMem, FieldSerialDesc);
        // move it to output stream for new serialization
        VarSOut.AppendBf(OldVarBf, OldVarLength);
    }
}

void TRecSerializator::ExtractFixedMem(const TMemBase& InRecMem, TMem& FixedMem) {
    // Reserve fixed space - null map, fixed fields and var-field indexes
    FixedMem.Reserve(VarContentPartOffset);
    // copy fixed part
    Assert(FixedMem.Len() <= InRecMem.Len());
    FixedMem.AddBf(InRecMem.GetBf(), VarContentPartOffset);
}

void TRecSerializator::Merge(const TMem& FixedMem, const TMOut& VarSOut, TMem& OutRecMem) {
    OutRecMem.Reserve(VarContentPartOffset + VarSOut.Len());
    OutRecMem.AddBf(FixedMem.GetBf(), VarContentPartOffset);
    OutRecMem.AddBf(VarSOut.GetBfAddr(), VarSOut.Len());
}

TRecSerializator::TRecSerializator(const TWPt<TStore>& Store, const TWPt<TToaster>& _Toaster,
        const TStoreSchema& StoreSchema, const TStoreLoc& _TargetStorage) {

    // collect the store's field table and initialize from it
    TFieldDescV FieldDescV;
    for (int FieldId = 0; FieldId < Store->GetFields(); FieldId++) {
        FieldDescV.Add(Store->GetFieldDesc(FieldId));
    }
    InitFromFields(_Toaster, FieldDescV, StoreSchema, _TargetStorage);
}

TRecSerializator::TRecSerializator(const TWPt<TToaster>& _Toaster, const TFieldDescV& FieldDescV,
        const TStoreSchema& StoreSchema, const TStoreLoc& _TargetStorage) {
    InitFromFields(_Toaster, FieldDescV, StoreSchema, _TargetStorage);
}

void TRecSerializator::InitFromFields(const TWPt<TToaster>& _Toaster, const TFieldDescV& FieldDescV,
        const TStoreSchema& StoreSchema, const TStoreLoc& _TargetStorage) {

    TargetStorage = _TargetStorage;
    // initialize toaster
    Toaster = _Toaster;
    UseToast = Toaster->CanToast();
    if (UseToast) {
        MxToastLen = Toaster->GetMaxToastLen();
    }

    // initialize offsets
    const int Fields = FieldDescV.Len();
    // fixed part starts after null-flags
    FixedPartOffset = (int)ceil((float)Fields / 8);
    // variable part starts same place before any fixed-width fields identified
    VarIndexPartOffset = FixedPartOffset;

    // maintaining current offsets and counts
    int FixedIndexOffset = 0;
    int VarIndexOffset = 0;
    int VarFieldCount = 0;

    for (int FieldId = 0; FieldId < Fields; FieldId++) {
        // get field description
        const TFieldDesc& FieldDesc = FieldDescV[FieldId];
        QmAssert(FieldDesc.GetFieldId() == FieldId);
        // get field name
        const TStr& FieldName = FieldDesc.GetFieldNm();
        // get extended field description from schema
        QmAssertR(StoreSchema.FieldExH.IsKey(FieldName), "[TRecSerializator] field " +
            FieldName + " is missing from the schema");
        const TFieldDescEx& FieldDescEx = StoreSchema.FieldExH.GetDat(FieldName);
        // skip field if it does not match targeted storage
        if (FieldDescEx.FieldStoreLoc != TargetStorage) { continue; }
        // check if field is fixed-width and if yes, what is its width
        int FixedSize = 0; bool FixedP = true;
        switch (FieldDesc.GetFieldType()) {
            case oftByte: FixedSize = sizeof(uchar); break;
            case oftInt: FixedSize = sizeof(int); break;
            case oftInt16: FixedSize = sizeof(int16); break;
            case oftInt64: FixedSize = sizeof(int64); break;
            case oftIntV: FixedP = false; break;
            case oftUInt: FixedSize = sizeof(uint); break;
            case oftUInt16: FixedSize = sizeof(uint16); break;
            case oftUInt64: FixedSize = sizeof(uint64); break;
            case oftStr: FixedP = FieldDescEx.CodebookP; if (FixedP) { FixedSize = sizeof(int); } break;
            case oftStrV: FixedP = false; break;
            case oftBool: FixedSize = sizeof(bool); break;
            case oftFlt: FixedSize = sizeof(double); break;
            case oftSFlt: FixedSize = sizeof(float); break;
            case oftFltPr: FixedSize = sizeof(double) * 2; break;
            case oftFltV: FixedP = false; break;
            case oftTm: FixedSize = sizeof(uint64); break;
            case oftNumSpV: FixedP = false; break;
            case oftBowSpV: FixedP = false; break;
            case oftTMem: FixedP = false; break;
            case oftJson: FixedP = false; break;
            default: throw TQmExcept::New("Unknown field type " + FieldDesc.GetFieldTypeStr());
        }
        // move variable offset for the fixed size of current field
        VarIndexPartOffset += FixedSize;
        // prepare field serialization description
        TFieldSerialDesc FieldSerialDesc;
        FieldSerialDesc.FieldId = FieldDesc.GetFieldId();
        FieldSerialDesc.StoreLoc = FieldDescEx.FieldStoreLoc;
        FieldSerialDesc.NullMapByte = FieldId / 8;
        FieldSerialDesc.NullMapMask = 1u << (FieldId % 8);
        FieldSerialDesc.FixedPartP = FixedP;
        FieldSerialDesc.Offset = (FixedP ? FixedIndexOffset : VarIndexOffset);
        FieldSerialDesc.CodebookP = FieldDescEx.CodebookP;
        FieldSerialDesc.SmallStringP = FieldDescEx.SmallStringP;
        FieldSerialDesc.DefaultVal = FieldDescEx.DefaultVal;
        // remember serialization description
        int FieldSerialDescId = FieldSerialDescV.Add(FieldSerialDesc);
        // remember mapping from field id to serialization description id
        FieldIdToSerialDescIdH.AddDat(FieldSerialDesc.FieldId, FieldSerialDescId);
        // accordingly update fixed or variable-index offsets
        if (FixedP) {
            FixedIndexOffset += FixedSize;
        } else {
            VarIndexOffset += sizeof(int);
            VarFieldCount++;
        }
    }
    // var-index part consists of integers that are offsets for specific field
    VarContentPartOffset = VarIndexPartOffset + VarFieldCount * sizeof(int);
}

void TRecSerializator::Load(TSIn& SIn) {
    TargetStorage = TStoreLoc(TInt(SIn).Val);
    FixedPartOffset.Load(SIn);
    VarIndexPartOffset.Load(SIn);
    VarContentPartOffset.Load(SIn);
    FieldSerialDescV.Load(SIn);
    FieldIdToSerialDescIdH.Load(SIn);
    CodebookH.Load(SIn);
    UseToast.Load(SIn);
    MxToastLen.Load(SIn);
}

void TRecSerializator::Save(TSOut& SOut) {
    TInt(TargetStorage).Save(SOut);
    FixedPartOffset.Save(SOut);
    VarIndexPartOffset.Save(SOut);
    VarContentPartOffset.Save(SOut);
    FieldSerialDescV.Save(SOut);
    FieldIdToSerialDescIdH.Save(SOut);
    CodebookH.Save(SOut);
    UseToast.Save(SOut);
    MxToastLen.Save(SOut);
}

void TRecSerializator::Serialize(const PJsonVal& RecVal, TMem& RecMem, const TWPt<TStore>& Store) {
    // Reserve fixed space - null map, fixed fields and var-field indexes
    TMem FixedMem(VarContentPartOffset);
    // Overwrite fixed part with zeros to start with
    FixedMem.GenZeros(VarContentPartOffset);
    // Prepare output stream for storing variable width values
    TMOut VarSOut;

    // iterate over fields and serialize them
    for (int FieldSerialDescId = 0; FieldSerialDescId < FieldSerialDescV.Len(); FieldSerialDescId++) {
        const TFieldSerialDesc& FieldSerialDesc = FieldSerialDescV[FieldSerialDescId];
        // get field description
        const TFieldDesc& FieldDesc = Store->GetFieldDesc(FieldSerialDesc.FieldId);
        TStr FieldName = FieldDesc.GetFieldNm();
        // parse field value from provided JSon
        PJsonVal FieldVal;
        // figure out value when not provided directly
        if (!RecVal->IsObjKey(FieldName)){
            // check if the field is a surrogate for a field join
            if (!FieldSerialDesc.DefaultVal.Empty()) {
                // use the provided default value
                FieldVal = FieldSerialDesc.DefaultVal;
            } else if (FieldDesc.IsNullable()) {
                // value not provided and object is nullable, so we set it to NULL
                SetFieldNull(FixedMem, FieldSerialDesc, true);
                // update variable-length index to point to the end of stream
                if (!FieldSerialDesc.FixedPartP) {
                    SetLocationVar(FixedMem, FieldSerialDesc, VarSOut.Len());
                }
                // we are done with this field
                continue;
            } else {
                // report missing field value since no other option available
                throw TQmExcept::New("JSon data is missing field - expecting " + FieldName + ", store " + Store->GetStoreNm());
            }
        }
        // load field value when default not already loaded
        if (FieldVal.Empty()) {
            FieldVal = RecVal->GetObjKey(FieldName);
        }
        // set the field as specified
        if (FieldVal->IsNull()) {
            // we are setting field explicitly to null
            QmAssertR(FieldDesc.IsNullable(), "Non-nullable field " + FieldName + " set to null");
            SetFieldNull(FixedMem, FieldSerialDesc, true);
            // if not from fixed part, point variable-length index to the end of stream
            if (!FieldSerialDesc.FixedPartP) {
                SetLocationVar(FixedMem, FieldSerialDesc, VarSOut.Len());
            }
        } else if (FieldSerialDesc.FixedPartP) {
            SetFixedJsonVal(FixedMem, FieldSerialDesc, FieldDesc, FieldVal);
        } else {
            SetVarJsonVal(FixedMem, VarSOut, FieldSerialDesc, FieldDesc, FieldVal);
        }
    }

    // merge fixed and variable parts for final result
    Merge(FixedMem, VarSOut, RecMem);
}

void TRecSerializator::SerializeUpdateInPlace(const PJsonVal& RecVal,
    TThinMIn MIn, const TWPt<TStore>& Store, TIntSet& ChangedFieldIdSet) {

    // iterate over fields and serialize them
    for (int FieldSerialDescId = 0; FieldSerialDescId < FieldSerialDescV.Len(); FieldSerialDescId++) {
        const TFieldSerialDesc& FieldSerialDesc = FieldSerialDescV[FieldSerialDescId];
        // get field description
        const TFieldDesc& FieldDesc = Store->GetFieldDesc(FieldSerialDesc.FieldId);
        TStr FieldName = FieldDesc.GetFieldNm();
        if (RecVal->IsObjKey(FieldName)) {
            // new value, must update
            int BfL = MIn.Len();
            char* Bf = MIn.GetBfAddrChar();
            PJsonVal JsonVal = RecVal->GetObjKey(FieldName);
            if (JsonVal->IsNull()) {
                // we are setting field explicitly to null
                QmAssertR(FieldDesc.IsNullable(), "Non-nullable field " + FieldName + " set to null");
                SetFieldNull(Bf, BfL, FieldSerialDesc.FieldId, true);
            } else {
                // remove null flag
                SetFieldNull(Bf, BfL, FieldSerialDesc.FieldId, true);
                // serialize the field
                QmAssert(FieldSerialDesc.FixedPartP);
                SetFixedJsonVal((char*)Bf, BfL, FieldSerialDesc, FieldDesc, JsonVal);
            }
            // remember for reporting back that we updated the field
            ChangedFieldIdSet.AddKey(FieldDesc.GetFieldId());
        }
    }
}

void TRecSerializator::SerializeUpdate(const PJsonVal& RecVal, const TMemBase& InRecMem,
        TMem& OutRecMem, const TWPt<TStore>& Store, TIntSet& ChangedFieldIdSet) {

    // split to fixed and variable parts
    TMem FixedMem; TMOut VarSOut; ExtractFixedMem(InRecMem, FixedMem);

    // iterate over fields and serialize them
    for (int FieldSerialDescId = 0; FieldSerialDescId < FieldSerialDescV.Len(); FieldSerialDescId++) {
        const TFieldSerialDesc& FieldSerialDesc = FieldSerialDescV[FieldSerialDescId];
        // get field description
        const TFieldDesc& FieldDesc = Store->GetFieldDesc(FieldSerialDesc.FieldId);
        TStr FieldName = FieldDesc.GetFieldNm();
        // figure out value when not provided directly
        if (!RecVal->IsObjKey(FieldName)){
            // copy the variable field when no update to it is provided
            // fixed length variables are already copied on start
            if (!FieldSerialDesc.FixedPartP) {
                CopyFieldVar(InRecMem, FixedMem, VarSOut, FieldSerialDesc);
            }
        } else {
            // new value, must update
            PJsonVal JsonVal = RecVal->GetObjKey(FieldName);
            if (JsonVal->IsNull()) {
                // we are setting field explicitly to null
                QmAssertR(FieldDesc.IsNullable(), "Non-nullable field " + FieldName + " set to null");
                SetFieldNull(FixedMem, FieldSerialDesc, true);
                // if not from fixed part, point variable-length index to the end of stream
                if (!FieldSerialDesc.FixedPartP) {
                    SetLocationVar(FixedMem, FieldSerialDesc, VarSOut.Len());
                }
            } else {
                // remove null flag
                SetFieldNull(FixedMem, FieldSerialDesc, false);
                // serialize the field
                if (FieldSerialDesc.FixedPartP) {
                    SetFixedJsonVal(FixedMem, FieldSerialDesc, FieldDesc, JsonVal);
                } else {
                    SetVarJsonVal(FixedMem, VarSOut, FieldSerialDesc, FieldDesc, JsonVal);
                }
            }
            // remember for reporting back that we updated the field
            ChangedFieldIdSet.AddKey(FieldDesc.GetFieldId());
        }
    }

    // merge fixed and variable parts for final result
    Merge(FixedMem, VarSOut, OutRecMem);
}

void TRecSerializator::SerializeCopyRec(const TWPt<TStore>& Store,
        const TRecSerializator& SrcSerMem, const TRecSerializator& SrcSerCache,
        const TMemBase& SrcMemRec, const TMemBase& SrcCacheRec, TMem& RecMem,
        const TIntV& NewToOldFieldIdV) {

    // Reserve fixed space - null map, fixed fields and var-field indexes
    TMem FixedMem(VarContentPartOffset);
    // Overwrite fixed part with zeros to start with
    FixedMem.GenZeros(VarContentPartOffset);
    // Prepare output stream for storing variable width values
    TMOut VarSOut;

    // iterate over fields and copy them from the section that currently holds them
    for (int FieldSerialDescId = 0; FieldSerialDescId < FieldSerialDescV.Len(); FieldSerialDescId++) {
        const TFieldSerialDesc& FieldSerialDesc = FieldSerialDescV[FieldSerialDescId];
        // the source serializators (and Store) may use different field ids when
        // the migration renumbered them; all reads below use the source id
        const int FieldId = NewToOldFieldIdV.Empty() ?
            FieldSerialDesc.FieldId.Val : NewToOldFieldIdV[FieldSerialDesc.FieldId].Val;
        const TFieldDesc& FieldDesc = Store->GetFieldDesc(FieldId);
        // locate the serializator (and matching record section) the field was written with
        const bool FromMemP = SrcSerMem.IsFieldId(FieldId);
        QmAssertR(FromMemP || SrcSerCache.IsFieldId(FieldId), "[SerializeCopyRec] field " +
            FieldDesc.GetFieldNm() + " of store " + Store->GetStoreNm() + " is missing from both source serializators");
        const TRecSerializator& SrcSer = FromMemP ? SrcSerMem : SrcSerCache;
        const TMemBase& SrcRec = FromMemP ? SrcMemRec : SrcCacheRec;

        // copy the null flag
        if (SrcSer.IsFieldNull(SrcRec, FieldId)) {
            QmAssertR(FieldDesc.IsNullable(), "[SerializeCopyRec] non-nullable field " +
                FieldDesc.GetFieldNm() + " of store " + Store->GetStoreNm() + " is null in the source record");
            SetFieldNull(FixedMem, FieldSerialDesc, true);
            // update variable-length index to point to the end of stream
            if (!FieldSerialDesc.FixedPartP) {
                SetLocationVar(FixedMem, FieldSerialDesc, VarSOut.Len());
            }
            continue;
        }
        // copy the value through the type-appropriate getter/setter pair; the
        // getters transparently un-TOAST source values, the setters re-TOAST
        // values that do not fit inline
        if (FieldSerialDesc.FixedPartP) {
            switch (FieldDesc.GetFieldType()) {
                case oftByte: SetFieldByte(FixedMem, FieldSerialDesc, SrcSer.GetFieldByte(SrcRec, FieldId)); break;
                case oftInt: SetFieldInt(FixedMem, FieldSerialDesc, SrcSer.GetFieldInt(SrcRec, FieldId)); break;
                case oftInt16: SetFieldInt16(FixedMem, FieldSerialDesc, SrcSer.GetFieldInt16(SrcRec, FieldId)); break;
                case oftInt64: SetFieldInt64(FixedMem, FieldSerialDesc, SrcSer.GetFieldInt64(SrcRec, FieldId)); break;
                case oftUInt: SetFieldUInt(FixedMem, FieldSerialDesc, SrcSer.GetFieldUInt(SrcRec, FieldId)); break;
                case oftUInt16: SetFieldUInt16(FixedMem, FieldSerialDesc, SrcSer.GetFieldUInt16(SrcRec, FieldId)); break;
                case oftUInt64: SetFieldUInt64(FixedMem, FieldSerialDesc, SrcSer.GetFieldUInt64(SrcRec, FieldId)); break;
                case oftStr: SetFieldStr(FixedMem, FieldSerialDesc, SrcSer.GetFieldStr(SrcRec, FieldId)); break;
                case oftBool: SetFieldBool(FixedMem, FieldSerialDesc, SrcSer.GetFieldBool(SrcRec, FieldId)); break;
                case oftFlt: SetFieldFlt(FixedMem, FieldSerialDesc, SrcSer.GetFieldFlt(SrcRec, FieldId)); break;
                case oftSFlt: SetFieldSFlt(FixedMem, FieldSerialDesc, SrcSer.GetFieldSFlt(SrcRec, FieldId)); break;
                case oftFltPr: SetFieldFltPr(FixedMem, FieldSerialDesc, SrcSer.GetFieldFltPr(SrcRec, FieldId)); break;
                case oftTm: SetFieldTmMSecs(FixedMem, FieldSerialDesc, SrcSer.GetFieldTmMSecs(SrcRec, FieldId)); break;
                default: throw TQmExcept::New("[SerializeCopyRec] unsupported fixed-part field type " +
                    FieldDesc.GetFieldTypeStr() + " for field " + FieldDesc.GetFieldNm());
            }
        } else {
            switch (FieldDesc.GetFieldType()) {
                case oftIntV: {
                    TIntV IntV; SrcSer.GetFieldIntV(SrcRec, FieldId, IntV);
                    SetFieldIntV(FixedMem, VarSOut, FieldSerialDesc, IntV); break;
                }
                case oftStr: SetFieldStr(FixedMem, VarSOut, FieldSerialDesc, SrcSer.GetFieldStr(SrcRec, FieldId)); break;
                case oftStrV: {
                    TStrV StrV; SrcSer.GetFieldStrV(SrcRec, FieldId, StrV);
                    SetFieldStrV(FixedMem, VarSOut, FieldSerialDesc, StrV); break;
                }
                case oftFltV: {
                    TFltV FltV; SrcSer.GetFieldFltV(SrcRec, FieldId, FltV);
                    SetFieldFltV(FixedMem, VarSOut, FieldSerialDesc, FltV); break;
                }
                case oftNumSpV: {
                    TIntFltKdV SpV; SrcSer.GetFieldNumSpV(SrcRec, FieldId, SpV);
                    SetFieldNumSpV(FixedMem, VarSOut, FieldSerialDesc, SpV); break;
                }
                case oftBowSpV: {
                    PBowSpV SpV; SrcSer.GetFieldBowSpV(SrcRec, FieldId, SpV);
                    SetFieldBowSpV(FixedMem, VarSOut, FieldSerialDesc, SpV); break;
                }
                case oftTMem: {
                    TMem Mem; SrcSer.GetFieldTMem(SrcRec, FieldId, Mem);
                    SetFieldTMem(FixedMem, VarSOut, FieldSerialDesc, Mem); break;
                }
                case oftJson: SetFieldJsonVal(FixedMem, VarSOut, FieldSerialDesc, SrcSer.GetFieldJsonVal(SrcRec, FieldId)); break;
                default: throw TQmExcept::New("[SerializeCopyRec] unsupported variable-part field type " +
                    FieldDesc.GetFieldTypeStr() + " for field " + FieldDesc.GetFieldNm());
            }
        }
    }

    // merge fixed and variable parts for final result
    Merge(FixedMem, VarSOut, RecMem);
}

void TRecSerializator::DeleteToast(const TMemBase& RecMem) {
    if (UseToast) {
        TToastWatcher Watcher(this); // to delay deletion of old TOASTS
        for (int FieldSerialDescId = 0; FieldSerialDescId < FieldSerialDescV.Len(); FieldSerialDescId++) {
            const TFieldSerialDesc& FieldSerialDesc = FieldSerialDescV[FieldSerialDescId];

            CheckToastDel(RecMem, FieldSerialDesc);
        }
    }
}

bool TRecSerializator::IsFieldNull(TThinMIn& min, const int& FieldId) const {
    const TFieldSerialDesc& FieldSerialDesc = GetFieldSerialDesc(FieldId);
    const char* bf = min.GetBfAddrChar() + FieldSerialDesc.NullMapByte;
    return ((*bf & FieldSerialDesc.NullMapMask) != 0);
}

uchar TRecSerializator::GetFieldByte(TThinMIn& min, const int& FieldId) const {
    const char* bf = GetLocationFixed(min, GetFieldSerialDesc(FieldId));
    return *((uchar*)bf);
}

int TRecSerializator::GetFieldInt(TThinMIn& min, const int& FieldId) const {
    const char* bf = GetLocationFixed(min, GetFieldSerialDesc(FieldId));
    return *((int*)bf);
}

int16 TRecSerializator::GetFieldInt16(TThinMIn& min, const int& FieldId) const {
    const char* bf = GetLocationFixed(min, GetFieldSerialDesc(FieldId));
    return *((int16*)bf);
}

int64 TRecSerializator::GetFieldInt64(TThinMIn& min, const int& FieldId) const {
    const char* bf = GetLocationFixed(min, GetFieldSerialDesc(FieldId));
    return TInt64::GetFromBufSafe(bf);
}

void TRecSerializator::GetFieldIntV(TThinMIn& min, const int& FieldId, TIntV& IntV) const {
    min.MoveTo(GetOffsetVar(min, GetFieldSerialDesc(FieldId)));
    if (UseToast && min.GetCh() == ToastYes) {
        TPgBlobPt Pt;
        min.GetBf(&Pt, sizeof(TPgBlobPt));
        TMem Mem;
        Toaster->UnToastVal(Pt, Mem);
        TThinMIn min2(Mem);
        IntV.Load(min2);
    } else {
        IntV.Load(min);
    }
}

uint TRecSerializator::GetFieldUInt(TThinMIn& min, const int& FieldId) const {
    const char* Bf = GetLocationFixed(min, GetFieldSerialDesc(FieldId));
    return *((uint*)Bf);
}

uint16 TRecSerializator::GetFieldUInt16(TThinMIn& min, const int& FieldId) const {
    const char* Bf = GetLocationFixed(min, GetFieldSerialDesc(FieldId));
    return *((uint16*)Bf);
}

uint64 TRecSerializator::GetFieldUInt64(TThinMIn& min, const int& FieldId) const {
    const char* Bf = GetLocationFixed(min, GetFieldSerialDesc(FieldId));
    return TUInt64::GetFromBufSafe(Bf);
}

TStr TRecSerializator::GetFieldStr(TThinMIn& min, const int& FieldId) const {
    const TFieldSerialDesc& FieldSerialDesc = GetFieldSerialDesc(FieldId);
    if (FieldSerialDesc.FixedPartP) {
        // get pointer to location
        char* bf = GetLocationFixed(min, FieldSerialDesc);
        // cast to codebook id value
        int StrId = *((int*)bf);
        // return string from codebook
        return CodebookH.GetKey(StrId);
    } else {
        min.MoveTo(GetOffsetVar(min, GetFieldSerialDesc(FieldId)));
        if (UseToast && min.GetCh() == ToastYes) {
            TPgBlobPt Pt;
            min.GetBf(&Pt, sizeof(TPgBlobPt));
            TMem Mem;
            Toaster->UnToastVal(Pt, Mem);
            TThinMIn min2(Mem);
            TStr Str;
            Str.Load(min2, FieldSerialDesc.SmallStringP);
            return Str;
        } else {
            TStr Str;
            Str.Load(min, FieldSerialDesc.SmallStringP);
            return Str;
        }
    }
}

void TRecSerializator::GetFieldStrV(TThinMIn& min, const int& FieldId, TStrV& StrV) const {
    min.MoveTo(GetOffsetVar(min, GetFieldSerialDesc(FieldId)));
    if (UseToast && min.GetCh() == ToastYes) {
        TPgBlobPt Pt;
        min.GetBf(&Pt, sizeof(TPgBlobPt));
        TMem Mem;
        Toaster->UnToastVal(Pt, Mem);
        TThinMIn min2(Mem);
        StrV.Load(min2);
    } else {
        StrV.Load(min);
    }
}

bool TRecSerializator::GetFieldBool(TThinMIn& min, const int& FieldId) const {
    const char* bf = GetLocationFixed(min, GetFieldSerialDesc(FieldId));
    return *((bool*)bf);
}

double TRecSerializator::GetFieldFlt(TThinMIn& min, const int& FieldId) const {
    const char* Bf = GetLocationFixed(min, GetFieldSerialDesc(FieldId));
    return TFlt::GetFromBufSafe(Bf); // do not cast (not portable to ARM)
}

float TRecSerializator::GetFieldSFlt(TThinMIn& min, const int& FieldId) const {
    const char* Bf = GetLocationFixed(min, GetFieldSerialDesc(FieldId));
    return TSFlt::GetFromBufSafe(Bf); // do not cast (not portable to ARM)
}

TFltPr TRecSerializator::GetFieldFltPr(TThinMIn& min, const int& FieldId) const {
    const char* Bf = GetLocationFixed(min, GetFieldSerialDesc(FieldId));
    return TFltPr(TFlt::GetFromBufSafe(Bf), TFlt::GetFromBufSafe(Bf + sizeof(double))); // do not cast (not portable to ARM)
}

void TRecSerializator::GetFieldFltV(TThinMIn& min, const int& FieldId, TFltV& FltV) const {
    min.MoveTo(GetOffsetVar(min, GetFieldSerialDesc(FieldId)));
    if (UseToast && min.GetCh() == ToastYes) {
        TPgBlobPt Pt;
        min.GetBf(&Pt, sizeof(TPgBlobPt));
        TMem Mem;
        Toaster->UnToastVal(Pt, Mem);
        TThinMIn min2(Mem);
        FltV.Load(min2);
    } else {
        FltV.Load(min);
    }
}

void TRecSerializator::GetFieldTm(TThinMIn& min, const int& FieldId, TTm& Tm) const {
    Tm = TTm::GetTmFromMSecs(GetFieldTmMSecs(min, FieldId));
}

uint64 TRecSerializator::GetFieldTmMSecs(TThinMIn& min, const int& FieldId) const {
    const char* bf = GetLocationFixed(min, GetFieldSerialDesc(FieldId));
    return *((uint64*)bf);
}

void TRecSerializator::GetFieldNumSpV(TThinMIn& min, const int& FieldId, TIntFltKdV& SpV) const {
    min.MoveTo(GetOffsetVar(min, GetFieldSerialDesc(FieldId)));
    if (UseToast && min.GetCh() == ToastYes) {
        TPgBlobPt Pt;
        min.GetBf(&Pt, sizeof(TPgBlobPt));
        TMem Mem;
        Toaster->UnToastVal(Pt, Mem);
        TThinMIn min2(Mem);
        SpV.Load(min2);
    } else {
        SpV.Load(min);
    }
}

void TRecSerializator::GetFieldBowSpV(TThinMIn& min, const int& FieldId, PBowSpV& SpV) const {
    min.MoveTo(GetOffsetVar(min, GetFieldSerialDesc(FieldId)));
    if (UseToast && min.GetCh() == ToastYes) {
        TPgBlobPt Pt;
        min.GetBf(&Pt, sizeof(TPgBlobPt));
        TMem Mem;
        Toaster->UnToastVal(Pt, Mem);
        TThinMIn min2(Mem);
        SpV = TBowSpV::Load(min2);
    } else {
        SpV = TBowSpV::Load(min);
    }
}

void TRecSerializator::GetFieldTMem(TThinMIn& min, const int& FieldId, TMem& Mem) const {
    min.MoveTo(GetOffsetVar(min, GetFieldSerialDesc(FieldId)));
    if (UseToast && min.GetCh() == ToastYes) {
        TPgBlobPt Pt;
        min.GetBf(&Pt, sizeof(TPgBlobPt));
        Toaster->UnToastVal(Pt, Mem);
    } else {
        Mem.Load(min);
    }
}

PJsonVal TRecSerializator::GetFieldJsonVal(TThinMIn& min, const int& FieldId) const {
    min.MoveTo(GetOffsetVar(min, GetFieldSerialDesc(FieldId)));
    if (UseToast && min.GetCh() == ToastYes) {
        TPgBlobPt Pt;
        min.GetBf(&Pt, sizeof(TPgBlobPt));
        TMem Mem;
        Toaster->UnToastVal(Pt, Mem);
        TThinMIn min2(Mem);
        TStr Str;
        Str.Load(min2);
        return TJsonVal::GetValFromStr(Str);
    } else {
        TStr Str;
        Str.Load(min);
        return TJsonVal::GetValFromStr(Str);
    }
}

bool TRecSerializator::IsFieldNull(const TMemBase& RecMem, const int& FieldId) const {
    TThinMIn ThinMIn(RecMem);
    return IsFieldNull(ThinMIn, FieldId);
}

uchar TRecSerializator::GetFieldByte(const TMemBase& RecMem, const int& FieldId) const {
    TThinMIn ThinMIn(RecMem);
    return GetFieldByte(ThinMIn, FieldId);
}

int TRecSerializator::GetFieldInt(const TMemBase& RecMem, const int& FieldId) const {
    TThinMIn ThinMIn(RecMem);
    return GetFieldInt(ThinMIn, FieldId);
}

int16 TRecSerializator::GetFieldInt16(const TMemBase& RecMem, const int& FieldId) const {
    TThinMIn ThinMIn(RecMem);
    return GetFieldInt16(ThinMIn, FieldId);
}

int64 TRecSerializator::GetFieldInt64(const TMemBase& RecMem, const int& FieldId) const {
    TThinMIn ThinMIn(RecMem);
    return GetFieldInt64(ThinMIn, FieldId);
}

void TRecSerializator::GetFieldIntV(const TMemBase& RecMem, const int& FieldId, TIntV& IntV) const {
    TThinMIn ThinMIn(RecMem);
    GetFieldIntV(ThinMIn, FieldId, IntV);
}

uint TRecSerializator::GetFieldUInt(const TMemBase& RecMem, const int& FieldId) const {
    TThinMIn ThinMIn(RecMem);
    return GetFieldUInt(ThinMIn, FieldId);
}
uint16 TRecSerializator::GetFieldUInt16(const TMemBase& RecMem, const int& FieldId) const {
    TThinMIn ThinMIn(RecMem);
    return GetFieldUInt16(ThinMIn, FieldId);
}
uint64 TRecSerializator::GetFieldUInt64(const TMemBase& RecMem, const int& FieldId) const {
    TThinMIn ThinMIn(RecMem);
    return GetFieldUInt64(ThinMIn, FieldId);
}

TStr TRecSerializator::GetFieldStr(const TMemBase& RecMem, const int& FieldId) const {
    TThinMIn ThinMIn(RecMem);
    return GetFieldStr(ThinMIn, FieldId);
}

void TRecSerializator::GetFieldStrV(const TMemBase& RecMem, const int& FieldId, TStrV& StrV) const {
    TThinMIn ThinMIn(RecMem);
    GetFieldStrV(ThinMIn, FieldId, StrV);
}

bool TRecSerializator::GetFieldBool(const TMemBase& RecMem, const int& FieldId) const {
    TThinMIn ThinMIn(RecMem);
    return GetFieldBool(ThinMIn, FieldId);
}

double TRecSerializator::GetFieldFlt(const TMemBase& RecMem, const int& FieldId) const {
    TThinMIn ThinMIn(RecMem);
    return GetFieldFlt(ThinMIn, FieldId);
}

float TRecSerializator::GetFieldSFlt(const TMemBase& RecMem, const int& FieldId) const {
    TThinMIn ThinMIn(RecMem);
    return GetFieldSFlt(ThinMIn, FieldId);
}

TFltPr TRecSerializator::GetFieldFltPr(const TMemBase& RecMem, const int& FieldId) const {
    TThinMIn ThinMIn(RecMem);
    return GetFieldFltPr(ThinMIn, FieldId);
}

void TRecSerializator::GetFieldFltV(const TMemBase& RecMem, const int& FieldId, TFltV& FltV) const {
    TThinMIn ThinMIn(RecMem);
    GetFieldFltV(ThinMIn, FieldId, FltV);
}

void TRecSerializator::GetFieldTm(const TMemBase& RecMem, const int& FieldId, TTm& Tm) const {
    TThinMIn ThinMIn(RecMem);
    GetFieldTm(ThinMIn, FieldId, Tm);
}

uint64 TRecSerializator::GetFieldTmMSecs(const TMemBase& RecMem, const int& FieldId) const {
    TThinMIn ThinMIn(RecMem);
    return GetFieldTmMSecs(ThinMIn, FieldId);
}

void TRecSerializator::GetFieldNumSpV(const TMemBase& RecMem, const int& FieldId, TIntFltKdV& SpV) const {
    TThinMIn ThinMIn(RecMem);
    GetFieldNumSpV(ThinMIn, FieldId, SpV);
}

void TRecSerializator::GetFieldBowSpV(const TMemBase& RecMem, const int& FieldId, PBowSpV& SpV) const {
    TThinMIn ThinMIn(RecMem);
    GetFieldBowSpV(ThinMIn, FieldId, SpV);
}

void TRecSerializator::GetFieldTMem(const TMemBase& RecMem, const int& FieldId, TMem& Mem) const {
    TThinMIn ThinMIn(RecMem);
    GetFieldTMem(ThinMIn, FieldId, Mem);
}

PJsonVal TRecSerializator::GetFieldJsonVal(const TMemBase& RecMem, const int& FieldId) const {
    TThinMIn ThinMIn(RecMem);
    return GetFieldJsonVal(ThinMIn, FieldId);
}

void TRecSerializator::SetFieldNull(const TMemBase& InRecMem, TMem& OutRecMem, const int& FieldId) {
    TToastWatcher Watcher(this); // to delay deletion of old TOASTS
    // different handling for fixed and variable fields
    const TFieldSerialDesc& FieldSerialDesc = GetFieldSerialDesc(FieldId);
    if (FieldSerialDesc.FixedPartP) {
        // copy existing serialization
        OutRecMem.Copy(InRecMem);
        // just mark fixed field as null
        SetFieldNull(OutRecMem, FieldSerialDesc, true);
    } else {
        // split to fixed and variable parts
        TMem FixedMem; TMOut VarSOut; ExtractFixedMem(InRecMem, FixedMem);
        // iterate over fields and serialize them
        for (int FieldSerialDescId = 0; FieldSerialDescId < FieldSerialDescV.Len(); FieldSerialDescId++) {
            const TFieldSerialDesc& FieldSerialDesc = FieldSerialDescV[FieldSerialDescId];
            if (FieldSerialDesc.FieldId == FieldId) {
                // check if value is toasted => we need to delete it
                CheckToastDel(InRecMem, FieldSerialDesc);
                // this is the field we are setting to NULL
                SetFieldNull(FixedMem, FieldSerialDesc, true);
                // update variable-length index to point
                QmAssert(!FieldSerialDesc.FixedPartP);
                SetLocationVar(FixedMem, FieldSerialDesc, VarSOut.Len());
            } else if (!FieldSerialDesc.FixedPartP) {
                // just copy other variable fields
                CopyFieldVar(InRecMem, FixedMem, VarSOut, FieldSerialDesc);
            }
        }
        // merge fixed and variable parts for final result
        Merge(FixedMem, VarSOut, OutRecMem);
    }
}

void TRecSerializator::SetFieldByte(const TMemBase& InRecMem,
        TMem& OutRecMem, const int& FieldId, const uchar& Byte) {

    const TFieldSerialDesc& FieldSerialDesc = GetFieldSerialDesc(FieldId);
    // copy existing serialization
    OutRecMem.Copy(InRecMem);
    // update the value
    SetFieldByte(OutRecMem, FieldSerialDesc, Byte);
}
void TRecSerializator::SetFieldInt(const TMemBase& InRecMem,
    TMem& OutRecMem, const int& FieldId, const int& Int) {

    const TFieldSerialDesc& FieldSerialDesc = GetFieldSerialDesc(FieldId);
    // copy existing serialization
    OutRecMem.Copy(InRecMem);
    // update the value
    SetFieldInt(OutRecMem, FieldSerialDesc, Int);
}
void TRecSerializator::SetFieldInt16(const TMemBase& InRecMem,
    TMem& OutRecMem, const int& FieldId, const int16& Int16) {

    const TFieldSerialDesc& FieldSerialDesc = GetFieldSerialDesc(FieldId);
    // copy existing serialization
    OutRecMem.Copy(InRecMem);
    // update the value
    SetFieldInt16(OutRecMem, FieldSerialDesc, Int16);
}
void TRecSerializator::SetFieldInt64(const TMemBase& InRecMem,
    TMem& OutRecMem, const int& FieldId, const int64& Int64) {

    const TFieldSerialDesc& FieldSerialDesc = GetFieldSerialDesc(FieldId);
    // copy existing serialization
    OutRecMem.Copy(InRecMem);
    // update the value
    SetFieldInt64(OutRecMem, FieldSerialDesc, Int64);
}

void TRecSerializator::SetFieldIntV(const TMemBase& InRecMem, TMem& OutRecMem, const int& FieldId, const TIntV& IntV) {
    TToastWatcher Watcher(this); // to delay deletion of old TOASTS
    // split to fixed and variable parts
    TMem FixedMem; TMOut VarSOut; ExtractFixedMem(InRecMem, FixedMem);
    // iterate over fields and serialize them
    for (int FieldSerialDescId = 0; FieldSerialDescId < FieldSerialDescV.Len(); FieldSerialDescId++) {
        const TFieldSerialDesc& FieldSerialDesc = FieldSerialDescV[FieldSerialDescId];
        if (FieldSerialDesc.FieldId == FieldId) {
            // check if value is toasted => we need to delete it
            CheckToastDel(InRecMem, FieldSerialDesc);
            // serialize to record buffer
            SetFieldIntV(FixedMem, VarSOut, FieldSerialDesc, IntV);
        } else if (!FieldSerialDesc.FixedPartP) {
            // just copy other variable fields
            CopyFieldVar(InRecMem, FixedMem, VarSOut, FieldSerialDesc);
        }
    }
    // merge fixed and variable parts for final result
    Merge(FixedMem, VarSOut, OutRecMem);
}

void TRecSerializator::SetFieldUInt(const TMemBase& InRecMem,
        TMem& OutRecMem, const int& FieldId, const uint& UInt) {

    const TFieldSerialDesc& FieldSerialDesc = GetFieldSerialDesc(FieldId);
    // copy existing serialization
    OutRecMem.Copy(InRecMem);
    // update the value
    SetFieldUInt(OutRecMem, FieldSerialDesc, UInt);
}
void TRecSerializator::SetFieldUInt16(const TMemBase& InRecMem,
    TMem& OutRecMem, const int& FieldId, const uint16& UInt16) {

    const TFieldSerialDesc& FieldSerialDesc = GetFieldSerialDesc(FieldId);
    // copy existing serialization
    OutRecMem.Copy(InRecMem);
    // update the value
    SetFieldUInt16(OutRecMem, FieldSerialDesc, UInt16);
}
void TRecSerializator::SetFieldUInt64(const TMemBase& InRecMem,
    TMem& OutRecMem, const int& FieldId, const uint64& UInt64) {

    const TFieldSerialDesc& FieldSerialDesc = GetFieldSerialDesc(FieldId);
    // copy existing serialization
    OutRecMem.Copy(InRecMem);
    // update the value
    SetFieldUInt64(OutRecMem, FieldSerialDesc, UInt64);
}

void TRecSerializator::SetFieldStr(const TMemBase& InRecMem,
        TMem& OutRecMem, const int& FieldId, const TStr& Str) {

    // different handling for codebook strings and normal strings
    const TFieldSerialDesc& FieldSerialDesc = GetFieldSerialDesc(FieldId);
    if (FieldSerialDesc.FixedPartP) {
        // copy existing serialization
        OutRecMem.Copy(InRecMem);
        // update value
        SetFieldStr(OutRecMem, FieldSerialDesc, Str);
    } else {
        TToastWatcher Watcher(this); // to delay deletion of old TOASTS
        // split to fixed and variable parts
        TMem FixedMem; TMOut VarSOut; ExtractFixedMem(InRecMem, FixedMem);
        // iterate over fields and serialize them
        for (int FieldSerialDescId = 0; FieldSerialDescId < FieldSerialDescV.Len(); FieldSerialDescId++) {
            const TFieldSerialDesc& FieldSerialDesc = FieldSerialDescV[FieldSerialDescId];
            if (FieldSerialDesc.FieldId == FieldId) {
                // check if value is toasted => we need to delete it
                CheckToastDel(InRecMem, FieldSerialDesc);
                // remove null flag, just in case
                SetFieldNull(FixedMem, FieldSerialDesc, false);
                // serialize to record buffer
                SetFieldStr(FixedMem, VarSOut, FieldSerialDesc, Str);
            } else if (!FieldSerialDesc.FixedPartP) {
                // just copy other variable fields
                CopyFieldVar(InRecMem, FixedMem, VarSOut, FieldSerialDesc);
            }
        }
        // merge fixed and variable parts for final result
        Merge(FixedMem, VarSOut, OutRecMem);
    }
}

void TRecSerializator::SetFieldStrV(const TMemBase& InRecMem,
        TMem& OutRecMem, const int& FieldId, const TStrV& StrV) {

    TToastWatcher Watcher(this); // to delay deletion of old TOASTS
    // split to fixed and variable parts
    TMem FixedMem; TMOut VarSOut; ExtractFixedMem(InRecMem, FixedMem);
    // iterate over fields and serialize them
    for (int FieldSerialDescId = 0; FieldSerialDescId < FieldSerialDescV.Len(); FieldSerialDescId++) {
        const TFieldSerialDesc& FieldSerialDesc = FieldSerialDescV[FieldSerialDescId];
        if (FieldSerialDesc.FieldId == FieldId) {
            // check if value is toasted => we need to delete it
            CheckToastDel(InRecMem, FieldSerialDesc);
            // remove null flag, just in case
            SetFieldNull(FixedMem, FieldSerialDesc, false);
            // serialize to record buffer
            SetFieldStrV(FixedMem, VarSOut, FieldSerialDesc, StrV);
        } else if (!FieldSerialDesc.FixedPartP) {
            // just copy other variable fields
            CopyFieldVar(InRecMem, FixedMem, VarSOut, FieldSerialDesc);
        }
    }
    // merge fixed and variable parts for final result
    Merge(FixedMem, VarSOut, OutRecMem);
}

void TRecSerializator::SetFieldBool(const TMemBase& InRecMem,
        TMem& OutRecMem, const int& FieldId, const bool& Bool) {

    const TFieldSerialDesc& FieldSerialDesc = GetFieldSerialDesc(FieldId);
    // copy existing serialization
    OutRecMem.Copy(InRecMem);
    // update the value
    SetFieldBool(OutRecMem, FieldSerialDesc, Bool);
}

void TRecSerializator::SetFieldFlt(const TMemBase& InRecMem,
        TMem& OutRecMem, const int& FieldId, const double& Flt) {

    const TFieldSerialDesc& FieldSerialDesc = GetFieldSerialDesc(FieldId);
    // copy existing serialization
    OutRecMem.Copy(InRecMem);
    // update the value
    SetFieldFlt(OutRecMem, FieldSerialDesc, Flt);
}
void TRecSerializator::SetFieldSFlt(const TMemBase& InRecMem,
    TMem& OutRecMem, const int& FieldId, const float& Flt) {

    const TFieldSerialDesc& FieldSerialDesc = GetFieldSerialDesc(FieldId);
    // copy existing serialization
    OutRecMem.Copy(InRecMem);
    // update the value
    SetFieldSFlt(OutRecMem, FieldSerialDesc, Flt);
}

void TRecSerializator::SetFieldFltPr(const TMemBase& InRecMem,
        TMem& OutRecMem, const int& FieldId, const TFltPr& FltPr) {

    const TFieldSerialDesc& FieldSerialDesc = GetFieldSerialDesc(FieldId);
    // copy existing serialization
    OutRecMem.Copy(InRecMem);
    // update the value
    SetFieldFltPr(OutRecMem, FieldSerialDesc, FltPr);
}

void TRecSerializator::SetFieldFltV(const TMemBase& InRecMem,
        TMem& OutRecMem, const int& FieldId, const TFltV& FltV) {

    TToastWatcher Watcher(this); // to delay deletion of old TOASTS
    // split to fixed and variable parts
    TMem FixedMem; TMOut VarSOut; ExtractFixedMem(InRecMem, FixedMem);
    // iterate over fields and serialize them
    for (int FieldSerialDescId = 0; FieldSerialDescId < FieldSerialDescV.Len(); FieldSerialDescId++) {
        const TFieldSerialDesc& FieldSerialDesc = FieldSerialDescV[FieldSerialDescId];
        if (FieldSerialDesc.FieldId == FieldId) {
            // check if value is toasted => we need to delete it
            CheckToastDel(InRecMem, FieldSerialDesc);
            // remove null flag, just in case
            SetFieldNull(FixedMem, FieldSerialDesc, false);
            // serialize to record buffer
            SetFieldFltV(FixedMem, VarSOut, FieldSerialDesc, FltV);
        } else if (!FieldSerialDesc.FixedPartP) {
            // just copy other variable fields
            CopyFieldVar(InRecMem, FixedMem, VarSOut, FieldSerialDesc);
        }
    }
    // merge fixed and variable parts for final result
    Merge(FixedMem, VarSOut, OutRecMem);
}

void TRecSerializator::SetFieldTm(const TMemBase& InRecMem,
        TMem& OutRecMem, const int& FieldId, const TTm& Tm) {

    const TFieldSerialDesc& FieldSerialDesc = GetFieldSerialDesc(FieldId);
    // copy existing serialization
    OutRecMem.Copy(InRecMem);
    // update the value
    SetFieldTm(OutRecMem, FieldSerialDesc, Tm);
}

void TRecSerializator::SetFieldTmMSecs(const TMemBase& InRecMem,
        TMem& OutRecMem, const int& FieldId, const uint64& TmMSecs) {

    const TFieldSerialDesc& FieldSerialDesc = GetFieldSerialDesc(FieldId);
    // copy existing serialization
    OutRecMem.Copy(InRecMem);
    // update the value
    SetFieldTmMSecs(OutRecMem, FieldSerialDesc, TmMSecs);
}

void TRecSerializator::SetFieldNumSpV(const TMemBase& InRecMem,
        TMem& OutRecMem, const int& FieldId, const TIntFltKdV& SpV) {

    TToastWatcher Watcher(this); // to delay deletion of old TOASTS
    // split to fixed and variable parts
    TMem FixedMem; TMOut VarSOut; ExtractFixedMem(InRecMem, FixedMem);
    // iterate over fields and serialize them
    for (int FieldSerialDescId = 0; FieldSerialDescId < FieldSerialDescV.Len(); FieldSerialDescId++) {
        const TFieldSerialDesc& FieldSerialDesc = FieldSerialDescV[FieldSerialDescId];
        if (FieldSerialDesc.FieldId == FieldId) {
            // check if value is toasted => we need to delete it
            CheckToastDel(InRecMem, FieldSerialDesc);
            // remove null flag, just in case
            SetFieldNull(FixedMem, FieldSerialDesc, false);
            // serialize to record buffer
            SetFieldNumSpV(FixedMem, VarSOut, FieldSerialDesc, SpV);
        } else if (!FieldSerialDesc.FixedPartP) {
            // just copy other variable fields
            CopyFieldVar(InRecMem, FixedMem, VarSOut, FieldSerialDesc);
        }
    }
    // merge fixed and variable parts for final result
    Merge(FixedMem, VarSOut, OutRecMem);
}

void TRecSerializator::SetFieldBowSpV(const TMemBase& InRecMem,
        TMem& OutRecMem, const int& FieldId, const PBowSpV& SpV) {

    TToastWatcher Watcher(this); // to delay deletion of old TOASTS
    // split to fixed and variable parts
    TMem FixedMem; TMOut VarSOut; ExtractFixedMem(InRecMem, FixedMem);
    // iterate over fields and serialize them
    for (int FieldSerialDescId = 0; FieldSerialDescId < FieldSerialDescV.Len(); FieldSerialDescId++) {
        const TFieldSerialDesc& FieldSerialDesc = FieldSerialDescV[FieldSerialDescId];
        if (FieldSerialDesc.FieldId == FieldId) {
            // check if value is toasted => we need to delete it
            CheckToastDel(InRecMem, FieldSerialDesc);
            // remove null flag, just in case
            SetFieldNull(FixedMem, FieldSerialDesc, false);
            // serialize to record buffer
            SetFieldBowSpV(FixedMem, VarSOut, FieldSerialDesc, SpV);
        } else if (!FieldSerialDesc.FixedPartP) {
            // just copy other variable fields
            CopyFieldVar(InRecMem, FixedMem, VarSOut, FieldSerialDesc);
        }
    }
    // merge fixed and variable parts for final result
    Merge(FixedMem, VarSOut, OutRecMem);
}

void TRecSerializator::SetFieldTMem(const TMemBase& InRecMem,
    TMem& OutRecMem, const int& FieldId, const TMem& Mem) {

    TToastWatcher Watcher(this); // to delay deletion of old TOASTS
    // split to fixed and variable parts
    TMem FixedMem; TMOut VarSOut; ExtractFixedMem(InRecMem, FixedMem);
    // iterate over fields and serialize them
    for (int FieldSerialDescId = 0; FieldSerialDescId < FieldSerialDescV.Len(); FieldSerialDescId++) {
        const TFieldSerialDesc& FieldSerialDesc = FieldSerialDescV[FieldSerialDescId];
        if (FieldSerialDesc.FieldId == FieldId) {
            // check if value is toasted => we need to delete it
            CheckToastDel(InRecMem, FieldSerialDesc);
            // remove null flag, just in case
            SetFieldNull(FixedMem, FieldSerialDesc, false);
            // serialize to record buffer
            SetFieldTMem(FixedMem, VarSOut, FieldSerialDesc, Mem);
        } else if (!FieldSerialDesc.FixedPartP) {
            // just copy other variable fields
            CopyFieldVar(InRecMem, FixedMem, VarSOut, FieldSerialDesc);
        }
    }
    // merge fixed and variable parts for final result
    Merge(FixedMem, VarSOut, OutRecMem);
}

void TRecSerializator::SetFieldJsonVal(const TMemBase& InRecMem,
    TMem& OutRecMem, const int& FieldId, const PJsonVal& Json) {

    TToastWatcher Watcher(this); // to delay deletion of old TOASTS
    // split to fixed and variable parts
    TMem FixedMem; TMOut VarSOut; ExtractFixedMem(InRecMem, FixedMem);
    // iterate over fields and serialize them
    for (int FieldSerialDescId = 0; FieldSerialDescId < FieldSerialDescV.Len(); FieldSerialDescId++) {
        const TFieldSerialDesc& FieldSerialDesc = FieldSerialDescV[FieldSerialDescId];
        if (FieldSerialDesc.FieldId == FieldId) {
            // check if value is toasted => we need to delete it
            CheckToastDel(InRecMem, FieldSerialDesc);
            // remove null flag, just in case
            SetFieldNull(FixedMem, FieldSerialDesc, false);
            // serialize to record buffer
            SetFieldJsonVal(FixedMem, VarSOut, FieldSerialDesc, Json);
        } else if (!FieldSerialDesc.FixedPartP) {
            // just copy other variable fields
            CopyFieldVar(InRecMem, FixedMem, VarSOut, FieldSerialDesc);
        }
    }
    // merge fixed and variable parts for final result
    Merge(FixedMem, VarSOut, OutRecMem);
}

int TRecSerializator::GetCodebookId(const int& FieldId, const TStr& Str) const {
    const TFieldSerialDesc& FieldSerialDesc = GetFieldSerialDesc(FieldId);
    // make sure we are in the codebook park
    QmAssertR(FieldSerialDesc.FixedPartP, TStr::Fmt("[TRecSerializator::GetCodebookId]: Field %d not in codebook", FieldId));
    // return string from codebook
    return CodebookH.GetKeyId(Str);
}

/// verify that given record is properly serialized
void TRecSerializator::Verify(char* Bf, const int& BfL) const {
    TVec<int> VarFields;
    for (int i = 0; i < FieldSerialDescV.Len(); i++) {
        const TFieldSerialDesc& FieldSerialDesc = FieldSerialDescV[i];
        if (FieldSerialDesc.FixedPartP) {
            // nothing to do
        } else {
            const char* Bf2 = Bf + FieldSerialDesc.NullMapByte;
            if ((*Bf2 & FieldSerialDesc.NullMapMask) == 0) {
                // field is not null
                VarFields.Add(i);
                int FieldContentOffset = *((int*)(Bf + VarIndexPartOffset + FieldSerialDesc.Offset));
                if (FieldContentOffset < 0) {
                    printf(" FieldSerialDesc.Offset=%d FieldContentOffset=%d\n", FieldSerialDesc.Offset.Val, FieldContentOffset);
                    QmAssertR(false, "Var-len field content offset is too low");
                }
                if (FieldContentOffset + VarContentPartOffset >= BfL) {
                    printf("FieldContentOffset=%d, VarContentPartOffset=%d, BfL=%d \n", FieldContentOffset, VarContentPartOffset.Val, BfL);
                    QmAssertR(false, "Var-len field content offset is too big");
                }
            }
        }
    }
}

///////////////////////////////
/// Field indexer
TStr TRecIndexer::TFieldIndexKey::GetKeyType() const {
    return
        IsValue()    ? "value" :
        IsText()     ? "text" :
        IsTextPos()  ? "text_position" :
        IsLocation() ? "location" :
                       "linear";
}

void TRecIndexer::IndexKey(const TFieldIndexKey& Key, const TMemBase& RecMem,
        const uint64& RecId, TRecSerializator& Serializator, const PJsonVal& RecJson) {

    // check the type of field and value to select indexing procedure
    if (Key.FieldType == oftStr && Key.IsValue()){
        // inverted index over non-tokenized strings
        TStr Str = Serializator.GetFieldStr(RecMem, Key.FieldId);
        Index->IndexValue(Key.KeyId, Str, RecId);
    } else if (Key.FieldType == oftStr && Key.IsText()) {
        // inverted index over tokenized strings
        if (!RecJson.Empty() && RecJson->IsObjKey("$" + Key.FieldNm + "Tokens")) {
            TStrV TokenV; RecJson->GetObjStrV("$" + Key.FieldNm + "Tokens", TokenV);
            Index->IndexText(Key.KeyId, TokenV, RecId);
        }
        else {
            TStr Str = Serializator.GetFieldStr(RecMem, Key.FieldId);
            Index->IndexText(Key.KeyId, Str, RecId);
        }
    } else if (Key.FieldType == oftStr && Key.IsTextPos()) {
        // inverted index over tokenized strings with position information
        // if we already have the tokenized text in the RecJson, then use that
        if (!RecJson.Empty() && RecJson->IsObjKey("$" + Key.FieldNm + "Tokens")) {
            TStrV TokenV; RecJson->GetObjStrV("$" + Key.FieldNm + "Tokens", TokenV);
            Index->IndexTextPos(Key.KeyId, TokenV, RecId);
        }
        // otherwise perform tokenization first
        else {
            TStr Str = Serializator.GetFieldStr(RecMem, Key.FieldId);
            Index->IndexTextPos(Key.KeyId, Str, RecId);
        }
    } else if (Key.FieldType == oftStrV && Key.IsValue()) {
        // inverted index over string array
        TStrV StrV; Serializator.GetFieldStrV(RecMem, Key.FieldId, StrV);
        Index->IndexValue(Key.KeyId, StrV, RecId);
    } else if (Key.FieldType == oftTm && Key.IsValue()) {
        // time indexed as timestamp string
        const uint64 TmMSecs = Serializator.GetFieldTmMSecs(RecMem, Key.FieldId);
        Index->IndexValue(Key.KeyId, TUInt64::GetStr(TmMSecs), RecId);
    } else if (Key.FieldType == oftFltPr && Key.IsLocation()) {
        // index geo-location using geo-index
        TFltPr FltPr = Serializator.GetFieldFltPr(RecMem, Key.FieldId);
        Index->IndexGeo(Key.KeyId, FltPr, RecId);
    } else if (Key.FieldType == oftByte && Key.IsLinear()) {
        // index integer value using btree
        const uchar Byte = Serializator.GetFieldByte(RecMem, Key.FieldId);
        Index->IndexLinear(Key.KeyId, Byte, RecId);
    } else if (Key.FieldType == oftInt && Key.IsLinear()) {
        // index integer value using btree
        const int Int = Serializator.GetFieldInt(RecMem, Key.FieldId);
        Index->IndexLinear(Key.KeyId, Int, RecId);
    } else if (Key.FieldType == oftInt16 && Key.IsLinear()) {
        // index integer value using btree
        const int16 Int = Serializator.GetFieldInt16(RecMem, Key.FieldId);
        Index->IndexLinear(Key.KeyId, Int, RecId);
    } else if (Key.FieldType == oftInt64 && Key.IsLinear()) {
        // index integer value using btree
        const int64 Int = Serializator.GetFieldInt64(RecMem, Key.FieldId);
        Index->IndexLinear(Key.KeyId, Int, RecId);
    } else if (Key.FieldType == oftUInt && Key.IsLinear()) {
        // index uint64 value using btree
        const uint UInt = Serializator.GetFieldUInt(RecMem, Key.FieldId);
        Index->IndexLinear(Key.KeyId, UInt, RecId);
    } else if (Key.FieldType == oftUInt16 && Key.IsLinear()) {
        // index uint64 value using btree
        const uint16 UInt16 = Serializator.GetFieldUInt16(RecMem, Key.FieldId);
        Index->IndexLinear(Key.KeyId, UInt16, RecId);
    } else if (Key.FieldType == oftUInt64 && Key.IsLinear()) {
        // index uint64 value using btree
        const uint64 UInt64 = Serializator.GetFieldUInt64(RecMem, Key.FieldId);
        Index->IndexLinear(Key.KeyId, UInt64, RecId);
    } else if (Key.FieldType == oftTm && Key.IsLinear()) {
        // index datetime value using btree
        const uint64 TmMSecs = Serializator.GetFieldTmMSecs(RecMem, Key.FieldId);
        Index->IndexLinear(Key.KeyId, TmMSecs, RecId);
    } else if (Key.FieldType == oftFlt && Key.IsLinear()) {
        // index float value using btree
        const double Flt = Serializator.GetFieldFlt(RecMem, Key.FieldId);
        Index->IndexLinear(Key.KeyId, Flt, RecId);
    } else if (Key.FieldType == oftSFlt && Key.IsLinear()) {
        // index float value using btree
        const float SFlt = Serializator.GetFieldSFlt(RecMem, Key.FieldId);
        Index->IndexLinear(Key.KeyId, SFlt, RecId);
    } else {
        ErrorLog(TStr::Fmt("[TFieldIndexer::IndexKey] Unsupported field and index type combination: %s[%s]: %s",
            Key.FieldNm.CStr(), Key.FieldTypeStr.CStr(), Key.GetKeyType().CStr()));
    }
}

void TRecIndexer::DeindexKey(const TFieldIndexKey& Key, const TMemBase& RecMem,
        const uint64& RecId, TRecSerializator& Serializator) {

    // check the type of field and value to select deindexing procedure
    if (Key.FieldType == oftStr && Key.IsValue()) {
        // inverted index over non-tokenized strings
        TStr Str = Serializator.GetFieldStr(RecMem, Key.FieldId);
        Index->DeleteValue(Key.KeyId, Str, RecId);
    } else if (Key.FieldType == oftStr && Key.IsText()) {
        // inverted index over tokenized strings
        TStr Str = Serializator.GetFieldStr(RecMem, Key.FieldId);
        Index->DeleteText(Key.KeyId, Str, RecId);
    } else if (Key.FieldType == oftStr && Key.IsTextPos()) {
        // inverted index over tokenized strings
        TStr Str = Serializator.GetFieldStr(RecMem, Key.FieldId);
        Index->DeleteTextPos(Key.KeyId, Str, RecId);
    } else if (Key.FieldType == oftStrV && Key.IsValue()) {
        // inverted index over string array
        TStrV StrV; Serializator.GetFieldStrV(RecMem, Key.FieldId, StrV);
        Index->DeleteValue(Key.KeyId, StrV, RecId);
    } else if (Key.FieldType == oftTm && Key.IsValue()) {
        // time indexed as timestamp string, TODO: proper time indexing
        const uint64 TmMSecs = Serializator.GetFieldTmMSecs(RecMem, Key.FieldId);
        Index->DeleteValue(Key.KeyId, TUInt64::GetStr(TmMSecs), RecId);
    } else if (Key.FieldType == oftFltPr && Key.IsLocation()) {
        // index geo-location using geo-index
        TFltPr FltPr = Serializator.GetFieldFltPr(RecMem, Key.FieldId);
        Index->DeleteGeo(Key.KeyId, FltPr, RecId);
    } else if (Key.FieldType == oftByte && Key.IsLinear()) {
        // index integer value using btree
        const uchar Byte = Serializator.GetFieldByte(RecMem, Key.FieldId);
        Index->DeleteLinear(Key.KeyId, Byte, RecId);
    } else if (Key.FieldType == oftInt && Key.IsLinear()) {
        // index integer value using btree
        const int Int = Serializator.GetFieldInt(RecMem, Key.FieldId);
        Index->DeleteLinear(Key.KeyId, Int, RecId);
    } else if (Key.FieldType == oftInt16 && Key.IsLinear()) {
        // index integer value using btree
        const int16 Int16 = Serializator.GetFieldInt16(RecMem, Key.FieldId);
        Index->DeleteLinear(Key.KeyId, Int16, RecId);
    } else if (Key.FieldType == oftInt64 && Key.IsLinear()) {
        // index integer value using btree
        const int64 Int64 = Serializator.GetFieldInt64(RecMem, Key.FieldId);
        Index->DeleteLinear(Key.KeyId, Int64, RecId);
    } else if (Key.FieldType == oftUInt && Key.IsLinear()) {
        // index uint value using btree
        const uint UInt = Serializator.GetFieldUInt(RecMem, Key.FieldId);
        Index->DeleteLinear(Key.KeyId, UInt, RecId);
    } else if (Key.FieldType == oftUInt16 && Key.IsLinear()) {
        // index uint16 value using btree
        const uint16 UInt16 = Serializator.GetFieldUInt16(RecMem, Key.FieldId);
        Index->DeleteLinear(Key.KeyId, UInt16, RecId);
    } else if (Key.FieldType == oftUInt64 && Key.IsLinear()) {
        // index uint64 value using btree
        const uint64 UInt64 = Serializator.GetFieldUInt64(RecMem, Key.FieldId);
        Index->DeleteLinear(Key.KeyId, UInt64, RecId);
    } else if (Key.FieldType == oftTm && Key.IsLinear()) {
        // index datetime value using btree
        const uint64 TmMSecs = Serializator.GetFieldTmMSecs(RecMem, Key.FieldId);
        Index->DeleteLinear(Key.KeyId, TmMSecs, RecId);
    } else if (Key.FieldType == oftFlt && Key.IsLinear()) {
        // index float value using btree
        const double Flt = Serializator.GetFieldFlt(RecMem, Key.FieldId);
        Index->DeleteLinear(Key.KeyId, Flt, RecId);
    } else if (Key.FieldType == oftSFlt && Key.IsLinear()) {
        // index float value using btree
        const float SFlt = Serializator.GetFieldSFlt(RecMem, Key.FieldId);
        Index->DeleteLinear(Key.KeyId, SFlt, RecId);
    } else {
        ErrorLog(TStr::Fmt("[TFieldIndexer::DeindexKey] Unsupported field and index type combination: %s[%s]: %s",
            Key.FieldNm.CStr(), Key.FieldTypeStr.CStr(), Key.GetKeyType().CStr()));
    }
}

void TRecIndexer::UpdateKey(const TFieldIndexKey& Key, const TMemBase& OldRecMem,
    const TMemBase& NewRecMem, const uint64& RecId, TRecSerializator& Serializator) {

    // check the type of field and value to select update procedure
    if (Key.FieldType == oftStr && Key.IsValue()) {
        // inverted index over non-tokenized strings
        TStr OldStr = Serializator.GetFieldStr(OldRecMem, Key.FieldId);
        TStr NewStr = Serializator.GetFieldStr(NewRecMem, Key.FieldId);
        if (OldStr == NewStr) { return; }
        Index->DeleteValue(Key.KeyId, OldStr, RecId);
        Index->IndexValue(Key.KeyId, NewStr, RecId);
    } else if (Key.FieldType == oftStr && Key.IsText()) {
        // inverted index over tokenized strings
        TStr OldStr = Serializator.GetFieldStr(OldRecMem, Key.FieldId);
        TStr NewStr = Serializator.GetFieldStr(NewRecMem, Key.FieldId);
        if (OldStr == NewStr) { return; }
        Index->DeleteText(Key.KeyId, OldStr, RecId);
        Index->IndexText(Key.KeyId, NewStr, RecId);
    } else if (Key.FieldType == oftStr && Key.IsTextPos()) {
        // inverted index over tokenized strings
        TStr OldStr = Serializator.GetFieldStr(OldRecMem, Key.FieldId);
        TStr NewStr = Serializator.GetFieldStr(NewRecMem, Key.FieldId);
        if (OldStr == NewStr) { return; }
        Index->DeleteTextPos(Key.KeyId, OldStr, RecId);
        Index->IndexTextPos(Key.KeyId, NewStr, RecId);
    } else if (Key.FieldType == oftStrV && Key.IsValue()) {
        // inverted index over string array
        TStrV OldStrV; Serializator.GetFieldStrV(OldRecMem, Key.FieldId, OldStrV);
        TStrV NewStrV; Serializator.GetFieldStrV(NewRecMem, Key.FieldId, NewStrV);
        if (OldStrV == NewStrV) { return; }
        Index->DeleteValue(Key.KeyId, OldStrV, RecId);
        Index->IndexValue(Key.KeyId, NewStrV, RecId);
    } else if (Key.FieldType == oftTm && Key.IsValue()) {
        // time indexed as timestamp string, TODO: proper time indexing
        const uint64 OldTmMSecs = Serializator.GetFieldTmMSecs(OldRecMem, Key.FieldId);
        const uint64 NewTmMSecs = Serializator.GetFieldTmMSecs(NewRecMem, Key.FieldId);
        if (OldTmMSecs == NewTmMSecs) { return; }
        Index->DeleteValue(Key.KeyId, TUInt64::GetStr(OldTmMSecs), RecId);
        Index->IndexValue(Key.KeyId, TUInt64::GetStr(NewTmMSecs), RecId);
    } else if (Key.FieldType == oftFltPr && Key.IsLocation()) {
        // index geo-location using geo-index
        TFltPr OldFltPr = Serializator.GetFieldFltPr(OldRecMem, Key.FieldId);
        TFltPr NewFltPr = Serializator.GetFieldFltPr(NewRecMem, Key.FieldId);
        if (Index->LocEquals(Key.KeyId, OldFltPr, NewFltPr)) { return; }
        Index->DeleteGeo(Key.KeyId, OldFltPr, RecId);
        Index->IndexGeo(Key.KeyId, NewFltPr, RecId);
    } else if (Key.FieldType == oftByte && Key.IsLinear()) {
        // index byte value using btree
        const uchar OldByte = Serializator.GetFieldByte(OldRecMem, Key.FieldId);
        const uchar NewByte = Serializator.GetFieldByte(NewRecMem, Key.FieldId);
        if (OldByte == NewByte) { return; }
        Index->DeleteLinear(Key.KeyId, OldByte, RecId);
        Index->IndexLinear(Key.KeyId, NewByte, RecId);
    } else if (Key.FieldType == oftInt && Key.IsLinear()) {
        // index integer value using btree
        const int OldInt = Serializator.GetFieldInt(OldRecMem, Key.FieldId);
        const int NewInt = Serializator.GetFieldInt(NewRecMem, Key.FieldId);
        if (OldInt == NewInt) { return; }
        Index->DeleteLinear(Key.KeyId, OldInt, RecId);
        Index->IndexLinear(Key.KeyId, NewInt, RecId);
    } else if (Key.FieldType == oftInt16 && Key.IsLinear()) {
        // index integer value using btree
        const int16 OldInt16 = Serializator.GetFieldInt16(OldRecMem, Key.FieldId);
        const int16 NewInt16 = Serializator.GetFieldInt16(NewRecMem, Key.FieldId);
        if (OldInt16 == NewInt16) { return; }
        Index->DeleteLinear(Key.KeyId, OldInt16, RecId);
        Index->IndexLinear(Key.KeyId, NewInt16, RecId);
    } else if (Key.FieldType == oftInt64 && Key.IsLinear()) {
        // index integer value using btree
        const int64 OldInt64 = Serializator.GetFieldInt64(OldRecMem, Key.FieldId);
        const int64 NewInt64 = Serializator.GetFieldInt64(NewRecMem, Key.FieldId);
        if (OldInt64 == NewInt64) { return; }
        Index->DeleteLinear(Key.KeyId, OldInt64, RecId);
        Index->IndexLinear(Key.KeyId, NewInt64, RecId);
    } else if (Key.FieldType == oftUInt && Key.IsLinear()) {
        // index uint64 value using btree
        const uint OldUInt = Serializator.GetFieldUInt(OldRecMem, Key.FieldId);
        const uint NewUInt = Serializator.GetFieldUInt(NewRecMem, Key.FieldId);
        if (OldUInt == NewUInt) { return; }
        Index->DeleteLinear(Key.KeyId, OldUInt, RecId);
        Index->IndexLinear(Key.KeyId, NewUInt, RecId);
    } else if (Key.FieldType == oftUInt16 && Key.IsLinear()) {
        // index uint64 value using btree
        const uint16 OldUInt16 = Serializator.GetFieldUInt16(OldRecMem, Key.FieldId);
        const uint16 NewUInt16 = Serializator.GetFieldUInt16(NewRecMem, Key.FieldId);
        if (OldUInt16 == NewUInt16) { return; }
        Index->DeleteLinear(Key.KeyId, OldUInt16, RecId);
        Index->IndexLinear(Key.KeyId, NewUInt16, RecId);
    } else if (Key.FieldType == oftUInt64 && Key.IsLinear()) {
        // index uint64 value using btree
        const uint64 OldUInt64 = Serializator.GetFieldUInt64(OldRecMem, Key.FieldId);
        const uint64 NewUInt64 = Serializator.GetFieldUInt64(NewRecMem, Key.FieldId);
        if (OldUInt64 == NewUInt64) { return; }
        Index->DeleteLinear(Key.KeyId, OldUInt64, RecId);
        Index->IndexLinear(Key.KeyId, NewUInt64, RecId);
    } else if (Key.FieldType == oftTm && Key.IsLinear()) {
        // index datetime value using btree
        const uint64 OldTmMSecs = Serializator.GetFieldTmMSecs(OldRecMem, Key.FieldId);
        const uint64 NewTmMSecs = Serializator.GetFieldTmMSecs(NewRecMem, Key.FieldId);
        if (OldTmMSecs == NewTmMSecs) { return; }
        Index->DeleteLinear(Key.KeyId, OldTmMSecs, RecId);
        Index->IndexLinear(Key.KeyId, NewTmMSecs, RecId);
    } else if (Key.FieldType == oftFlt && Key.IsLinear()) {
        // index float value using btree
        const double OldFlt = Serializator.GetFieldFlt(OldRecMem, Key.FieldId);
        const double NewFlt = Serializator.GetFieldFlt(NewRecMem, Key.FieldId);
        if (OldFlt == NewFlt) { return; }
        Index->DeleteLinear(Key.KeyId, OldFlt, RecId);
        Index->IndexLinear(Key.KeyId, NewFlt, RecId);
    } else if (Key.FieldType == oftSFlt && Key.IsLinear()) {
        // index float value using btree
        const float OldFlt = Serializator.GetFieldSFlt(OldRecMem, Key.FieldId);
        const float NewFlt = Serializator.GetFieldSFlt(NewRecMem, Key.FieldId);
        if (OldFlt == NewFlt) { return; }
        Index->DeleteLinear(Key.KeyId, OldFlt, RecId);
        Index->IndexLinear(Key.KeyId, NewFlt, RecId);
    } else {
        ErrorLog(TStr::Fmt("[TFieldIndexer::UpdateKey] Unsupported field and index type combination: %s[%s]: %s",
            Key.FieldNm.CStr(), Key.FieldTypeStr.CStr(), Key.GetKeyType().CStr()));
    }
}

void TRecIndexer::ProcessKey(const TFieldIndexKey& Key, const TMemBase& OldRecMem,
    const TMemBase& NewRecMem, const uint64& RecId, TRecSerializator& Serializator) {

    // check how to process the change
    const bool OldNullP = Serializator.IsFieldNull(OldRecMem, Key.FieldId);
    const bool NewNullP = Serializator.IsFieldNull(NewRecMem, Key.FieldId);
    if (OldNullP && !NewNullP) {
        // if no value before, just index
        IndexKey(Key, NewRecMem, RecId, Serializator);
    } else if (!OldNullP && NewNullP) {
        // no new value, just deindex
        DeindexKey(Key, OldRecMem, RecId, Serializator);
    } else if (!OldNullP && !NewNullP) {
        // value update, do deindexing of old and indexing of new
        UpdateKey(Key, OldRecMem, NewRecMem, RecId, Serializator);
    } else {
        // nothing before to deindex
        // nothing now to index
        // life is easy
    }
}

TRecIndexer::TRecIndexer(const TWPt<TIndex>& _Index, const TWPt<TStore>& Store):
        Index(_Index), IndexVoc(_Index->GetIndexVoc()) {

    // go over all the fields
    for (int FieldId = 0; FieldId < Store->GetFields(); FieldId++) {
        const TFieldDesc& FieldDesc = Store->GetFieldDesc(FieldId);
        // go over all keys associated with the field
        for (int KeyIdN = 0; KeyIdN < FieldDesc.GetKeys(); KeyIdN++) {
            const int KeyId = FieldDesc.GetKeyId(KeyIdN);
            const TIndexKey& Key = IndexVoc->GetKey(KeyId);
            // remember the field-key details
            const int KeyN = FieldIndexKeyV.Add(TFieldIndexKey(FieldId,
                FieldDesc.GetFieldNm(), FieldDesc.GetFieldType(),
                FieldDesc.GetFieldTypeStr(), KeyId, Key.GetType(),
                Key.GetWordVocId()));
            // remember mapping from field id to key position
            FieldIdToKeyN.AddDat(FieldId, KeyN);
        }
    }
}

void TRecIndexer::IndexRec(const TMemBase& RecMem, const uint64& RecId, TRecSerializator& Serializator, const PJsonVal RecJson) {
    // go over all keys associated with the store and its fields
    for (int FieldIndexKeyN = 0; FieldIndexKeyN < FieldIndexKeyV.Len(); FieldIndexKeyN++) {
        const TFieldIndexKey& Key = FieldIndexKeyV[FieldIndexKeyN];
        // check if field is handled by the serializator
        if (!Serializator.IsFieldId(Key.FieldId)) { continue; }
        // check if field is not NULL (e.g. there is something to index)
        if (Serializator.IsFieldNull(RecMem, Key.FieldId)) { continue; }
        // index the key
        IndexKey(Key, RecMem, RecId, Serializator, RecJson);
    }
}

void TRecIndexer::DeindexRec(const TMemBase& RecMem, const uint64& RecId, TRecSerializator& Serializator) {
    // go over all keys associated with the store and its fields
    for (int FieldIndexKeyN = 0; FieldIndexKeyN < FieldIndexKeyV.Len(); FieldIndexKeyN++) {
        const TFieldIndexKey& Key = FieldIndexKeyV[FieldIndexKeyN];
        // check if field is handled by the serializator
        if (!Serializator.IsFieldId(Key.FieldId)) { continue; }
        // check if field is not NULL (e.g. there is something to deindex)
        if (Serializator.IsFieldNull(RecMem, Key.FieldId)) { continue; }
        // deindex the key
        DeindexKey(Key, RecMem, RecId, Serializator);
    }
}

void TRecIndexer::GetGixKeyIdSet(TIntSet& KeyIdSet) const {
    for (int N = 0; N < FieldIndexKeyV.Len(); N++) {
        const TFieldIndexKey& Key = FieldIndexKeyV[N];
        if (Key.IsValue() || Key.IsText() || Key.IsTextPos()) {
            KeyIdSet.AddKey(Key.KeyId);
        }
    }
}

void TRecIndexer::DeindexRecNonGix(const TMemBase& RecMem, const uint64& RecId,
        TRecSerializator& Serializator) {
    for (int N = 0; N < FieldIndexKeyV.Len(); N++) {
        const TFieldIndexKey& Key = FieldIndexKeyV[N];
        if (Key.IsValue() || Key.IsText() || Key.IsTextPos()) { continue; }
        if (!Serializator.IsFieldId(Key.FieldId)) { continue; }
        if (Serializator.IsFieldNull(RecMem, Key.FieldId)) { continue; }
        DeindexKey(Key, RecMem, RecId, Serializator);
    }
}

void TRecIndexer::UpdateRec(const TMemBase& OldRecMem, const TMemBase& NewRecMem,
        const uint64& RecId, const int& ChangedFieldId, TRecSerializator& Serializator) {

    // check if we have a key for the field
    if (FieldIdToKeyN.IsKey(ChangedFieldId)) {
        // get field index key
        const int FieldIndexKeyN = FieldIdToKeyN.GetDat(ChangedFieldId);
        const TFieldIndexKey& Key = FieldIndexKeyV[FieldIndexKeyN];
        // check how to process the change
        ProcessKey(Key, OldRecMem, NewRecMem, RecId, Serializator);
    }
}

void TRecIndexer::UpdateRec(const TMemBase& OldRecMem, const TMemBase& NewRecMem,
        const uint64& RecId, TIntSet& ChangedFieldIdSet, TRecSerializator& Serializator) {

    // go over all keys associated with the store and its fields
    for (int FieldIndexKeyN = 0; FieldIndexKeyN < FieldIndexKeyV.Len(); FieldIndexKeyN++) {
        const TFieldIndexKey& Key = FieldIndexKeyV[FieldIndexKeyN];
        // check if field is changed
        if (!ChangedFieldIdSet.IsKey(Key.FieldId)) { continue; }
        // check if field is handled by the serializator
        if (!Serializator.IsFieldId(Key.FieldId)) { continue; }
        // check how to process the change
        ProcessKey(Key, OldRecMem, NewRecMem, RecId, Serializator);
    }
}

void TRecIndexer::DeindexRecField(const TMemBase& RecMem, const uint64& RecId, const int& FieldId, TRecSerializator& Serializator)
{
    // check if we have a key for the field
    if (FieldIdToKeyN.IsKey(FieldId)) {
        // get field index key
        const int FieldIndexKeyN = FieldIdToKeyN.GetDat(FieldId);
        const TFieldIndexKey& Key = FieldIndexKeyV[FieldIndexKeyN];
        // deindex the content
        DeindexKey(Key, RecMem, RecId, Serializator);
    }
}

void TRecIndexer::IndexRecField(const TMemBase& RecMem, const uint64& RecId, const int& FieldId, TRecSerializator& Serializator)
{
    // check if we have a key for the field
    if (FieldIdToKeyN.IsKey(FieldId)) {
        // get field index key
        const int FieldIndexKeyN = FieldIdToKeyN.GetDat(FieldId);
        const TFieldIndexKey& Key = FieldIndexKeyV[FieldIndexKeyN];
        // deindex the content
        IndexKey(Key, RecMem, RecId, Serializator);
    }
}

bool TRecIndexer::IsFieldIndexKey(const int& FieldId) const {
    // go over all keys associated with the store and its fields
    for (int i = 0; i < FieldIndexKeyV.Len(); i++) {
        if (FieldIndexKeyV[i].FieldId == FieldId) {
            return true;
        }
    }
    return false;
}

///////////////////////////////
/// Implementation of store which does not store any records
TStoreEmpty::TStoreEmpty(const TWPt<TBase>& _Base, const uint& StoreId, const TStr& StoreName,
        const TStoreSchema& StoreSchema): TStore(_Base, StoreId, StoreName) {

    QmAssertR(StoreSchema.DefaultFieldStoreLoc == slMemory, "TStoreEmpty does not support disk location");
    // create fields
    for (auto& FieldNmDesc : StoreSchema.FieldH) {
        // get field details
        const TStr& FieldNm = FieldNmDesc.Key;
        const TFieldDesc& FieldDesc = FieldNmDesc.Dat;
        const TFieldDescEx& FieldDescEx = StoreSchema.FieldExH.GetDat(FieldNm);
        // add it to the store
        AddFieldDesc(FieldDesc);
        // make sure it does not assume any storage
        QmAssertR(FieldDescEx.FieldStoreLoc == slMemory, "TStoreEmpty does not support disk location");
    }
    // make sure we do not have any index keys
    QmAssertR(StoreSchema.IndexKeyExV.Len() == 0, "TStoreEmpty does not support index keys");
    // make sure we do not have any index joins
    for (TJoinDescEx& JoinDescEx : StoreSchema.JoinDescExV) {
        // we support only field joins
        QmAssertR(JoinDescEx.JoinType == osjtField, "TStoreEmpty supports only field joins");
    }
}

///////////////////////////////
/// Implementation of store which can be initialized from a schema.
void TStoreImpl::InitFieldLocV() {
    for (int FieldId = 0; FieldId < GetFields(); FieldId++) {
        if (SerializatorCache->IsFieldId(FieldId)) {
            FieldLocV.Add(slDisk);
        } else if (SerializatorMem->IsFieldId(FieldId)) {
            FieldLocV.Add(slMemory);
        } else {
            throw TQmExcept::New("Unknown storage location for field " +
                GetFieldNm(FieldId) + " in store " + GetStoreNm());
        }
    }
}

void TStoreImpl::GetRecMem(const TStoreLoc& RecLoc, const uint64& RecId, TMem& Rec) const {
    if (RecLoc == slDisk) {
        DataCache.GetVal(RecId, Rec);
    } else if (RecLoc == slMemory)  {
        DataMem.GetVal(RecId, Rec);
    } else {
        throw TQmExcept::New("Unknown storage location");
    }
}

void TStoreImpl::GetRecMem(const uint64& RecId, const int& FieldId, TMem& Rec) const {
    GetRecMem(FieldLocV[FieldId], RecId, Rec);
}

void TStoreImpl::PutRecMem(const TStoreLoc& RecLoc, const uint64& RecId, const TMem& Rec) {
    if (RecLoc == slDisk) {
        DataCache.SetVal(RecId, Rec);
    } else if (RecLoc == slMemory)  {
        DataMem.SetVal(RecId, Rec);
    } else {
        throw TQmExcept::New("Unknown storage location");
    }
}

void TStoreImpl::PutRecMem(const uint64& RecId, const int& FieldId, const TMem& Rec) {
    PutRecMem(FieldLocV[FieldId], RecId, Rec);
}

bool TStoreImpl::IsFieldDisk(const int &FieldId) const {
    return SerializatorCache->IsFieldId(FieldId);
}

bool TStoreImpl::IsFieldInMemory(const int &FieldId) const {
    return SerializatorMem->IsFieldId(FieldId);
}

TRecSerializator* TStoreImpl::GetSerializator(const TStoreLoc& StoreLoc) {
    return (StoreLoc == slMemory) ? SerializatorMem : SerializatorCache;
}

const TRecSerializator* TStoreImpl::GetSerializator(const TStoreLoc& StoreLoc) const {
    return (StoreLoc == slMemory) ? SerializatorMem : SerializatorCache;
}

TRecSerializator* TStoreImpl::GetFieldSerializator(const int &FieldId) {
    return GetSerializator(FieldLocV[FieldId]);
}

const TRecSerializator* TStoreImpl::GetFieldSerializator(const int &FieldId) const {
    return GetSerializator(FieldLocV[FieldId]);
}

void TStoreImpl::SetPrimaryField(const uint64& RecId) {
    MetaDirtyP = true;
    if (PrimaryFieldType == oftStr) {
        PrimaryStrIdH.AddDat(GetFieldStr(RecId, PrimaryFieldId)) = RecId;
    } else if (PrimaryFieldType == oftInt) {
        PrimaryIntIdH.AddDat(GetFieldInt(RecId, PrimaryFieldId)) = RecId;
    } else if (PrimaryFieldType == oftUInt64) {
        PrimaryUInt64IdH.AddDat(GetFieldUInt64(RecId, PrimaryFieldId)) = RecId;
    } else if (PrimaryFieldType == oftFlt) {
        PrimaryFltIdH.AddDat(GetFieldFlt(RecId, PrimaryFieldId)) = RecId;
    } else if (PrimaryFieldType == oftTm) {
        PrimaryTmMSecsIdH.AddDat(GetFieldTmMSecs(RecId, PrimaryFieldId)) = RecId;
    } else {
        EAssertR(false, "Unsupported primary-field type");
    }
}

void TStoreImpl::SetPrimaryFieldStr(const uint64& RecId, const TStr& Str) {
    PrimaryStrIdH.AddDat(Str) = RecId;
    MetaDirtyP = true;
}

void TStoreImpl::SetPrimaryFieldInt(const uint64& RecId, const int& Int) {
    PrimaryIntIdH.AddDat(Int) = RecId;
    MetaDirtyP = true;
}

void TStoreImpl::SetPrimaryFieldUInt64(const uint64& RecId, const uint64& UInt64) {
    PrimaryUInt64IdH.AddDat(UInt64) = RecId;
    MetaDirtyP = true;
}

void TStoreImpl::SetPrimaryFieldFlt(const uint64& RecId, const double& Flt) {
    PrimaryFltIdH.AddDat(Flt) = RecId;
    MetaDirtyP = true;
}

void TStoreImpl::SetPrimaryFieldMSecs(const uint64& RecId, const uint64& MSecs) {
    PrimaryTmMSecsIdH.AddDat(MSecs) = RecId;
    MetaDirtyP = true;
}

void TStoreImpl::DelPrimaryField(const uint64& RecId) {
    MetaDirtyP = true;
    if (PrimaryFieldType == oftStr) {
        PrimaryStrIdH.DelIfKey(GetFieldStr(RecId, PrimaryFieldId));
    } else if (PrimaryFieldType == oftInt) {
        PrimaryIntIdH.DelIfKey(GetFieldInt(RecId, PrimaryFieldId));
    } else if (PrimaryFieldType == oftUInt64) {
        PrimaryUInt64IdH.DelIfKey(GetFieldUInt64(RecId, PrimaryFieldId));
    } else if (PrimaryFieldType == oftFlt) {
        PrimaryFltIdH.DelIfKey(GetFieldFlt(RecId, PrimaryFieldId));
    } else if (PrimaryFieldType == oftTm) {
        PrimaryTmMSecsIdH.DelIfKey(GetFieldTmMSecs(RecId, PrimaryFieldId));
    } else {
        EAssertR(false, "Unsupported primary-field type");
    }
}

void TStoreImpl::DelPrimaryFieldStr(const uint64& RecId, const TStr& Str) {
    Assert(PrimaryStrIdH.GetDat(Str) == RecId);
    PrimaryStrIdH.DelIfKey(Str);
    MetaDirtyP = true;
}

void TStoreImpl::DelPrimaryFieldInt(const uint64& RecId, const int& Int) {
    Assert(PrimaryIntIdH.GetDat(Int) == RecId);
    PrimaryIntIdH.DelIfKey(Int);
    MetaDirtyP = true;
}

void TStoreImpl::DelPrimaryFieldUInt64(const uint64& RecId, const uint64& UInt64) {
    Assert(PrimaryUInt64IdH.GetDat(UInt64) == RecId);
    PrimaryUInt64IdH.DelIfKey(UInt64);
    MetaDirtyP = true;
}

void TStoreImpl::DelPrimaryFieldFlt(const uint64& RecId, const double& Flt) {
    Assert(PrimaryFltIdH.GetDat(Flt) == RecId);
    PrimaryFltIdH.DelIfKey(Flt);
    MetaDirtyP = true;
}

void TStoreImpl::DelPrimaryFieldMSecs(const uint64& RecId, const uint64& MSecs) {
    Assert(PrimaryTmMSecsIdH.GetDat(MSecs) == RecId);
    PrimaryTmMSecsIdH.DelIfKey(MSecs);
    MetaDirtyP = true;
}

void TStoreImpl::InitFromSchema(const TStoreSchema& StoreSchema) {
    // at start there is no primary key
    RecNmFieldP = false;
    PrimaryFieldId = -1;
    PrimaryFieldType = oftUndef;
    // create fields
    for (int i = 0; i < StoreSchema.FieldH.Len(); i++) {
        const TFieldDesc& FieldDesc = StoreSchema.FieldH[i];
        AddFieldDesc(FieldDesc);
        // check if we found a primary field
        if (FieldDesc.IsPrimary()) {
            QmAssertR(PrimaryFieldId == -1, "Store can have only one primary field");
            // only string fields can serve as record name (TODO: extend)
            RecNmFieldP = FieldDesc.IsStr();
            PrimaryFieldId = GetFieldId(FieldDesc.GetFieldNm());
            PrimaryFieldType = FieldDesc.GetFieldType();
        }
    }
    // create index keys
    TWPt<TIndexVoc> IndexVoc = GetIndex()->GetIndexVoc();
    for (int IndexKeyExN = 0; IndexKeyExN < StoreSchema.IndexKeyExV.Len(); IndexKeyExN++) {
        TIndexKeyEx IndexKeyEx = StoreSchema.IndexKeyExV[IndexKeyExN];
        // get associated field
        const int FieldId = GetFieldId(IndexKeyEx.FieldName);
        // if we are given vocabulary name, check if we have one with such name already
        const int WordVocId = GetBase()->NewIndexWordVoc(IndexKeyEx.KeyType, IndexKeyEx.WordVocName);
        // create new index key
        const int KeyId = GetBase()->NewFieldIndexKey(this, IndexKeyEx.KeyIndexName,
            FieldId, WordVocId, IndexKeyEx.KeyType, IndexKeyEx.GixType, IndexKeyEx.SortType);
        // assign tokenizer to it if we have one
        if (IndexKeyEx.IsTokenizer()) { IndexVoc->PutTokenizer(KeyId, IndexKeyEx.Tokenizer); }
    }
    // prepare serializators for disk and in-memory store
    SerializatorCache = new TRecSerializator(this, this, StoreSchema, slDisk);
    SerializatorMem = new TRecSerializator(this, this, StoreSchema, slMemory);
    // initialize field to storage location map
    InitFieldLocV();
    // initialize record indexer
    RecIndexer = TRecIndexer(GetIndex(), this);
    // remember window parameters
    WndDesc = StoreSchema.WndDesc;
}

void TStoreImpl::InitDataFlags() {
    // go over all the fields and remember if we use in-memory or cache storage
    DataCacheP = false;
    DataMemP = false;
    for (int FieldId = 0; FieldId < GetFields(); FieldId++) {
        DataCacheP = DataCacheP || (FieldLocV[FieldId] == slDisk);
        DataMemP = DataMemP || (FieldLocV[FieldId] == slMemory);
    }
    // at least one must be true, otherwise we have no fields, which is not good
    EAssert(DataCacheP || DataMemP);
}

TStoreImpl::TStoreImpl(const TWPt<TBase>& Base, const uint& StoreId,
    const TStr& StoreName, const TStoreSchema& StoreSchema, const TStr& _StoreFNm,
    const int64& _MxCacheSize, const int& BlockSize):
        TStore(Base, StoreId, StoreName), StoreFNm(_StoreFNm), FAccess(Base->GetFAccess()),
        DataCache(_StoreFNm + ".Cache", Base->GetStoreBlobBs(), _MxCacheSize, 1024),
        DataMem(_StoreFNm + ".MemCache", Base->GetStoreBlobBs(), BlockSize) {

    SetStoreType("TStoreImpl");
    // freshly created store must write its metadata files at least once
    MetaDirtyP = true;
    InitFromSchema(StoreSchema);
    // initialize data storage flags
    InitDataFlags();
}

TStoreImpl::TStoreImpl(const TWPt<TBase>& Base, const TStr& _StoreFNm,
    const int64& _MxCacheSize, const bool& _Lazy): TStore(Base, _StoreFNm + ".BaseStore"),
        StoreFNm(_StoreFNm), FAccess(Base->GetFAccess()), PrimaryFieldType(oftUndef),
        DataCache(_StoreFNm + ".Cache", Base->GetStoreBlobBs(), Base->GetFAccess(), _MxCacheSize),
        DataMem(_StoreFNm + ".MemCache", Base->GetStoreBlobBs(), Base->GetFAccess(), _Lazy) {

    SetStoreType("TStoreImpl");
    // load members
    TFIn FIn(StoreFNm + ".GenericStore");
    RecNmFieldP.Load(FIn);
    PrimaryFieldId.Load(FIn);
    // deduce primary field type
    if (PrimaryFieldId != -1) {
        PrimaryFieldType = GetFieldDesc(PrimaryFieldId).GetFieldType();
        if (PrimaryFieldType == oftStr) {
            PrimaryStrIdH.Load(FIn);
        } else if (PrimaryFieldType == oftInt) {
            PrimaryIntIdH.Load(FIn);
        } else if (PrimaryFieldType == oftUInt64) {
            PrimaryUInt64IdH.Load(FIn);
        } else if (PrimaryFieldType == oftFlt) {
            PrimaryFltIdH.Load(FIn);
        } else if (PrimaryFieldType == oftTm) {
            PrimaryTmMSecsIdH.Load(FIn);
        } else {
            throw TQmExcept::New("Unsupported primary field type!");
        }
    } else {
        // backwards compatibility
        PrimaryStrIdH.Load(FIn);
    }
    // load time window
    WndDesc.Load(FIn);
    // load data
    SerializatorCache = new TRecSerializator(this);
    SerializatorMem = new TRecSerializator(this);
    SerializatorCache->Load(FIn);
    SerializatorMem->Load(FIn);

    // initialize field to storage location map
    InitFieldLocV();
    // initialize record indexer
    RecIndexer = TRecIndexer(GetIndex(), this);

    // initialize data storage flags
    InitDataFlags();

    // nothing was modified yet with regards to the loaded metadata
    MetaDirtyP = false;
}

TStoreImpl::~TStoreImpl() {
    // save if necessary; when the metadata (primary-field maps, which are the only
    // parts that can change at runtime) is unchanged, skip the rewrite - it
    // dominated shutdown time on large read-mostly stores
    if (FAccess == faRdOnly) {
        TEnv::Logger->OnStatus("No saving of generic store " + GetStoreNm() + " neccessary!");
    } else if (!MetaDirtyP) {
        TEnv::Logger->OnStatus(TStr::Fmt("Store '%s' metadata unchanged - not saving", GetStoreNm().CStr()));
    } else {
        TEnv::Logger->OnStatus(TStr::Fmt("Saving store '%s'...", GetStoreNm().CStr()));
        // save base store
        TFOut BaseFOut(StoreFNm + ".BaseStore");
        SaveStore(BaseFOut);
        // save store parameters
        TFOut FOut(StoreFNm + ".GenericStore");
        // save parameters about primary field
        RecNmFieldP.Save(FOut);
        PrimaryFieldId.Save(FOut);
        if (PrimaryFieldType == oftInt) {
            PrimaryIntIdH.Save(FOut);
        } else if (PrimaryFieldType == oftUInt64) {
            PrimaryUInt64IdH.Save(FOut);
        } else if (PrimaryFieldType == oftFlt) {
            PrimaryFltIdH.Save(FOut);
        } else if (PrimaryFieldType == oftTm) {
            PrimaryTmMSecsIdH.Save(FOut);
        } else {
            PrimaryStrIdH.Save(FOut);
        }
        // save time window
        WndDesc.Save(FOut);
        // save data
        SerializatorCache->Save(FOut);
        SerializatorMem->Save(FOut);
    }
    delete SerializatorCache;
    delete SerializatorMem;
}

bool TStoreImpl::IsRecId(const uint64& RecId) const {
    return DataMemP ? DataMem.IsValId(RecId) : DataCache.IsValId(RecId);
}

uint64 TStoreImpl::GetRecs() const {
    return DataMemP ? DataMem.Len() : DataCache.Len();
}

bool TStoreImpl::IsRecNm(const TStr& RecNm) const {
    return RecNmFieldP && PrimaryStrIdH.IsKey(RecNm);
}

TStr TStoreImpl::GetRecNm(const uint64& RecId) const {
    // return empty string when no primary key
    if (!HasRecNm()) { return TStr(); }
    // get the name of primary key
    return GetFieldStr(RecId, PrimaryFieldId);
}

uint64 TStoreImpl::GetRecId(const TStr& RecNm) const {
    return (PrimaryStrIdH.IsKey(RecNm) ? PrimaryStrIdH.GetDat(RecNm).Val : TUInt64::Mx);
}

PStoreIter TStoreImpl::GetIter() const {
    if (Empty()) { return TStoreIterVec::New(); }
    return DataMemP ?
        TStoreIterVec::New(DataMem.GetFirstValId(), DataMem.GetLastValId(), true) :
        TStoreIterVec::New(DataCache.GetFirstValId(), DataCache.GetLastValId(), true);
}

uint64 TStoreImpl::GetFirstRecId() const {
    return Empty() ? TUInt64::Mx :
        (DataMemP ? DataMem.GetFirstValId() : DataCache.GetFirstValId());
}

uint64 TStoreImpl::GetLastRecId() const {
    return Empty() ? TUInt64::Mx :
        (DataMemP ? DataMem.GetLastValId() : DataCache.GetLastValId());
}

PStoreIter TStoreImpl::BackwardIter() const {
    if (Empty()) { return TStoreIterVec::New(); }
    return DataMemP ?
        TStoreIterVec::New(DataMem.GetLastValId(), DataMem.GetFirstValId(), false) :
        TStoreIterVec::New(DataCache.GetLastValId(), DataCache.GetFirstValId(), false);
}

uint64 TStoreImpl::AddRec(const PJsonVal& RecVal, const bool& TriggerEvents) {
    // check if we are given reference to existing record
    try {
        // parse out record id, if referred directly
        {
            const uint64 RecId = TStore::GetRecId(RecVal);
            if (IsRecId(RecId)) {
                // check if we have anything more than record identifier, which would require calling UpdateRec
                if (RecVal->GetObjKeys() > 1) { UpdateRec(RecId, RecVal); }
                // return named record
                return RecId;
            }
        }
        // check if we have a primary field
        if (IsPrimaryField()) {
            uint64 PrimaryRecId = TUInt64::Mx;
            // primary field cannot be nullable, so we must have it
            const TStr& PrimaryField = GetFieldNm(PrimaryFieldId);
            QmAssertR(RecVal->IsObjKey(PrimaryField), "Missing primary field in the record: " + PrimaryField);
            // parse based on the field type
            if (PrimaryFieldType == oftStr) {
                TStr FieldVal = RecVal->GetObjStr(PrimaryField);
                if (PrimaryStrIdH.IsKey(FieldVal)) {
                    PrimaryRecId = PrimaryStrIdH.GetDat(FieldVal);
                }
            } else if (PrimaryFieldType == oftInt) {
                const int FieldVal = RecVal->GetObjInt(PrimaryField);
                if (PrimaryIntIdH.IsKey(FieldVal)) {
                    PrimaryRecId = PrimaryIntIdH.GetDat(FieldVal);
                }
            } else if (PrimaryFieldType == oftUInt64) {
                const uint64 FieldVal = RecVal->GetObjUInt64(PrimaryField);
                if (PrimaryUInt64IdH.IsKey(FieldVal)) {
                    PrimaryRecId = PrimaryUInt64IdH.GetDat(FieldVal);
                }
            } else if (PrimaryFieldType == oftFlt) {
                const double FieldVal = RecVal->GetObjNum(PrimaryField);
                if (PrimaryFltIdH.IsKey(FieldVal)) {
                    PrimaryRecId = PrimaryFltIdH.GetDat(FieldVal);
                }
            } else if (PrimaryFieldType == oftTm) {
                const uint64 FieldVal = RecVal->GetObjTmMSecs(PrimaryField);
                if (PrimaryTmMSecsIdH.IsKey(FieldVal)) {
                    PrimaryRecId = PrimaryTmMSecsIdH.GetDat(FieldVal);
                }
            } else {
                EAssertR(false, "Unsupported primary-field type");
            }
            // check if we found primary field with existing value
            if (PrimaryRecId != TUInt64::Mx) {
                // check if we have anything more than primary field, which would require redirect to UpdateRec
                if (RecVal->GetObjKeys() > 1) { UpdateRec(PrimaryRecId, RecVal); }
                // return id of named record
                return PrimaryRecId;
            }
        }
    } catch (const PExcept& Except) {
        // error parsing, report error and return nothing
        ErrorLog("[TStoreImpl::AddRec] Error parsing out reference to existing record:");
        ErrorLog(Except->GetMsgStr());
        return TUInt64::Mx;
    }

    // always add system field that means "inserted_at"
    RecVal->AddToObj(TStoreWndDesc::SysInsertedAtFieldName, TTm::GetCurUniTm().GetStr());

    // for storing record id
    uint64 RecId = TUInt64::Mx;
    uint64 CacheRecId = TUInt64::Mx;
    uint64 MemRecId = TUInt64::Mx;
    // store to disk storage
    if (DataCacheP) {
        TMem CacheRecMem;
        SerializatorCache->Serialize(RecVal, CacheRecMem, this);
        CacheRecId = DataCache.AddVal(CacheRecMem);
        RecId = CacheRecId;
        // index new record
        RecIndexer.IndexRec(CacheRecMem, RecId, *SerializatorCache, RecVal);
    }
    // store to in-memory storage
    if (DataMemP) {
        TMem MemRecMem;
        SerializatorMem->Serialize(RecVal, MemRecMem, this);
        MemRecId = DataMem.AddVal(MemRecMem);
        RecId = MemRecId;
        // index new record
        RecIndexer.IndexRec(MemRecMem, RecId, *SerializatorMem, RecVal);
    }
    // make sure we are consistent with respect to Ids!
    if (DataCacheP && DataMemP) {
        EAssert(CacheRecId == MemRecId);
    }

    // remember value-recordId map when primary field available
    if (IsPrimaryField()) { SetPrimaryField(RecId); }

    // insert nested join records
    AddJoinRec(RecId, RecVal);
    // call add triggers
    if (TriggerEvents) {
        OnAdd(RecId);
    }

    // return record Id of the new record
    return RecId;
}

void TStoreImpl::UpdateRec(const uint64& RecId, const PJsonVal& RecVal) {
    // figure out which storage fields are affected
    bool CacheP = false, MemP = false, PrimaryP = false;
    for (int FieldId = 0; FieldId < GetFields(); FieldId++) {
        // check if field appears in the record JSon
        TStr FieldNm = GetFieldNm(FieldId);
        if (RecVal->IsObjKey(FieldNm)) {
            CacheP = CacheP || (FieldLocV[FieldId] == slDisk);
            MemP = MemP || (FieldLocV[FieldId] == slMemory);
            PrimaryP = PrimaryP || (FieldId == PrimaryFieldId);
        }
    }
    // remove old primary field
    if (PrimaryP) { DelPrimaryField(RecId); }
    // update disk serialization when necessary
    if (CacheP) {
        // update serialization
        TMem CacheOldRecMem; DataCache.GetVal(RecId, CacheOldRecMem);
        TMem CacheNewRecMem; TIntSet CacheChangedFieldIdSet;
        SerializatorCache->SerializeUpdate(RecVal, CacheOldRecMem,
            CacheNewRecMem, this, CacheChangedFieldIdSet);
        // update the stored serializations with new values
        DataCache.SetVal(RecId, CacheNewRecMem);
        // update indexes pointing to the record
        RecIndexer.UpdateRec(CacheOldRecMem, CacheNewRecMem, RecId, CacheChangedFieldIdSet, *SerializatorCache);
    }
    // update in-memory serialization when necessary
    if (MemP) {
        // update serialization
        TMem MemOldRecMem; DataMem.GetVal(RecId, MemOldRecMem);
        TMem MemNewRecMem; TIntSet MemChangedFieldIdSet;
        SerializatorMem->SerializeUpdate(RecVal, MemOldRecMem,
            MemNewRecMem, this, MemChangedFieldIdSet);
        // update the stored serializations with new values
        DataMem.SetVal(RecId, MemNewRecMem);
        // update indexes pointing to the record
        RecIndexer.UpdateRec(MemOldRecMem, MemNewRecMem, RecId, MemChangedFieldIdSet, *SerializatorMem);
    }
    // check if primary key changed and update the mapping
    if (PrimaryP) { SetPrimaryField(RecId); }
    // call update triggers
    OnUpdate(RecId);
}

void TStoreImpl::GarbageCollect(const int& MxTimeMSecs) {
    // if no window, nothing to do here
    if (WndDesc.WindowType == swtNone) { return; }
    // if no records, nothing to do here
    if (Empty()) { return; }
    // report on activity
    TEnv::Logger->OnStatusFmt("Garbage Collection in %s", GetStoreNm().CStr());
    TEnv::Logger->OnStatusFmt("  %s records at start", TUInt64::GetStr(GetRecs()).CStr());

    // prepare list of records that need to be deleted
    TUInt64V DelRecIdV;
    if (WndDesc.WindowType == swtTime) {
        // get last added record
        const uint64 LastRecId = GetLastRecId();
        // get time window field
        const int TimeFieldId = GetFieldId(WndDesc.TimeFieldNm);
        // get time which we use as end of time-window (could be insert time or field value)
        uint64 CurMSecs = WndDesc.InsertP ? TTm::GetCurUniMSecs() :
            GetFieldTmMSecs(LastRecId, TimeFieldId);
        // get start of time window
        const uint64 WindowStartMSecs = CurMSecs - WndDesc.WindowSize;
        // report what is the established time window used by the garbage collection
        TEnv::Logger->OnStatusFmt("  window: %s - %s",
            TTm::GetTmFromMSecs(WindowStartMSecs).GetWebLogDateTimeStr(true, "T", false).CStr(),
            TTm::GetTmFromMSecs(CurMSecs).GetWebLogDateTimeStr(true, "T", false).CStr());
        // iterate from the start until we hit the time window
        PStoreIter Iter = GetIter();
        while (Iter->Next()) {
            uint64 RecId = Iter->GetRecId();
            // get record time
            uint64 TmMSecs = GetFieldTmMSecs(RecId, TimeFieldId);
            // if we are within time window we stop
            if (TmMSecs >= WindowStartMSecs) break;
            // otherwise we mark the record for deletion
            DelRecIdV.Add(RecId);
        }
        // report progress
    } else if (GetRecs() > WndDesc.WindowSize) {
        // we are windowing based on number of records
        TEnv::Logger->OnStatusFmt("  window: last %d records", (int)WndDesc.WindowSize);
        // get number of records which need to be deleted so we are back in the window
        int DelRecs = (int)(GetRecs() - WndDesc.WindowSize);
        // iterate from the start until we hit the time window
        PStoreIter Iter = GetIter();
        while (Iter->Next() && DelRecs > 0) {
            // mark record for deletion
            DelRecIdV.Add(Iter->GetRecId());
            // track progress
            DelRecs--;
        }
    }
    TEnv::Logger->OnStatusFmt("  purging %d records", DelRecIdV.Len());
    TStoreImpl::DeleteRecs(DelRecIdV, MxTimeMSecs, false);
}

/// Deletes all records
void TStoreImpl::DeleteAllRecs() {
    // if no records, nothing to do here
    if (Empty()) { return; }
    TEnv::Logger->OnStatusFmt("Deleting all (%d) records in %s", GetRecs(), GetStoreNm().CStr());

    // NOTE: if you change the logic bellow, be sure to also change the DeleteRecs() method

    // delete records from index
    for (uint64 DelRecId = GetFirstRecId(); DelRecId <= GetLastRecId(); DelRecId++) {
        // executed triggers before deletion
        OnDelete(DelRecId);
        // delete record from name-id map
        if (IsPrimaryField()) { DelPrimaryField(DelRecId); }
        // delete record from indexes
        if (DataCacheP) {
            TMem CacheRecMem;
            DataCache.GetVal(DelRecId, CacheRecMem);
            RecIndexer.DeindexRec(CacheRecMem, DelRecId, *SerializatorCache);
        }
        if (DataMemP) {
            TMem MemRecMem;
            DataMem.GetVal(DelRecId, MemRecMem);
            RecIndexer.DeindexRec(MemRecMem, DelRecId, *SerializatorMem);
        }
        // delete record from joins
        TRec Rec(this, DelRecId);
        for (int JoinN = 0; JoinN < GetJoins(); JoinN++) {
            TJoinDesc JoinDesc = GetJoinDesc(JoinN);
            // execute the join
            PRecSet JoinRecSet = Rec.DoJoin(GetBase(), JoinDesc.GetJoinId());
            for (int JoinRecN = 0; JoinRecN < JoinRecSet->GetRecs(); JoinRecN++) {
                // remove joins with all matched records, one by one
                const uint64 JoinRecId = JoinRecSet->GetRecId(JoinRecN);
                const int JoinFq = JoinRecSet->GetRecFq(JoinRecN);
                DelJoin(JoinDesc.GetJoinId(), DelRecId, JoinRecId, JoinFq);
            }
        }
    }
    // delete records from disk
    PrimaryStrIdH.Clr();
    PrimaryIntIdH.Clr();
    PrimaryUInt64IdH.Clr();
    PrimaryFltIdH.Clr();
    PrimaryTmMSecsIdH.Clr();
    MetaDirtyP = true;
    DataCache.DelVals(TInt::Mx);
    DataMem.DelVals(TInt::Mx);
    PartialFlush(TInt::Mx);
}

void TStoreImpl::DeleteFirstRecs(const int& DelRecs)  {
    // if no records, nothing to do here
    if (Empty()) { return; }
    // report on activity
    TEnv::Logger->OnStatusFmt("Deleting %d records in %s", DelRecs, GetStoreNm().CStr());
    TEnv::Logger->OnStatusFmt("  %s records at start", TUInt64::GetStr(GetRecs()).CStr());

    // prepare list of records that need to be deleted
    TUInt64V DelRecIdV;
    // iterate from the start until we hit the time window
    PStoreIter Iter = GetIter();
    int Deleted = 0;
    while (Iter->Next() && Deleted < DelRecs) {
        // mark record for deletion
        DelRecIdV.Add(Iter->GetRecId());
        // track progress
        Deleted++;
    }
    TStoreImpl::DeleteRecs(DelRecIdV, -1, false);
}

void TStoreImpl::DeleteRecs(const TUInt64V& DelRecIdV, const int& MxTimeMSecs, const bool& AssertOK) {
    if (AssertOK) {
        // assert that DelRecIdV is valid, without gaps and that deleting will not create gaps
        PStoreIter Iter = GetIter();
        int Counter = 0;
        QmAssertR((uint64)DelRecIdV.Len() <= GetRecs(), "TStoreImpl::DeleteRecs: "
            "incorrect record id sequence. The length is greater than the total number of records.");
        while (Iter->Next() && Counter < DelRecIdV.Len()) {
            QmAssertR(DelRecIdV[Counter] == Iter->GetRecId(), "TStoreImpl::DeleteRecs: "
                "incorrect record id sequence. The sequence should start at the first store "
                "records, should contain only record ids and should not contain gaps");
            Counter++;
        }
    }

    // NOTE: if you change the logic bellow, be sure to also change the DeleteAllRecs() method

    // delete records from index
    TTmStopWatch StopWatch(true);
    int DeletedRecs = 0;
    for (int DelRecN = 0; DelRecN < DelRecIdV.Len(); DelRecN++) {
        // report progress
        if (DelRecN > 0 && DelRecN % 1000 == 0) {
            TEnv::Logger->OnStatusFmt("    %d\r", DelRecN);
        }
        // check if we still have time
        if ((MxTimeMSecs != -1) && (StopWatch.GetMSecInt() > MxTimeMSecs)) {
            TEnv::Logger->OnStatusFmt("Reached time limit of %d msecs in TStoreImpl::DeleteRecs");
            break;
        }
        // what are we deleting now
        const uint64 DelRecId = DelRecIdV[DelRecN];
        // executed triggers before deletion
        OnDelete(DelRecId);
        // delete record from name-id map
        if (IsPrimaryField()) {
            DelPrimaryField(DelRecId);
        }
        // delete record from indexes
        if (DataCacheP) {
            TMem CacheRecMem;
            DataCache.GetVal(DelRecId, CacheRecMem);
            RecIndexer.DeindexRec(CacheRecMem, DelRecId, *SerializatorCache);
        }
        if (DataMemP) {
            TMem MemRecMem;
            DataMem.GetVal(DelRecId, MemRecMem);
            RecIndexer.DeindexRec(MemRecMem, DelRecId, *SerializatorMem);
        }
        // delete record from joins
        TRec Rec(this, DelRecId);
        for (int JoinN = 0; JoinN < GetJoins(); JoinN++) {
            TJoinDesc JoinDesc = GetJoinDesc(JoinN);
            // execute the join
            PRecSet JoinRecSet = Rec.DoJoin(GetBase(), JoinDesc.GetJoinId());
            for (int JoinRecN = 0; JoinRecN < JoinRecSet->GetRecs(); JoinRecN++) {
                // remove joins with all matched records, one by one
                const uint64 JoinRecId = JoinRecSet->GetRecId(JoinRecN);
                DelJoin(JoinDesc.GetJoinId(), DelRecId, JoinRecId);
            }
        }
        // count what we deleted
        DeletedRecs++;
    }
    // delete records from disk
    if (DataCacheP) {
        DataCache.DelVals(DeletedRecs);
    }
    // delete records from in-memory store
    if (DataMemP) {
        DataMem.DelVals(DeletedRecs);
    }

    // report success :-)
    if (DelRecIdV.Len() > 1000) {
        TEnv::Logger->OnStatusFmt("  %s records at end", TUInt64::GetStr(GetRecs()).CStr());
    }
}

void TStoreImpl::BatchDeleteRecs(const TUInt64V& DelRecIdV, const TBatchDelProgressCb& OnProgress) {
    if (DelRecIdV.Empty()) { return; }

    TUInt64H RecIdSet(DelRecIdV.Len());
    for (int N = 0; N < DelRecIdV.Len(); N++) { RecIdSet.AddKey(DelRecIdV[N]); }

    // smallest record id that survives this delete: every index item below it references either
    // a record deleted right now or one already gone, so the gix scan can drop whole posting-list
    // children below it from their headers without reading them. The threshold is only as good
    // as the oldest survivor - a single very old record that is kept moves it to that record
    uint64 MinKeepRecId = TUInt64::Mx;
    { PStoreIter Iter = GetIter(); while (Iter->Next()) {
        const uint64 RecId = Iter->GetRecId();
        if (RecId < MinKeepRecId && !RecIdSet.IsKey(RecId)) { MinKeepRecId = RecId; }
    } }
    // no survivor means everything goes - TUInt64::Mx then correctly drops every child
    TEnv::Logger->OnStatusFmt("BatchDeleteRecs: smallest record id kept: %s",
        TUInt64::GetStr(MinKeepRecId).CStr());

    TIntSet KeyIdSet;
    RecIndexer.GetGixKeyIdSet(KeyIdSet);
    GetIndex()->BatchDeleteFromGix(KeyIdSet, RecIdSet, MinKeepRecId, OnProgress);

    // the store phase advances one requested record at a time; ids that are no longer valid are
    // skipped, so Done counts records looked at and Removed counts the ones actually deleted
    const TStr Phase = "5/5 Store";
    const int64 TotalRecs = DelRecIdV.Len();
    const int64 ReportEvery = TotalRecs / 200 + 1;
    int64 DeletedRecs = 0;
    if (OnProgress) { OnProgress(Phase, 0, TotalRecs, 0); }
    for (int N = 0; N < DelRecIdV.Len(); N++) {
        const uint64 DelRecId = DelRecIdV[N];
        if (IsRecId(DelRecId)) {
            OnDelete(DelRecId);
            if (IsPrimaryField()) { DelPrimaryField(DelRecId); }
            if (DataCacheP) {
                TMem RecMem; DataCache.GetVal(DelRecId, RecMem);
                RecIndexer.DeindexRecNonGix(RecMem, DelRecId, *SerializatorCache);
            }
            if (DataMemP) {
                TMem RecMem; DataMem.GetVal(DelRecId, RecMem);
                RecIndexer.DeindexRecNonGix(RecMem, DelRecId, *SerializatorMem);
            }
            TRec Rec(this, DelRecId);
            for (int JoinN = 0; JoinN < GetJoins(); JoinN++) {
                TJoinDesc JoinDesc = GetJoinDesc(JoinN);
                PRecSet JoinRecSet = Rec.DoJoin(GetBase(), JoinDesc.GetJoinId());
                for (int JoinRecN = 0; JoinRecN < JoinRecSet->GetRecs(); JoinRecN++) {
                    DelJoin(JoinDesc.GetJoinId(), DelRecId, JoinRecSet->GetRecId(JoinRecN));
                }
            }
            DeletedRecs++;
        }
        const int64 Done = N + 1;
        if (OnProgress && (Done % ReportEvery == 0 || Done == TotalRecs)) {
            OnProgress(Phase, Done, TotalRecs, DeletedRecs);
        }
    }
    if (DataCacheP) { DataCache.DelVals((int)DeletedRecs); }
    if (DataMemP)   { DataMem.DelVals((int)DeletedRecs); }
}

bool TStoreImpl::IsFieldNull(const uint64& RecId, const int& FieldId) const {
    TMem RecMem; GetRecMem(RecId, FieldId, RecMem);
    return GetFieldSerializator(FieldId)->IsFieldNull(RecMem, FieldId);
}

uchar TStoreImpl::GetFieldByte(const uint64& RecId, const int& FieldId) const {
    TMem RecMem; GetRecMem(RecId, FieldId, RecMem);
    return GetFieldSerializator(FieldId)->GetFieldByte(RecMem, FieldId);
}

int TStoreImpl::GetFieldInt(const uint64& RecId, const int& FieldId) const {
    TMem RecMem; GetRecMem(RecId, FieldId, RecMem);
    return GetFieldSerializator(FieldId)->GetFieldInt(RecMem, FieldId);
}

int16 TStoreImpl::GetFieldInt16(const uint64& RecId, const int& FieldId) const {
    TMem RecMem; GetRecMem(RecId, FieldId, RecMem);
    return GetFieldSerializator(FieldId)->GetFieldInt16(RecMem, FieldId);
}

int64 TStoreImpl::GetFieldInt64(const uint64& RecId, const int& FieldId) const {
    TMem RecMem; GetRecMem(RecId, FieldId, RecMem);
    return GetFieldSerializator(FieldId)->GetFieldInt64(RecMem, FieldId);
}

TStr TStoreImpl::GetFieldStr(const uint64& RecId, const int& FieldId) const {
    TMem RecMem; GetRecMem(RecId, FieldId, RecMem);
    return GetFieldSerializator(FieldId)->GetFieldStr(RecMem, FieldId);
}

bool TStoreImpl::GetFieldBool(const uint64& RecId, const int& FieldId) const {
    TMem RecMem; GetRecMem(RecId, FieldId, RecMem);
    return GetFieldSerializator(FieldId)->GetFieldBool(RecMem, FieldId);
}

double TStoreImpl::GetFieldFlt(const uint64& RecId, const int& FieldId) const {
    TMem RecMem; GetRecMem(RecId, FieldId, RecMem);
    return GetFieldSerializator(FieldId)->GetFieldFlt(RecMem, FieldId);
}

float TStoreImpl::GetFieldSFlt(const uint64& RecId, const int& FieldId) const {
    TMem RecMem; GetRecMem(RecId, FieldId, RecMem);
    return GetFieldSerializator(FieldId)->GetFieldSFlt(RecMem, FieldId);
}

TFltPr TStoreImpl::GetFieldFltPr(const uint64& RecId, const int& FieldId) const {
    TMem RecMem; GetRecMem(RecId, FieldId, RecMem);
    return GetFieldSerializator(FieldId)->GetFieldFltPr(RecMem, FieldId);
}

uint TStoreImpl::GetFieldUInt(const uint64& RecId, const int& FieldId) const {
    TMem RecMem; GetRecMem(RecId, FieldId, RecMem);
    return GetFieldSerializator(FieldId)->GetFieldUInt(RecMem, FieldId);
}

uint16 TStoreImpl::GetFieldUInt16(const uint64& RecId, const int& FieldId) const {
    TMem RecMem; GetRecMem(RecId, FieldId, RecMem);
    return GetFieldSerializator(FieldId)->GetFieldUInt16(RecMem, FieldId);
}

uint64 TStoreImpl::GetFieldUInt64(const uint64& RecId, const int& FieldId) const {
    TMem RecMem; GetRecMem(RecId, FieldId, RecMem);
    return GetFieldSerializator(FieldId)->GetFieldUInt64(RecMem, FieldId);
}

void TStoreImpl::GetFieldStrV(const uint64& RecId, const int& FieldId, TStrV& StrV) const {
    TMem RecMem; GetRecMem(RecId, FieldId, RecMem);
    GetFieldSerializator(FieldId)->GetFieldStrV(RecMem, FieldId, StrV);
}

void TStoreImpl::GetFieldIntV(const uint64& RecId, const int& FieldId, TIntV& IntV) const {
    TMem RecMem; GetRecMem(RecId, FieldId, RecMem);
    GetFieldSerializator(FieldId)->GetFieldIntV(RecMem, FieldId, IntV);
}

void TStoreImpl::GetFieldFltV(const uint64& RecId, const int& FieldId, TFltV& FltV) const {
    TMem RecMem; GetRecMem(RecId, FieldId, RecMem);
    GetFieldSerializator(FieldId)->GetFieldFltV(RecMem, FieldId, FltV);
}

void TStoreImpl::GetFieldTm(const uint64& RecId, const int& FieldId, TTm& Tm) const {
    TMem RecMem; GetRecMem(RecId, FieldId, RecMem);
    GetFieldSerializator(FieldId)->GetFieldTm(RecMem, FieldId, Tm);
}

uint64 TStoreImpl::GetFieldTmMSecs(const uint64& RecId, const int& FieldId) const {
    TMem RecMem; GetRecMem(RecId, FieldId, RecMem);
    return GetFieldSerializator(FieldId)->GetFieldTmMSecs(RecMem, FieldId);
}

void TStoreImpl::GetFieldNumSpV(const uint64& RecId, const int& FieldId, TIntFltKdV& SpV) const {
    TMem RecMem; GetRecMem(RecId, FieldId, RecMem);
    GetFieldSerializator(FieldId)->GetFieldNumSpV(RecMem, FieldId, SpV);
}

void TStoreImpl::GetFieldBowSpV(const uint64& RecId, const int& FieldId, PBowSpV& SpV) const {
    TMem RecMem; GetRecMem(RecId, FieldId, RecMem);
    GetFieldSerializator(FieldId)->GetFieldBowSpV(RecMem, FieldId, SpV);
}

void TStoreImpl::GetFieldTMem(const uint64& RecId, const int& FieldId, TMem& Mem) const {
    TMem RecMem; GetRecMem(RecId, FieldId, RecMem);
    GetFieldSerializator(FieldId)->GetFieldTMem(RecMem, FieldId, Mem);
}

PJsonVal TStoreImpl::GetFieldJsonVal(const uint64& RecId, const int& FieldId) const {
    TMem RecMem; GetRecMem(RecId, FieldId, RecMem);
    return GetFieldSerializator(FieldId)->GetFieldJsonVal(RecMem, FieldId);
}

void TStoreImpl::SetFieldNull(const uint64& RecId, const int& FieldId) {
    TMem InRecMem; GetRecMem(RecId, FieldId, InRecMem);
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    TMem OutRecMem; FieldSerializator->SetFieldNull(InRecMem, OutRecMem, FieldId);
    RecIndexer.UpdateRec(InRecMem, OutRecMem, RecId, FieldId, *FieldSerializator);
    PutRecMem(RecId, FieldId, OutRecMem);
}

void TStoreImpl::SetFieldByte(const uint64& RecId, const int& FieldId, const uchar& Byte) {
    TMem InRecMem; GetRecMem(RecId, FieldId, InRecMem);
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    TMem OutRecMem;
    FieldSerializator->SetFieldByte(InRecMem, OutRecMem, FieldId, Byte);
    RecIndexer.UpdateRec(InRecMem, OutRecMem, RecId, FieldId, *FieldSerializator);
    PutRecMem(RecId, FieldId, OutRecMem);
}

void TStoreImpl::SetFieldInt(const uint64& RecId, const int& FieldId, const int& Int) {
    // special case if field is primary field
    if (FieldId == PrimaryFieldId) {
        // it is, make sure new value does not exist yet
        if (PrimaryIntIdH.IsKey(Int) && PrimaryIntIdH.GetDat(Int) != RecId) {
            throw TQmExcept::New("[TStoreImpl::SetFieldInt] Primary key '" + TInt::GetStr(Int) +
                "' being set to field '" + GetFieldNm(FieldId) + "' already taken.");
        }
    }
    // replace the field value
    TMem InRecMem; GetRecMem(RecId, FieldId, InRecMem);
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    if (FieldId == PrimaryFieldId) {
        DelPrimaryFieldInt(RecId, FieldSerializator->GetFieldInt(InRecMem, FieldId));
    }
    TMem OutRecMem;
    FieldSerializator->SetFieldInt(InRecMem, OutRecMem, FieldId, Int);
    RecIndexer.UpdateRec(InRecMem, OutRecMem, RecId, FieldId, *FieldSerializator);
    PutRecMem(RecId, FieldId, OutRecMem);
    if (FieldId == PrimaryFieldId) { SetPrimaryFieldInt(RecId, Int); }
}

void TStoreImpl::SetFieldInt16(const uint64& RecId, const int& FieldId, const int16& Int16) {
    TMem InRecMem; GetRecMem(RecId, FieldId, InRecMem);
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    TMem OutRecMem;
    FieldSerializator->SetFieldInt16(InRecMem, OutRecMem, FieldId, Int16);
    RecIndexer.UpdateRec(InRecMem, OutRecMem, RecId, FieldId, *FieldSerializator);
    PutRecMem(RecId, FieldId, OutRecMem);
}

void TStoreImpl::SetFieldInt64(const uint64& RecId, const int& FieldId, const int64& Int64) {
    TMem InRecMem; GetRecMem(RecId, FieldId, InRecMem);
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    TMem OutRecMem;
    FieldSerializator->SetFieldInt64(InRecMem, OutRecMem, FieldId, Int64);
    RecIndexer.UpdateRec(InRecMem, OutRecMem, RecId, FieldId, *FieldSerializator);
    PutRecMem(RecId, FieldId, OutRecMem);
}

void TStoreImpl::SetFieldIntV(const uint64& RecId, const int& FieldId, const TIntV& IntV) {
    TMem InRecMem; GetRecMem(RecId, FieldId, InRecMem);
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    TMem OutRecMem;
    FieldSerializator->SetFieldIntV(InRecMem, OutRecMem, FieldId, IntV);
    RecIndexer.UpdateRec(InRecMem, OutRecMem, RecId, FieldId, *FieldSerializator);
    PutRecMem(RecId, FieldId, OutRecMem);
}

void TStoreImpl::SetFieldUInt(const uint64& RecId, const int& FieldId, const uint& UInt) {
    TMem InRecMem; GetRecMem(RecId, FieldId, InRecMem);
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    TMem OutRecMem;
    FieldSerializator->SetFieldUInt(InRecMem, OutRecMem, FieldId, UInt);
    RecIndexer.UpdateRec(InRecMem, OutRecMem, RecId, FieldId, *FieldSerializator);
    PutRecMem(RecId, FieldId, OutRecMem);
}

void TStoreImpl::SetFieldUInt16(const uint64& RecId, const int& FieldId, const uint16& UInt16) {
    TMem InRecMem; GetRecMem(RecId, FieldId, InRecMem);
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    TMem OutRecMem;
    FieldSerializator->SetFieldUInt16(InRecMem, OutRecMem, FieldId, UInt16);
    RecIndexer.UpdateRec(InRecMem, OutRecMem, RecId, FieldId, *FieldSerializator);
    PutRecMem(RecId, FieldId, OutRecMem);
}

void TStoreImpl::SetFieldUInt64(const uint64& RecId, const int& FieldId, const uint64& UInt64) {
    // special case if field is primary field
    if (FieldId == PrimaryFieldId) {
        // it is, make sure new value does not exist yet
        if (PrimaryUInt64IdH.IsKey(UInt64) && PrimaryUInt64IdH.GetDat(UInt64) != RecId) {
            throw TQmExcept::New("[TStoreImpl::SetFieldUInt64] Primary key '" + TUInt64::GetStr(UInt64) +
                "' being set to field '" + GetFieldNm(FieldId) + "' already taken.");
        }
    }
    // replace the field value
    TMem InRecMem; GetRecMem(RecId, FieldId, InRecMem);
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    if (FieldId == PrimaryFieldId) {
        DelPrimaryFieldUInt64(RecId, FieldSerializator->GetFieldUInt64(InRecMem, FieldId));
    }
    TMem OutRecMem;
    FieldSerializator->SetFieldUInt64(InRecMem, OutRecMem, FieldId, UInt64);
    RecIndexer.UpdateRec(InRecMem, OutRecMem, RecId, FieldId, *FieldSerializator);
    PutRecMem(RecId, FieldId, OutRecMem);
    if (FieldId == PrimaryFieldId) { SetPrimaryFieldUInt64(RecId, UInt64); }
}

void TStoreImpl::SetFieldStr(const uint64& RecId, const int& FieldId, const TStr& Str) {
    // special case if field is primary field
    if (FieldId == PrimaryFieldId) {
        // it is, make sure new value does not exist yet
        if (PrimaryStrIdH.IsKey(Str) && PrimaryStrIdH.GetDat(Str) != RecId) {
            throw TQmExcept::New("[TStoreImpl::SetFieldStr] Primary key '" + Str +
                "' being set to field '" + GetFieldNm(FieldId) + "' already taken.");
        }
    }
    // replace the field value
    TMem InRecMem; GetRecMem(RecId, FieldId, InRecMem);
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    if (FieldId == PrimaryFieldId) {
        DelPrimaryFieldStr(RecId, FieldSerializator->GetFieldStr(InRecMem, FieldId));
    }
    TMem OutRecMem;
    FieldSerializator->SetFieldStr(InRecMem, OutRecMem, FieldId, Str);
    RecIndexer.UpdateRec(InRecMem, OutRecMem, RecId, FieldId, *FieldSerializator);
    PutRecMem(RecId, FieldId, OutRecMem);
    if (FieldId == PrimaryFieldId) { SetPrimaryFieldStr(RecId, Str); }
}

void TStoreImpl::SetFieldStrV(const uint64& RecId, const int& FieldId, const TStrV& StrV) {
    TMem InRecMem; GetRecMem(RecId, FieldId, InRecMem);
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    TMem OutRecMem;
    FieldSerializator->SetFieldStrV(InRecMem, OutRecMem, FieldId, StrV);
    RecIndexer.UpdateRec(InRecMem, OutRecMem, RecId, FieldId, *FieldSerializator);
    PutRecMem(RecId, FieldId, OutRecMem);
}

void TStoreImpl::SetFieldBool(const uint64& RecId, const int& FieldId, const bool& Bool) {
    TMem InRecMem; GetRecMem(RecId, FieldId, InRecMem);
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    TMem OutRecMem;
    FieldSerializator->SetFieldBool(InRecMem, OutRecMem, FieldId, Bool);
    RecIndexer.UpdateRec(InRecMem, OutRecMem, RecId, FieldId, *FieldSerializator);
    PutRecMem(RecId, FieldId, OutRecMem);
}

void TStoreImpl::SetFieldFlt(const uint64& RecId, const int& FieldId, const double& Flt) {
    // special case if field is primary field
    if (FieldId == PrimaryFieldId) {
        // it is, make sure new value does not exist yet
        if (PrimaryFltIdH.IsKey(Flt) && PrimaryFltIdH.GetDat(Flt) != RecId) {
            throw TQmExcept::New("[TStoreImpl::SetFieldFlt] Primary key '" + TFlt::GetStr(Flt) +
                "' being set to field '" + GetFieldNm(FieldId) + "' already taken.");
        }
    }
    // replace the field value
    TMem InRecMem; GetRecMem(RecId, FieldId, InRecMem);
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    if (FieldId == PrimaryFieldId) {
        DelPrimaryFieldFlt(RecId, FieldSerializator->GetFieldFlt(InRecMem, FieldId));
    }
    TMem OutRecMem;
    FieldSerializator->SetFieldFlt(InRecMem, OutRecMem, FieldId, Flt);
    RecIndexer.UpdateRec(InRecMem, OutRecMem, RecId, FieldId, *FieldSerializator);
    PutRecMem(RecId, FieldId, OutRecMem);
    if (FieldId == PrimaryFieldId) { SetPrimaryFieldFlt(RecId, Flt); }
}
void TStoreImpl::SetFieldSFlt(const uint64& RecId, const int& FieldId, const float& SFlt) {
    TMem InRecMem; GetRecMem(RecId, FieldId, InRecMem);
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    TMem OutRecMem;
    FieldSerializator->SetFieldSFlt(InRecMem, OutRecMem, FieldId, SFlt);
    RecIndexer.UpdateRec(InRecMem, OutRecMem, RecId, FieldId, *FieldSerializator);
    PutRecMem(RecId, FieldId, OutRecMem);
}

void TStoreImpl::SetFieldFltPr(const uint64& RecId, const int& FieldId, const TFltPr& FltPr) {
    TMem InRecMem; GetRecMem(RecId, FieldId, InRecMem);
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    TMem OutRecMem;
    FieldSerializator->SetFieldFltPr(InRecMem, OutRecMem, FieldId, FltPr);
    RecIndexer.UpdateRec(InRecMem, OutRecMem, RecId, FieldId, *FieldSerializator);
    PutRecMem(RecId, FieldId, OutRecMem);
}

void TStoreImpl::SetFieldFltV(const uint64& RecId, const int& FieldId, const TFltV& FltV) {
    TMem InRecMem; GetRecMem(RecId, FieldId, InRecMem);
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    TMem OutRecMem;
    FieldSerializator->SetFieldFltV(InRecMem, OutRecMem, FieldId, FltV);
    RecIndexer.UpdateRec(InRecMem, OutRecMem, RecId, FieldId, *FieldSerializator);
    PutRecMem(RecId, FieldId, OutRecMem);
}

void TStoreImpl::SetFieldTm(const uint64& RecId, const int& FieldId, const TTm& Tm) {
    TMem InRecMem; GetRecMem(RecId, FieldId, InRecMem);
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    TMem OutRecMem;
    FieldSerializator->SetFieldTm(InRecMem, OutRecMem, FieldId, Tm);
    RecIndexer.UpdateRec(InRecMem, OutRecMem, RecId, FieldId, *FieldSerializator);
    PutRecMem(RecId, FieldId, OutRecMem);
}

void TStoreImpl::SetFieldTmMSecs(const uint64& RecId, const int& FieldId, const uint64& TmMSecs) {
    // special case if field is primary field
    if (FieldId == PrimaryFieldId) {
        // it is, make sure new value does not exist yet
        if (PrimaryTmMSecsIdH.IsKey(TmMSecs) && PrimaryTmMSecsIdH.GetDat(TmMSecs) != RecId) {
            throw TQmExcept::New("[TStoreImpl::SetFieldTmMSecs] Primary key '" + TUInt64::GetStr(TmMSecs) +
                "' being set to field '" + GetFieldNm(FieldId) + "' already taken.");
        }
    }
    // replace the field value
    TMem InRecMem; GetRecMem(RecId, FieldId, InRecMem);
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    if (FieldId == PrimaryFieldId) {
        DelPrimaryFieldMSecs(RecId, FieldSerializator->GetFieldTmMSecs(InRecMem, FieldId));
    }
    TMem OutRecMem;
    FieldSerializator->SetFieldTmMSecs(InRecMem, OutRecMem, FieldId, TmMSecs);
    RecIndexer.UpdateRec(InRecMem, OutRecMem, RecId, FieldId, *FieldSerializator);
    PutRecMem(RecId, FieldId, OutRecMem);
    if (FieldId == PrimaryFieldId) { SetPrimaryFieldMSecs(RecId, TmMSecs); }
}

void TStoreImpl::SetFieldNumSpV(const uint64& RecId, const int& FieldId, const TIntFltKdV& SpV) {
    TMem InRecMem; GetRecMem(RecId, FieldId, InRecMem);
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    TMem OutRecMem;
    FieldSerializator->SetFieldNumSpV(InRecMem, OutRecMem, FieldId, SpV);
    RecIndexer.UpdateRec(InRecMem, OutRecMem, RecId, FieldId, *FieldSerializator);
    PutRecMem(RecId, FieldId, OutRecMem);
}

void TStoreImpl::SetFieldBowSpV(const uint64& RecId, const int& FieldId, const PBowSpV& SpV) {
    TMem InRecMem; GetRecMem(RecId, FieldId, InRecMem);
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    TMem OutRecMem;
    FieldSerializator->SetFieldBowSpV(InRecMem, OutRecMem, FieldId, SpV);
    RecIndexer.UpdateRec(InRecMem, OutRecMem, RecId, FieldId, *FieldSerializator);
    PutRecMem(RecId, FieldId, OutRecMem);
}

void TStoreImpl::SetFieldTMem(const uint64& RecId, const int& FieldId, const TMem& Mem) {
    TMem InRecMem; GetRecMem(RecId, FieldId, InRecMem);
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    TMem OutRecMem;
    FieldSerializator->SetFieldTMem(InRecMem, OutRecMem, FieldId, Mem);
    RecIndexer.UpdateRec(InRecMem, OutRecMem, RecId, FieldId, *FieldSerializator);
    PutRecMem(RecId, FieldId, OutRecMem);
}

void TStoreImpl::SetFieldJsonVal(const uint64& RecId, const int& FieldId, const PJsonVal& Json) {
    TMem InRecMem; GetRecMem(RecId, FieldId, InRecMem);
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    TMem OutRecMem;
    FieldSerializator->SetFieldJsonVal(InRecMem, OutRecMem, FieldId, Json);
    RecIndexer.UpdateRec(InRecMem, OutRecMem, RecId, FieldId, *FieldSerializator);
    PutRecMem(RecId, FieldId, OutRecMem);
}

PJsonVal TStoreImpl::GetStoreJson(const TWPt<TBase>& Base) const {
    PJsonVal Result = TStore::GetStoreJson(Base);

    if (WndDesc.WindowType != TStoreWndType::swtNone) {
        PJsonVal WindowJson = TJsonVal::NewObj();

        WindowJson->AddToObj("type", WndDesc.WindowType == TStoreWndType::swtLength ? "length" : "time");
        WindowJson->AddToObj("size", (int) WndDesc.WindowSize);

        if (WndDesc.WindowType == TStoreWndType::swtTime) {
            WindowJson->AddToObj("timeField", WndDesc.TimeFieldNm);
        }

        Result->AddToObj("window", WindowJson);
    }

    return Result;
}

int TStoreImpl::GetCodebookId(const int& FieldId, const TStr& Str) const {
    const TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    return FieldSerializator->GetCodebookId(FieldId, Str);
}

int TStoreImpl::PartialFlush(int WndInMsec) {
    int slice = WndInMsec / 2;
    TTmStopWatch sw(true);
    int res = DataMem.PartialFlush(slice);
    int res2 = DataCache.PartialFlush(slice);
    return res + res2;
}

PJsonVal TStoreImpl::GetStats() {
    PJsonVal res = TJsonVal::NewObj();
    res->AddToObj("name", GetStoreNm());
    res->AddToObj("blob_storage_memory", BlobBsStatsToJson(DataMem.GetBlobBsStats()));
    res->AddToObj("blob_storage_cache", BlobBsStatsToJson(DataCache.GetBlobBsStats()));
    return res;
}

/// Run verification for whole store
void TStoreImpl::RunVerification() {
    // do nothing for now
}

/// Run verification for single record
void TStoreImpl::RunVerificationForRecord(const uint64& RecId) {
    // do nothing fo rnow
}

///////////////////////////////
/// TStorePbBlob

template <class TRecPtMap>
uint64 TStorePbBlobT<TRecPtMap>::AddRec(const PJsonVal& RecVal, const bool& TriggerEvents) {// check if we are given reference to existing record
    try {
        // parse out record id, if referred directly
        {
            const uint64 RecId = TStore::GetRecId(RecVal);
            if (IsRecId(RecId)) {
                // check if we have anything more than record identifier, which would require calling UpdateRec
                if (RecVal->GetObjKeys() > 1) { UpdateRec(RecId, RecVal); }
                // return named record
                return RecId;
            }
        }
        // check if we have a primary field
        if (IsPrimaryField()) {
            uint64 PrimaryRecId = TUInt64::Mx;
            // primary field cannot be nullable, so we must have it
            const TStr& PrimaryField = GetFieldNm(PrimaryFieldId);
            QmAssertR(RecVal->IsObjKey(PrimaryField), "Missing primary field in the record: " + PrimaryField);
            // parse based on the field type
            if (PrimaryFieldType == oftStr) {
                TStr FieldVal = RecVal->GetObjStr(PrimaryField);
                if (PrimaryStrIdH.IsKey(FieldVal)) {
                    PrimaryRecId = PrimaryStrIdH.GetDat(FieldVal);
                }
            } else if (PrimaryFieldType == oftInt) {
                const int FieldVal = RecVal->GetObjInt(PrimaryField);
                if (PrimaryIntIdH.IsKey(FieldVal)) {
                    PrimaryRecId = PrimaryIntIdH.GetDat(FieldVal);
                }
            } else if (PrimaryFieldType == oftUInt64) {
                const uint64 FieldVal = RecVal->GetObjUInt64(PrimaryField);
                if (PrimaryUInt64IdH.IsKey(FieldVal)) {
                    PrimaryRecId = PrimaryUInt64IdH.GetDat(FieldVal);
                }
            } else if (PrimaryFieldType == oftFlt) {
                const double FieldVal = RecVal->GetObjNum(PrimaryField);
                if (PrimaryFltIdH.IsKey(FieldVal)) {
                    PrimaryRecId = PrimaryFltIdH.GetDat(FieldVal);
                }
            } else if (PrimaryFieldType == oftTm) {
                TStr TmStr = RecVal->GetObjStr(PrimaryField);
                TTm Tm = TTm::GetTmFromWebLogDateTimeStr(TmStr, '-', ':', '.', 'T');
                const uint64 FieldVal = TTm::GetMSecsFromTm(Tm);
                if (PrimaryTmMSecsIdH.IsKey(FieldVal)) {
                    PrimaryRecId = PrimaryTmMSecsIdH.GetDat(FieldVal);
                }
            } else {
                EAssertR(false, "Unsupported primary-field type");
            }
            // check if we found primary field with existing value
            if (PrimaryRecId != TUInt64::Mx) {
                // check if we have anything more than primary field, which would require redirect to UpdateRec
                if (RecVal->GetObjKeys() > 1) { UpdateRec(PrimaryRecId, RecVal); }
                // return id of named record
                return PrimaryRecId;
            }
        }
    } catch (const PExcept& Except) {
        // error parsing, report error and return nothing
        ErrorLog("[TStoreImpl::AddRec] Error parsing out reference to existing record:");
        ErrorLog(Except->GetMsgStr());
        return TUInt64::Mx;
    }

    // always add system field that means "inserted_at"
    RecVal->AddToObj(TStoreWndDesc::SysInsertedAtFieldName, TTm::GetCurUniTm().GetStr());

    // for storing record id
    TPgBlobPt CacheRecId;
    TPgBlobPt MemRecId;
    uint64 RecId = RecIdCounter++;
    MetaDirtyP = true;
    // store to disk storage
    if (DataBlobP) {
        TMem CacheRecMem;
        SerializatorCache->Serialize(RecVal, CacheRecMem, this);
        TPgBlobPt Pt = DataBlob->Put(CacheRecMem.GetBf(), CacheRecMem.Len());
        CacheRecId = Pt;
        RecIdBlobPtH.AddDat(RecId, Pt);
        // index new record
        RecIndexer.IndexRec(CacheRecMem, RecId, *SerializatorCache, RecVal);
    }
    // store to in-memory storage
    if (DataMemP) {
        TMem MemRecMem;
        SerializatorMem->Serialize(RecVal, MemRecMem, this);
        TPgBlobPt Pt = DataMem->Put(MemRecMem.GetBf(), MemRecMem.Len());
        MemRecId = Pt;
        RecIdBlobPtHMem.AddDat(RecId, Pt);
        RecIndexer.IndexRec(MemRecMem, RecId, *SerializatorMem, RecVal);
    }
    // make sure we are consistent with respect to Ids!
    if (DataBlobP && DataMemP) {
        EAssert(RecId == RecIdCounter - 1);
    }

    // remember value-recordId map when primary field available
    if (IsPrimaryField()) { SetPrimaryField(RecId); }

    // insert nested join records
    AddJoinRec(RecId, RecVal);
    // call add triggers
    if (TriggerEvents) {
        OnAdd(RecId);
    }

    // return record Id of the new record
    return RecId;
}

/// Update existing record
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::UpdateRec(const uint64& RecId, const PJsonVal& RecVal) {
    // figure out which storage fields are affected
    bool CacheP = false, MemP = false, PrimaryP = false;
    bool CacheVarP = false, MemVarP = false, KeyP = false;
    for (int FieldId = 0; FieldId < GetFields(); FieldId++) {
        // check if field appears in the record JSon
        TStr FieldNm = GetFieldNm(FieldId);
        if (RecVal->IsObjKey(FieldNm)) {
            CacheP = CacheP || (FieldLocV[FieldId] == slDisk);
            MemP = MemP || (FieldLocV[FieldId] == slMemory);
            PrimaryP = PrimaryP || (FieldId == PrimaryFieldId);
            TFieldDesc fd = GetFieldDesc(FieldId);
            switch (fd.GetFieldType()) {
            case TFieldType::oftBowSpV:
            case TFieldType::oftTMem:
            case TFieldType::oftJson:
            case TFieldType::oftFltV:
            case TFieldType::oftIntV:
            case TFieldType::oftNumSpV:
            case TFieldType::oftStrV:
                // variable length
                CacheVarP = CacheVarP || (FieldLocV[FieldId] == slDisk);
                MemVarP = MemVarP || (FieldLocV[FieldId] == slMemory);
                break;
            case TFieldType::oftStr:
                // variable length
                CacheVarP = CacheVarP ||
                    (FieldLocV[FieldId] == slDisk && !SerializatorCache->IsInFixedPart(FieldId));
                MemVarP = MemVarP ||
                    (FieldLocV[FieldId] == slMemory  && !SerializatorMem->IsInFixedPart(FieldId));
                break;
            default:
                break;
            }
            KeyP = KeyP || RecIndexer.IsFieldIndexKey(FieldId);
        }
    }
    // remove old primary field
    if (PrimaryP) { DelPrimaryField(RecId); }
    // update disk serialization when necessary
    if (CacheP) {
        // update serialization
        TPgBlobPt Pt = RecIdBlobPtH.GetDat(RecId);
        TThinMIn MIn = DataBlob->Get(Pt);

        TIntSet CacheChangedFieldIdSet;
        if (CacheVarP || KeyP) {
            // variable fields changed, so we need to serialize whole record
            TMem CacheNewRecMem;
            TIntSet CacheChangedFieldIdSet;
            TMemBase CacheOldRecMem = MIn.GetMemBase();

            SerializatorCache->SerializeUpdate(RecVal, CacheOldRecMem,
                CacheNewRecMem, this, CacheChangedFieldIdSet);

            // update indexes pointing to the record
            // NOTE: we have update rec here before calling Put, since it overrides the original data
            RecIndexer.UpdateRec(CacheOldRecMem, CacheNewRecMem, RecId,
                CacheChangedFieldIdSet, *SerializatorCache);

            // update the stored serializations with new values
            Pt = DataBlob->Put(CacheNewRecMem.GetBf(), CacheNewRecMem.Len(), Pt);
            RecIdBlobPtH.AddDat(RecId, Pt);
            // the record may have moved to another page - the persisted rec-id to
            // blob-pointer map must be rewritten on close or it would keep pointing
            // at the old, freed location
            MetaDirtyP = true;
        } else {
            // nice, all changes can be done in-place, no index changes
            SerializatorCache->SerializeUpdateInPlace(RecVal, MIn, this,
                CacheChangedFieldIdSet);
            DataBlob->SetDirty(Pt);
        }
    }
    // update in-memory serialization when necessary
    if (MemP) {
        // update serialization
        TPgBlobPt Pt = RecIdBlobPtHMem.GetDat(RecId);
        TThinMIn MIn = DataMem->Get(Pt);

        TIntSet ChangedFieldIdSet;
        if (MemVarP || KeyP) {
            // variable fields changed, so we need to serialize whole record
            TMem NewRecMem;
            TIntSet ChangedFieldIdSet;
            TMemBase OldRecMem = MIn.GetMemBase();

            SerializatorMem->SerializeUpdate(RecVal, OldRecMem,
                NewRecMem, this, ChangedFieldIdSet);

            // update indexes pointing to the record
            // NOTE: we have update rec here before calling Put, since it overrides the original data
            RecIndexer.UpdateRec(OldRecMem, NewRecMem, RecId,
                ChangedFieldIdSet, *SerializatorMem);

            // update the stored serializations with new values
            Pt = DataMem->Put(NewRecMem.GetBf(), NewRecMem.Len(), Pt);
            RecIdBlobPtHMem.AddDat(RecId, Pt);
            // same as above - a moved record invalidates the persisted pointer map
            MetaDirtyP = true;
        } else {
            // nice, all changes can be done in-place, no index changes
            SerializatorMem->SerializeUpdateInPlace(RecVal, MIn, this,
                ChangedFieldIdSet);
            DataMem->SetDirty(Pt);
        }
    }
    // check if primary key changed and update the mapping
    if (PrimaryP) { SetPrimaryField(RecId); }
    // call update triggers
    OnUpdate(RecId);
}

//////////////////////////////////////////////////////////

/// Load page with with given record and return pointer to it
template <class TRecPtMap>
TThinMIn TStorePbBlobT<TRecPtMap>::GetPgBf(const uint64& RecId, const bool& UseMem) const {
    if (UseMem) {
        const TPgBlobPt& PgPt = RecIdBlobPtHMem.GetDat(RecId);
        TThinMIn min = DataMem->Get(PgPt);
        return min;
    } else {
        const TPgBlobPt& PgPt = RecIdBlobPtH.GetDat(RecId);
        TThinMIn min = DataBlob->Get(PgPt);
        return min;
    }
}

/// Get serializator for given location
template <class TRecPtMap>
TRecSerializator* TStorePbBlobT<TRecPtMap>::GetSerializator(const TStoreLoc& StoreLoc) {
    return (StoreLoc == TStoreLoc::slDisk ? SerializatorCache : SerializatorMem);
}

template <class TRecPtMap>
const TRecSerializator* TStorePbBlobT<TRecPtMap>::GetSerializator(const TStoreLoc& StoreLoc) const {
    return (StoreLoc == TStoreLoc::slDisk ? SerializatorCache : SerializatorMem);
}

template <class TRecPtMap>
TRecSerializator* TStorePbBlobT<TRecPtMap>::GetFieldSerializator(const int &FieldId) {
    return GetSerializator(FieldLocV[FieldId]);
}

template <class TRecPtMap>
const TRecSerializator* TStorePbBlobT<TRecPtMap>::GetFieldSerializator(const int &FieldId) const {
    return GetSerializator(FieldLocV[FieldId]);
}

template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::SetPrimaryFieldStr(const uint64& RecId, const TStr& Str) {
    PrimaryStrIdH.AddDat(Str) = RecId;
    MetaDirtyP = true;
}

template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::SetPrimaryFieldInt(const uint64& RecId, const int& Int) {
    PrimaryIntIdH.AddDat(Int) = RecId;
    MetaDirtyP = true;
}

template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::SetPrimaryFieldUInt64(const uint64& RecId, const uint64& UInt64) {
    PrimaryUInt64IdH.AddDat(UInt64) = RecId;
    MetaDirtyP = true;
}

template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::SetPrimaryFieldFlt(const uint64& RecId, const double& Flt) {
    PrimaryFltIdH.AddDat(Flt) = RecId;
    MetaDirtyP = true;
}

template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::SetPrimaryFieldMSecs(const uint64& RecId, const uint64& MSecs) {
    PrimaryTmMSecsIdH.AddDat(MSecs) = RecId;
    MetaDirtyP = true;
}

template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::DelPrimaryFieldStr(const uint64& RecId, const TStr& Str) {
    Assert(PrimaryStrIdH.GetDat(Str) == RecId);
    PrimaryStrIdH.DelIfKey(Str);
    MetaDirtyP = true;
}

template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::DelPrimaryFieldInt(const uint64& RecId, const int& Int) {
    Assert(PrimaryIntIdH.GetDat(Int) == RecId);
    PrimaryIntIdH.DelIfKey(Int);
    MetaDirtyP = true;
}

template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::DelPrimaryFieldUInt64(const uint64& RecId, const uint64& UInt64) {
    Assert(PrimaryUInt64IdH.GetDat(UInt64) == RecId);
    PrimaryUInt64IdH.DelIfKey(UInt64);
    MetaDirtyP = true;
}

template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::DelPrimaryFieldFlt(const uint64& RecId, const double& Flt) {
    Assert(PrimaryFltIdH.GetDat(Flt) == RecId);
    PrimaryFltIdH.DelIfKey(Flt);
    MetaDirtyP = true;
}

template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::DelPrimaryFieldMSecs(const uint64& RecId, const uint64& MSecs) {
    Assert(PrimaryTmMSecsIdH.GetDat(MSecs) == RecId);
    PrimaryTmMSecsIdH.DelIfKey(MSecs);
    MetaDirtyP = true;
}

/// Check if the value of given field for a given record is NULL
template <class TRecPtMap>
bool TStorePbBlobT<TRecPtMap>::IsFieldNull(const uint64& RecId, const int& FieldId) const {
    TThinMIn MIn = GetPgBf(RecId, FieldLocV[FieldId] != TStoreLoc::slDisk);
    return GetSerializator(FieldLocV[FieldId])->IsFieldNull(MIn, FieldId);
}
/// Get field value using field id (default implementation throws exception)
template <class TRecPtMap>
uchar TStorePbBlobT<TRecPtMap>::GetFieldByte(const uint64& RecId, const int& FieldId) const {
    TThinMIn MIn = GetPgBf(RecId, FieldLocV[FieldId] != TStoreLoc::slDisk);
    return GetSerializator(FieldLocV[FieldId])->GetFieldByte(MIn, FieldId);
}
/// Get field value using field id (default implementation throws exception)
template <class TRecPtMap>
int TStorePbBlobT<TRecPtMap>::GetFieldInt(const uint64& RecId, const int& FieldId) const {
    TThinMIn MIn = GetPgBf(RecId, FieldLocV[FieldId] != TStoreLoc::slDisk);
    return GetSerializator(FieldLocV[FieldId])->GetFieldInt(MIn, FieldId);
}
/// Get field value using field id (default implementation throws exception)
template <class TRecPtMap>
int16 TStorePbBlobT<TRecPtMap>::GetFieldInt16(const uint64& RecId, const int& FieldId) const {
    TThinMIn MIn = GetPgBf(RecId, FieldLocV[FieldId] != TStoreLoc::slDisk);
    return GetSerializator(FieldLocV[FieldId])->GetFieldInt16(MIn, FieldId);
}
/// Get field value using field id (default implementation throws exception)
template <class TRecPtMap>
int64 TStorePbBlobT<TRecPtMap>::GetFieldInt64(const uint64& RecId, const int& FieldId) const {
    TThinMIn MIn = GetPgBf(RecId, FieldLocV[FieldId] != TStoreLoc::slDisk);
    return GetSerializator(FieldLocV[FieldId])->GetFieldInt64(MIn, FieldId);
}
/// Get field value using field id (default implementation throws exception)
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::GetFieldIntV(const uint64& RecId, const int& FieldId, TIntV& IntV) const {
    TThinMIn MIn = GetPgBf(RecId, FieldLocV[FieldId] != TStoreLoc::slDisk);
    GetSerializator(FieldLocV[FieldId])->GetFieldIntV(MIn, FieldId, IntV);
}
/// Get field value using field id (default implementation throws exception)
template <class TRecPtMap>
uint TStorePbBlobT<TRecPtMap>::GetFieldUInt(const uint64& RecId, const int& FieldId) const {
    TThinMIn MIn = GetPgBf(RecId, FieldLocV[FieldId] != TStoreLoc::slDisk);
    return GetSerializator(FieldLocV[FieldId])->GetFieldUInt(MIn, FieldId);
}
/// Get field value using field id (default implementation throws exception)
template <class TRecPtMap>
uint16 TStorePbBlobT<TRecPtMap>::GetFieldUInt16(const uint64& RecId, const int& FieldId) const {
    TThinMIn MIn = GetPgBf(RecId, FieldLocV[FieldId] != TStoreLoc::slDisk);
    return GetSerializator(FieldLocV[FieldId])->GetFieldUInt16(MIn, FieldId);
}
/// Get field value using field id (default implementation throws exception)
template <class TRecPtMap>
uint64 TStorePbBlobT<TRecPtMap>::GetFieldUInt64(const uint64& RecId, const int& FieldId) const {
    TThinMIn MIn = GetPgBf(RecId, FieldLocV[FieldId] != TStoreLoc::slDisk);
    return GetSerializator(FieldLocV[FieldId])->GetFieldUInt64(MIn, FieldId);
}
/// Get field value using field id (default implementation throws exception)
template <class TRecPtMap>
TStr TStorePbBlobT<TRecPtMap>::GetFieldStr(const uint64& RecId, const int& FieldId) const {
    TThinMIn MIn = GetPgBf(RecId, FieldLocV[FieldId] != TStoreLoc::slDisk);
    return GetSerializator(FieldLocV[FieldId])->GetFieldStr(MIn, FieldId);
}
/// Get field value using field id (default implementation throws exception)
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::GetFieldStrV(const uint64& RecId, const int& FieldId, TStrV& StrV) const {
    TThinMIn MIn = GetPgBf(RecId, FieldLocV[FieldId] != TStoreLoc::slDisk);
    GetSerializator(FieldLocV[FieldId])->GetFieldStrV(MIn, FieldId, StrV);
}
/// Get field value using field id (default implementation throws exception)
template <class TRecPtMap>
bool TStorePbBlobT<TRecPtMap>::GetFieldBool(const uint64& RecId, const int& FieldId) const {
    TThinMIn MIn = GetPgBf(RecId, FieldLocV[FieldId] != TStoreLoc::slDisk);
    return GetSerializator(FieldLocV[FieldId])->GetFieldBool(MIn, FieldId);
}
/// Get field value using field id (default implementation throws exception)
template <class TRecPtMap>
double TStorePbBlobT<TRecPtMap>::GetFieldFlt(const uint64& RecId, const int& FieldId) const {
    TThinMIn MIn = GetPgBf(RecId, FieldLocV[FieldId] != TStoreLoc::slDisk);
    return GetSerializator(FieldLocV[FieldId])->GetFieldFlt(MIn, FieldId);
}
/// Get field value using field id (default implementation throws exception)
template <class TRecPtMap>
float TStorePbBlobT<TRecPtMap>::GetFieldSFlt(const uint64& RecId, const int& FieldId) const {
    TThinMIn MIn = GetPgBf(RecId, FieldLocV[FieldId] != TStoreLoc::slDisk);
    return GetSerializator(FieldLocV[FieldId])->GetFieldSFlt(MIn, FieldId);
}
/// Get field value using field id (default implementation throws exception)
template <class TRecPtMap>
TFltPr TStorePbBlobT<TRecPtMap>::GetFieldFltPr(const uint64& RecId, const int& FieldId) const {
    TThinMIn MIn = GetPgBf(RecId, FieldLocV[FieldId] != TStoreLoc::slDisk);
    return GetSerializator(FieldLocV[FieldId])->GetFieldFltPr(MIn, FieldId);
}
/// Get field value using field id (default implementation throws exception)
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::GetFieldFltV(const uint64& RecId, const int& FieldId, TFltV& FltV) const {
    TThinMIn MIn = GetPgBf(RecId, FieldLocV[FieldId] != TStoreLoc::slDisk);
    GetSerializator(FieldLocV[FieldId])->GetFieldFltV(MIn, FieldId, FltV);
}
/// Get field value using field id (default implementation throws exception)
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::GetFieldTm(const uint64& RecId, const int& FieldId, TTm& Tm) const {
    TThinMIn MIn = GetPgBf(RecId, FieldLocV[FieldId] != TStoreLoc::slDisk);
    GetSerializator(FieldLocV[FieldId])->GetFieldTm(MIn, FieldId, Tm);
}
/// Get field value using field id (default implementation throws exception)
template <class TRecPtMap>
uint64 TStorePbBlobT<TRecPtMap>::GetFieldTmMSecs(const uint64& RecId, const int& FieldId) const {
    TThinMIn MIn = GetPgBf(RecId, FieldLocV[FieldId] != TStoreLoc::slDisk);
    return GetSerializator(FieldLocV[FieldId])->GetFieldTmMSecs(MIn, FieldId);
}
/// Get field value using field id (default implementation throws exception)
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::GetFieldNumSpV(const uint64& RecId, const int& FieldId, TIntFltKdV& SpV) const {
    TThinMIn MIn = GetPgBf(RecId, FieldLocV[FieldId] != TStoreLoc::slDisk);
    GetSerializator(FieldLocV[FieldId])->GetFieldNumSpV(MIn, FieldId, SpV);
}
/// Get field value using field id (default implementation throws exception)
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::GetFieldBowSpV(const uint64& RecId, const int& FieldId, PBowSpV& SpV) const {
    TThinMIn MIn = GetPgBf(RecId, FieldLocV[FieldId] != TStoreLoc::slDisk);
    GetSerializator(FieldLocV[FieldId])->GetFieldBowSpV(MIn, FieldId, SpV);
}
/// Get field value using field id (default implementation throws exception)
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::GetFieldTMem(const uint64& RecId, const int& FieldId, TMem& Mem) const {
    TThinMIn MIn = GetPgBf(RecId, FieldLocV[FieldId] != TStoreLoc::slDisk);
    GetSerializator(FieldLocV[FieldId])->GetFieldTMem(MIn, FieldId, Mem);
}
/// Get field value using field id (default implementation throws exception)
template <class TRecPtMap>
PJsonVal TStorePbBlobT<TRecPtMap>::GetFieldJsonVal(const uint64& RecId, const int& FieldId) const {
    TThinMIn MIn = GetPgBf(RecId, FieldLocV[FieldId] != TStoreLoc::slDisk);
    return GetSerializator(FieldLocV[FieldId])->GetFieldJsonVal(MIn, FieldId);
}

//////////////////////

template <class TRecPtMap>
TThinMIn TStorePbBlobT<TRecPtMap>::GetEditableField(const uint64& RecId, const int& FieldId) {
    if (FieldLocV[FieldId] == TStoreLoc::slDisk) {
        TPgBlobPt& PgPt = RecIdBlobPtH.GetDat(RecId);
        DataBlob->SetDirty(PgPt);
        return DataBlob->Get(PgPt);
    }
    else {
        TPgBlobPt& PgPt = RecIdBlobPtHMem.GetDat(RecId);
        DataMem->SetDirty(PgPt);
        return DataMem->Get(PgPt);
    }
}

template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::GetRecData(const uint64& RecId, const int& FieldId, TMem& Mem, TRecPtMap* &RecIdBlobPtr, PPgBlob& Blob, TPgBlobPt* &PgPt)
{
    // callers (variable-length field setters) may re-point the returned hash
    // entry to a new blob location, which changes the persisted metadata
    MetaDirtyP = true;
    TMemBase MemInternal;
    if (FieldLocV[FieldId] == TStoreLoc::slDisk) {
        Blob = DataBlob;
        RecIdBlobPtr = &RecIdBlobPtH;
        PgPt = &RecIdBlobPtH.GetDat(RecId);
        MemInternal = DataBlob->GetMemBase(*PgPt);
    }
    else {
        Blob = DataMem;
        RecIdBlobPtr = &RecIdBlobPtHMem;
        PgPt = &RecIdBlobPtHMem.GetDat(RecId);
        MemInternal = DataMem->GetMemBase(*PgPt);
    }
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    if (FieldSerializator->GetUseToast()) {
        Mem.Copy(MemInternal);
    }
    else {
        Mem = MemInternal;
    }
}

/// Set the value of given field to NULL
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::SetFieldNull(const uint64& RecId, const int& FieldId) {
    // get the memory containig the field for the record
    TThinMIn min = GetEditableField(RecId, FieldId);

    // first deindex the old value
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    if (RecIndexer.HasIndexKey(FieldId) && !IsFieldNull(RecId, FieldId)) {
        RecIndexer.DeindexRecField(min.GetMemBase(), RecId, FieldId, *FieldSerializator);
    }

    FieldSerializator->SetFieldNull(min.GetBfAddrChar(), min.Len(), FieldId, true);
}
/// Set field value using field id (default implementation throws exception)
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::SetFieldByte(const uint64& RecId, const int& FieldId, const uchar& Byte) {
    // get the memory containig the field for the record
    TThinMIn min = GetEditableField(RecId, FieldId);

    // if we are indexing the field and the value is nonnull, first deindex the old value
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    if (RecIndexer.HasIndexKey(FieldId) && !IsFieldNull(RecId, FieldId)) {
        RecIndexer.DeindexRecField(min.GetMemBase(), RecId, FieldId, *FieldSerializator);
    }

    // set new value
    FieldSerializator->SetFieldByte(min.GetBfAddrChar(), min.Len(), FieldId, Byte);

    // index the new value in the updated memory buffer
    RecIndexer.IndexRecField(min.GetMemBase(), RecId, FieldId, *FieldSerializator);

}
/// Set field value using field id (default implementation throws exception)
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::SetFieldInt(const uint64& RecId, const int& FieldId, const int& Int) {
    // special case if field is primary field
    if (FieldId == PrimaryFieldId) {
        // it is, make sure new value does not exist yet
        if (PrimaryIntIdH.IsKey(Int) && PrimaryIntIdH.GetDat(Int) != RecId) {
            throw TQmExcept::New("[TStorePbBlob::SetFieldInt] Primary key '" + TInt::GetStr(Int) +
                "' being set to field '" + GetFieldNm(FieldId) + "' already taken.");
        }
    }
    TThinMIn min = GetEditableField(RecId, FieldId);

    // if we are indexing the field and the value is nonnull, first deindex the old value
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    if (RecIndexer.HasIndexKey(FieldId) && !IsFieldNull(RecId, FieldId)) {
        RecIndexer.DeindexRecField(min.GetMemBase(), RecId, FieldId, *FieldSerializator);
    }
    if (FieldId == PrimaryFieldId) { DelPrimaryFieldInt(RecId, FieldSerializator->GetFieldInt(min, FieldId)); }

    // set new value
    FieldSerializator->SetFieldInt(min.GetBfAddrChar(), min.Len(), FieldId, Int);

    // index the new value in the updated memory buffer
    RecIndexer.IndexRecField(min.GetMemBase(), RecId, FieldId, *FieldSerializator);
    if (FieldId == PrimaryFieldId) { SetPrimaryFieldInt(RecId, Int); }
}
/// Set field value using field id (default implementation throws exception)
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::SetFieldInt16(const uint64& RecId, const int& FieldId, const int16& Int16) {
    // get the memory containig the field for the record
    TThinMIn min = GetEditableField(RecId, FieldId);

    // if we are indexing the field and the value is nonnull, first deindex the old value
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    if (RecIndexer.HasIndexKey(FieldId) && !IsFieldNull(RecId, FieldId)) {
        RecIndexer.DeindexRecField(min.GetMemBase(), RecId, FieldId, *FieldSerializator);
    }

    // set new value
    FieldSerializator->SetFieldInt16(min.GetBfAddrChar(), min.Len(), FieldId, Int16);

    // index the new value in the updated memory buffer
    RecIndexer.IndexRecField(min.GetMemBase(), RecId, FieldId, *FieldSerializator);
}
/// Set field value using field id (default implementation throws exception)
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::SetFieldInt64(const uint64& RecId, const int& FieldId, const int64& Int64) {
    // get the memory containig the field for the record
    TThinMIn min = GetEditableField(RecId, FieldId);

    // if we are indexing the field and the value is nonnull, first deindex the old value
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    if (RecIndexer.HasIndexKey(FieldId) && !IsFieldNull(RecId, FieldId)) {
        RecIndexer.DeindexRecField(min.GetMemBase(), RecId, FieldId, *FieldSerializator);
    }

    // set new value
    FieldSerializator->SetFieldInt64(min.GetBfAddrChar(), min.Len(), FieldId, Int64);

    // index the new value in the updated memory buffer
    RecIndexer.IndexRecField(min.GetMemBase(), RecId, FieldId, *FieldSerializator);
}
/// Set field value using field id (default implementation throws exception)
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::SetFieldIntV(const uint64& RecId, const int& FieldId, const TIntV& IntV) {
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    TRecPtMap* RecIdBlobPtr = NULL;
    PPgBlob Blob; TPgBlobPt* PgPt = NULL;
    TMem mem_in, mem_out;
    GetRecData(RecId, FieldId, mem_in, RecIdBlobPtr, Blob, PgPt);
    // deindex
    if (RecIndexer.HasIndexKey(FieldId) && !IsFieldNull(RecId, FieldId)) {
        RecIndexer.DeindexRecField(mem_in, RecId, FieldId, *FieldSerializator);
    }

    // set new value
    FieldSerializator->SetFieldIntV(mem_in, mem_out, FieldId, IntV);

    // index new data
    RecIndexer.IndexRecField(mem_out, RecId, FieldId, *FieldSerializator);
    RecIdBlobPtr->GetDat(RecId) = Blob->Put(mem_out.GetBf(), mem_out.Len(), *PgPt);
}
/// Set field value using field id (default implementation throws exception)
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::SetFieldUInt(const uint64& RecId, const int& FieldId, const uint& UInt) {
    // get the memory containig the field for the record
    TThinMIn min = GetEditableField(RecId, FieldId);

    // if we are indexing the field and the value is nonnull, first deindex the old value
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    if (RecIndexer.HasIndexKey(FieldId) && !IsFieldNull(RecId, FieldId)) {
        RecIndexer.DeindexRecField(min.GetMemBase(), RecId, FieldId, *FieldSerializator);
    }

    // set new value
    FieldSerializator->SetFieldUInt(min.GetBfAddrChar(), min.Len(), FieldId, UInt);

    // index the new value in the updated memory buffer
    RecIndexer.IndexRecField(min.GetMemBase(), RecId, FieldId, *FieldSerializator);
}
/// Set field value using field id (default implementation throws exception)
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::SetFieldUInt16(const uint64& RecId, const int& FieldId, const uint16& UInt16) {
    // get the memory containig the field for the record
    TThinMIn min = GetEditableField(RecId, FieldId);

    // if we are indexing the field and the value is nonnull, first deindex the old value
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    if (RecIndexer.HasIndexKey(FieldId) && !IsFieldNull(RecId, FieldId)) {
        RecIndexer.DeindexRecField(min.GetMemBase(), RecId, FieldId, *FieldSerializator);
    }

    // set new value
    FieldSerializator->SetFieldUInt16(min.GetBfAddrChar(), min.Len(), FieldId, UInt16);

    // index the new value in the updated memory buffer
    RecIndexer.IndexRecField(min.GetMemBase(), RecId, FieldId, *FieldSerializator);
}
/// Set field value using field id (default implementation throws exception)
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::SetFieldUInt64(const uint64& RecId, const int& FieldId, const uint64& UInt64) {
    // special case if field is primary field
    if (FieldId == PrimaryFieldId) {
        // it is, make sure new value does not exist yet
        if (PrimaryUInt64IdH.IsKey(UInt64) && PrimaryUInt64IdH.GetDat(UInt64) != RecId) {
            throw TQmExcept::New("[TStorePbBlob::SetFieldUInt64] Primary key '" + TUInt64::GetStr(UInt64) +
                "' being set to field '" + GetFieldNm(FieldId) + "' already taken.");
        }
    }
    // get the memory containig the field for the record
    TThinMIn min = GetEditableField(RecId, FieldId);

    // if we are indexing the field and the value is nonnull, first deindex the old value
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    if (RecIndexer.HasIndexKey(FieldId) && !IsFieldNull(RecId, FieldId)) {
        RecIndexer.DeindexRecField(min.GetMemBase(), RecId, FieldId, *FieldSerializator);
    }
    if (FieldId == PrimaryFieldId) { DelPrimaryFieldUInt64(RecId, FieldSerializator->GetFieldUInt64(min, FieldId)); }

    // set new value
    FieldSerializator->SetFieldUInt64(min.GetBfAddrChar(), min.Len(), FieldId, UInt64);

    // index the new value in the updated memory buffer
    RecIndexer.IndexRecField(min.GetMemBase(), RecId, FieldId, *FieldSerializator);
    if (FieldId == PrimaryFieldId) { SetPrimaryFieldUInt64(RecId, UInt64); }
}

/// Set field value using field id (default implementation throws exception)
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::SetFieldStr(const uint64& RecId, const int& FieldId, const TStr& Str) {
    // special case if field is primary field
    if (FieldId == PrimaryFieldId) {
        // it is, make sure new value does not exist yet
        if (PrimaryStrIdH.IsKey(Str) && PrimaryStrIdH.GetDat(Str) != RecId) {
            throw TQmExcept::New("[TStorePbBlob::SetFieldStr] Primary key '" + Str +
                "' being set to field '" + GetFieldNm(FieldId) + "' already taken.");
        }
    }
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    TRecPtMap* RecIdBlobPtr = NULL;
    PPgBlob Blob; TPgBlobPt* PgPt = NULL;
    TMem mem_in, mem_out;
    GetRecData(RecId, FieldId, mem_in, RecIdBlobPtr, Blob, PgPt);
    if (FieldId == PrimaryFieldId) { DelPrimaryFieldStr(RecId, FieldSerializator->GetFieldStr(mem_in, FieldId)); }
    // deindex
    if (RecIndexer.HasIndexKey(FieldId) && !IsFieldNull(RecId, FieldId)) {
        RecIndexer.DeindexRecField(mem_in, RecId, FieldId, *FieldSerializator);
    }

    // set new value
    FieldSerializator->SetFieldStr(mem_in, mem_out, FieldId, Str);

    // index new data
    RecIndexer.IndexRecField(mem_out, RecId, FieldId, *FieldSerializator);
    if (FieldId == PrimaryFieldId) {
        SetPrimaryFieldStr(RecId, Str);
    }
    RecIdBlobPtr->GetDat(RecId) = Blob->Put(mem_out.GetBf(), mem_out.Len(), *PgPt);
}
/// Set field value using field id (default implementation throws exception)
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::SetFieldStrV(const uint64& RecId, const int& FieldId, const TStrV& StrV) {
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    TRecPtMap* RecIdBlobPtr = NULL;
    PPgBlob Blob; TPgBlobPt* PgPt = NULL;
    TMem mem_in, mem_out;
    GetRecData(RecId, FieldId, mem_in, RecIdBlobPtr, Blob, PgPt);
    // deindex
    if (RecIndexer.HasIndexKey(FieldId) && !IsFieldNull(RecId, FieldId)) {
        RecIndexer.DeindexRecField(mem_in, RecId, FieldId, *FieldSerializator);
    }

    // set new value
    FieldSerializator->SetFieldStrV(mem_in, mem_out, FieldId, StrV);

    // index new data
    RecIndexer.IndexRecField(mem_out, RecId, FieldId, *FieldSerializator);
    RecIdBlobPtr->GetDat(RecId) = Blob->Put(mem_out.GetBf(), mem_out.Len(), *PgPt);
}
/// Set field value using field id (default implementation throws exception)
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::SetFieldBool(const uint64& RecId, const int& FieldId, const bool& Bool) {
    // get the memory containig the field for the record
    TThinMIn min = GetEditableField(RecId, FieldId);

    // if we are indexing the field and the value is nonnull, first deindex the old value
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    if (RecIndexer.HasIndexKey(FieldId) && !IsFieldNull(RecId, FieldId)) {
        RecIndexer.DeindexRecField(min.GetMemBase(), RecId, FieldId, *FieldSerializator);
    }

    // set new value
    FieldSerializator->SetFieldBool(min.GetBfAddrChar(), min.Len(), FieldId, Bool);

    // index the new value in the updated memory buffer
    RecIndexer.IndexRecField(min.GetMemBase(), RecId, FieldId, *FieldSerializator);
}
/// Set field value using field id (default implementation throws exception)
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::SetFieldFlt(const uint64& RecId, const int& FieldId, const double& Flt) {
    // special case if field is primary field
    if (FieldId == PrimaryFieldId) {
        // it is, make sure new value does not exist yet
        if (PrimaryFltIdH.IsKey(Flt) && PrimaryFltIdH.GetDat(Flt) != RecId) {
            throw TQmExcept::New("[TStorePbBlob::SetFieldFlt] Primary key '" + TFlt::GetStr(Flt) +
                "' being set to field '" + GetFieldNm(FieldId) + "' already taken.");
        }
    }
    // get the memory containig the field for the record
    TThinMIn min = GetEditableField(RecId, FieldId);

    // if we are indexing the field and the value is nonnull, first deindex the old value
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    if (RecIndexer.HasIndexKey(FieldId) && !IsFieldNull(RecId, FieldId)) {
        RecIndexer.DeindexRecField(min.GetMemBase(), RecId, FieldId, *FieldSerializator);
    }
    if (FieldId == PrimaryFieldId) { DelPrimaryFieldFlt(RecId, FieldSerializator->GetFieldFlt(min, FieldId)); }

    // set new value
    FieldSerializator->SetFieldFlt(min.GetBfAddrChar(), min.Len(), FieldId, Flt);

    // index the new value in the updated memory buffer
    RecIndexer.IndexRecField(min.GetMemBase(), RecId, FieldId, *FieldSerializator);
    if (FieldId == PrimaryFieldId) { SetPrimaryFieldFlt(RecId, Flt); }
}
/// Set field value using field id (default implementation throws exception)
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::SetFieldSFlt(const uint64& RecId, const int& FieldId, const float& SFlt) {
    // get the memory containig the field for the record
    TThinMIn min = GetEditableField(RecId, FieldId);

    // if we are indexing the field and the value is nonnull, first deindex the old value
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    if (RecIndexer.HasIndexKey(FieldId) && !IsFieldNull(RecId, FieldId)) {
        RecIndexer.DeindexRecField(min.GetMemBase(), RecId, FieldId, *FieldSerializator);
    }

    // set new value
    FieldSerializator->SetFieldSFlt(min.GetBfAddrChar(), min.Len(), FieldId, SFlt);

    // index the new value in the updated memory buffer
    RecIndexer.IndexRecField(min.GetMemBase(), RecId, FieldId, *FieldSerializator);
}
/// Set field value using field id (default implementation throws exception)
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::SetFieldFltPr(const uint64& RecId, const int& FieldId, const TFltPr& FltPr) {
    // get the memory containig the field for the record
    TThinMIn min = GetEditableField(RecId, FieldId);

    // if we are indexing the field and the value is nonnull, first deindex the old value
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    if (RecIndexer.HasIndexKey(FieldId) && !IsFieldNull(RecId, FieldId)) {
        RecIndexer.DeindexRecField(min.GetMemBase(), RecId, FieldId, *FieldSerializator);
    }

    // set new value
    FieldSerializator->SetFieldFltPr(min.GetBfAddrChar(), min.Len(), FieldId, FltPr);

    // index the new value in the updated memory buffer
    RecIndexer.IndexRecField(min.GetMemBase(), RecId, FieldId, *FieldSerializator);
}
/// Set field value using field id (default implementation throws exception)
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::SetFieldFltV(const uint64& RecId, const int& FieldId, const TFltV& FltV) {
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    TRecPtMap* RecIdBlobPtr = NULL;
    PPgBlob Blob; TPgBlobPt* PgPt = NULL;
    TMem mem_in, mem_out;
    GetRecData(RecId, FieldId, mem_in, RecIdBlobPtr, Blob, PgPt);
    // deindex
    if (RecIndexer.HasIndexKey(FieldId) && !IsFieldNull(RecId, FieldId)) {
        RecIndexer.DeindexRecField(mem_in, RecId, FieldId, *FieldSerializator);
    }

    // set new value
    FieldSerializator->SetFieldFltV(mem_in, mem_out, FieldId, FltV);

    // index new data
    RecIndexer.IndexRecField(mem_out, RecId, FieldId, *FieldSerializator);
    RecIdBlobPtr->GetDat(RecId) = Blob->Put(mem_out.GetBf(), mem_out.Len(), *PgPt);
}
/// Set field value using field id (default implementation throws exception)
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::SetFieldTm(const uint64& RecId, const int& FieldId, const TTm& Tm) {
    // get the memory containig the field for the record
    TThinMIn min = GetEditableField(RecId, FieldId);

    // if we are indexing the field and the value is nonnull, first deindex the old value
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    if (RecIndexer.HasIndexKey(FieldId) && !IsFieldNull(RecId, FieldId)) {
        RecIndexer.DeindexRecField(min.GetMemBase(), RecId, FieldId, *FieldSerializator);
    }

    // set new value
    FieldSerializator->SetFieldTm(min.GetBfAddrChar(), min.Len(), FieldId, Tm);

    // index the new value in the updated memory buffer
    RecIndexer.IndexRecField(min.GetMemBase(), RecId, FieldId, *FieldSerializator);
}
/// Set field value using field id (default implementation throws exception)
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::SetFieldTmMSecs(const uint64& RecId, const int& FieldId, const uint64& TmMSecs) {
    // special case if field is primary field
    if (FieldId == PrimaryFieldId) {
        // it is, make sure new value does not exist yet
        if (PrimaryTmMSecsIdH.IsKey(TmMSecs) && PrimaryTmMSecsIdH.GetDat(TmMSecs) != RecId) {
            throw TQmExcept::New("[TStorePbBlob::SetFieldTmMSecs] Primary key '" + TUInt64::GetStr(TmMSecs) +
                "' being set to field '" + GetFieldNm(FieldId) + "' already taken.");
        }
    }
    // get the memory containig the field for the record
    TThinMIn min = GetEditableField(RecId, FieldId);

    // if we are indexing the field and the value is nonnull, first deindex the old value
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    if (RecIndexer.HasIndexKey(FieldId) && !IsFieldNull(RecId, FieldId)) {
        RecIndexer.DeindexRecField(min.GetMemBase(), RecId, FieldId, *FieldSerializator);
    }
    if (FieldId == PrimaryFieldId) { DelPrimaryFieldMSecs(RecId, FieldSerializator->GetFieldTmMSecs(min, FieldId)); }

    // set new value
    FieldSerializator->SetFieldTmMSecs(min.GetBfAddrChar(), min.Len(), FieldId, TmMSecs);

    // index the new value in the updated memory buffer
    RecIndexer.IndexRecField(min.GetMemBase(), RecId, FieldId, *FieldSerializator);
    if (FieldId == PrimaryFieldId) { SetPrimaryFieldMSecs(RecId, TmMSecs); }
}
/// Set field value using field id (default implementation throws exception)
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::SetFieldNumSpV(const uint64& RecId, const int& FieldId, const TIntFltKdV& SpV) {
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    TRecPtMap* RecIdBlobPtr = NULL;
    PPgBlob Blob; TPgBlobPt* PgPt = NULL;
    TMem mem_in, mem_out;
    GetRecData(RecId, FieldId, mem_in, RecIdBlobPtr, Blob, PgPt);
    // deindex
    if (RecIndexer.HasIndexKey(FieldId) && !IsFieldNull(RecId, FieldId)) {
        RecIndexer.DeindexRecField(mem_in, RecId, FieldId, *FieldSerializator);
    }

    // set new value
    FieldSerializator->SetFieldNumSpV(mem_in, mem_out, FieldId, SpV);

    // index new data
    RecIndexer.IndexRecField(mem_out, RecId, FieldId, *FieldSerializator);
    RecIdBlobPtr->GetDat(RecId) = Blob->Put(mem_out.GetBf(), mem_out.Len(), *PgPt);
}
/// Set field value using field id (default implementation throws exception)
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::SetFieldBowSpV(const uint64& RecId, const int& FieldId, const PBowSpV& SpV) {
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    TRecPtMap* RecIdBlobPtr = NULL;
    PPgBlob Blob; TPgBlobPt* PgPt = NULL;
    TMem mem_in, mem_out;
    GetRecData(RecId, FieldId, mem_in, RecIdBlobPtr, Blob, PgPt);
    // deindex
    if (RecIndexer.HasIndexKey(FieldId) && !IsFieldNull(RecId, FieldId)) {
        RecIndexer.DeindexRecField(mem_in, RecId, FieldId, *FieldSerializator);
    }

    // set new value
    FieldSerializator->SetFieldBowSpV(mem_in, mem_out, FieldId, SpV);

    // index new data
    RecIndexer.IndexRecField(mem_out, RecId, FieldId, *FieldSerializator);
    RecIdBlobPtr->GetDat(RecId) = Blob->Put(mem_out.GetBf(), mem_out.Len(), *PgPt);
}
/// Set field value using field id (default implementation throws exception)
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::SetFieldTMem(const uint64& RecId, const int& FieldId, const TMem& Mem) {
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    TRecPtMap* RecIdBlobPtr = NULL;
    PPgBlob Blob; TPgBlobPt* PgPt = NULL;
    TMem mem_in, mem_out;
    GetRecData(RecId, FieldId, mem_in, RecIdBlobPtr, Blob, PgPt);
    // deindex
    if (RecIndexer.HasIndexKey(FieldId) && !IsFieldNull(RecId, FieldId)) {
        RecIndexer.DeindexRecField(mem_in, RecId, FieldId, *FieldSerializator);
    }

    // set new value
    FieldSerializator->SetFieldTMem(mem_in, mem_out, FieldId, Mem);

    // index new data
    RecIndexer.IndexRecField(mem_out, RecId, FieldId, *FieldSerializator);
    RecIdBlobPtr->GetDat(RecId) = Blob->Put(mem_out.GetBf(), mem_out.Len(), *PgPt);
}
/// Set field value using field id (default implementation throws exception)
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::SetFieldJsonVal(const uint64& RecId, const int& FieldId, const PJsonVal& Json) {
    TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    TRecPtMap* RecIdBlobPtr = NULL;
    PPgBlob Blob; TPgBlobPt* PgPt = NULL;
    TMem mem_in, mem_out;
    GetRecData(RecId, FieldId, mem_in, RecIdBlobPtr, Blob, PgPt);
    // deindex
    if (RecIndexer.HasIndexKey(FieldId) && !IsFieldNull(RecId, FieldId)) {
        RecIndexer.DeindexRecField(mem_in, RecId, FieldId, *FieldSerializator);
    }

    // set new value
    FieldSerializator->SetFieldJsonVal(mem_in, mem_out, FieldId, Json);

    // index new data
    RecIndexer.IndexRecField(mem_out, RecId, FieldId, *FieldSerializator);
    RecIdBlobPtr->GetDat(RecId) = Blob->Put(mem_out.GetBf(), mem_out.Len(), *PgPt);
}

/// Check if given ID is valid
template <class TRecPtMap>
bool TStorePbBlobT<TRecPtMap>::IsRecId(const uint64& RecId) const {
    return DataMemP ? RecIdBlobPtHMem.IsKey(RecId) : RecIdBlobPtH.IsKey(RecId);
}

/// Set primary field map
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::SetPrimaryField(const uint64& RecId) {
    // the primary map lives only in the PgBlobStore state file, which the
    // destructor skips rewriting unless the metadata is dirty (the typed
    // SetPrimaryField* setters do the same)
    MetaDirtyP = true;
    if (PrimaryFieldType == oftStr) {
        PrimaryStrIdH.AddDat(GetFieldStr(RecId, PrimaryFieldId)) = RecId;
    } else if (PrimaryFieldType == oftInt) {
        PrimaryIntIdH.AddDat(GetFieldInt(RecId, PrimaryFieldId)) = RecId;
    } else if (PrimaryFieldType == oftUInt64) {
        PrimaryUInt64IdH.AddDat(GetFieldUInt64(RecId, PrimaryFieldId)) = RecId;
    } else if (PrimaryFieldType == oftFlt) {
        PrimaryFltIdH.AddDat(GetFieldFlt(RecId, PrimaryFieldId)) = RecId;
    } else if (PrimaryFieldType == oftTm) {
        PrimaryTmMSecsIdH.AddDat(GetFieldTmMSecs(RecId, PrimaryFieldId)) = RecId;
    } else {
        EAssertR(false, "Unsupported primary-field type");
    }
}

/// Delete primary field map
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::DelPrimaryField(const uint64& RecId) {
    // see SetPrimaryField - without this a JSON UpdateRec that changes the
    // primary field of a record through the in-place update path would leave
    // the metadata clean and the rewritten primary map would be lost on close
    MetaDirtyP = true;
    if (PrimaryFieldType == oftStr) {
        PrimaryStrIdH.DelIfKey(GetFieldStr(RecId, PrimaryFieldId));
    } else if (PrimaryFieldType == oftInt) {
        PrimaryIntIdH.DelIfKey(GetFieldInt(RecId, PrimaryFieldId));
    } else if (PrimaryFieldType == oftUInt64) {
        PrimaryUInt64IdH.DelIfKey(GetFieldUInt64(RecId, PrimaryFieldId));
    } else if (PrimaryFieldType == oftFlt) {
        PrimaryFltIdH.DelIfKey(GetFieldFlt(RecId, PrimaryFieldId));
    } else if (PrimaryFieldType == oftTm) {
        PrimaryTmMSecsIdH.DelIfKey(GetFieldTmMSecs(RecId, PrimaryFieldId));
    } else {
        EAssertR(false, "Unsupported primary-field type");
    }
}

/// Check if record with given name exists
template <class TRecPtMap>
bool TStorePbBlobT<TRecPtMap>::IsRecNm(const TStr& RecNm) const {
    return RecNmFieldP && PrimaryStrIdH.IsKey(RecNm);
}

/// Find name of the record with given ID
template <class TRecPtMap>
TStr TStorePbBlobT<TRecPtMap>::GetRecNm(const uint64& RecId) const {
    // return empty string when no primary key
    if (!HasRecNm()) { return TStr(); }
    // get the name of primary key
    return GetFieldStr(RecId, PrimaryFieldId);
}

/// Return ID of record with given name
template <class TRecPtMap>
uint64 TStorePbBlobT<TRecPtMap>::GetRecId(const TStr& RecNm) const {
    return (PrimaryStrIdH.IsKey(RecNm) ? PrimaryStrIdH.GetDat(RecNm).Val : TUInt64::Mx);
}

/// Get number of record
template <class TRecPtMap>
uint64 TStorePbBlobT<TRecPtMap>::GetRecs() const {
    return DataMemP ? RecIdBlobPtHMem.Len() : RecIdBlobPtH.Len();
}

/// Return iterator over store
template <class TRecPtMap>
PStoreIter TStorePbBlobT<TRecPtMap>::GetIter() const {
    if (Empty()) { return TStoreIterVec::New(); }
    return DataMemP ? RecIdBlobPtHMem.GetIter() : RecIdBlobPtH.GetIter();
}

template <class TRecPtMap>
uint64 TStorePbBlobT<TRecPtMap>::GetFirstRecId() const {
    // record ids are monotonically increasing, but any record can be deleted, so
    // the smallest live id has to come from the map itself (TUInt64::Mx when empty)
    return DataMemP ? RecIdBlobPtHMem.GetFirstRecId() : RecIdBlobPtH.GetFirstRecId();
}

template <class TRecPtMap>
uint64 TStorePbBlobT<TRecPtMap>::GetLastRecId() const {
    // record ids are monotonically increasing, but any record can be deleted, so
    // the largest live id has to come from the map itself (TUInt64::Mx when empty)
    return DataMemP ? RecIdBlobPtHMem.GetLastRecId() : RecIdBlobPtH.GetLastRecId();
}

/// Helper function for returning JSON definition of store
template <class TRecPtMap>
PJsonVal TStorePbBlobT<TRecPtMap>::GetStoreJson(const TWPt<TBase>& Base) const {
    PJsonVal Result = TStore::GetStoreJson(Base);
    Result->AddToObj("name", this->GetStoreNm());
    if (WndDesc.WindowType != TStoreWndType::swtNone) {
        PJsonVal WindowJson = TJsonVal::NewObj();
        WindowJson->AddToObj("type", WndDesc.WindowType == TStoreWndType::swtLength ? "length" : "time");
        WindowJson->AddToObj("size", (int)WndDesc.WindowSize);
        if (WndDesc.WindowType == TStoreWndType::swtTime) {
            WindowJson->AddToObj("timeField", WndDesc.TimeFieldNm);
        }
        Result->AddToObj("window", WindowJson);
    }
    return Result;
}

template <class TRecPtMap>
int TStorePbBlobT<TRecPtMap>::GetCodebookId(const int& FieldId, const TStr& Str) const {
    const TRecSerializator* FieldSerializator = GetFieldSerializator(FieldId);
    return FieldSerializator->GetCodebookId(FieldId, Str);
}

/// Save part of the data, given time-window
template <class TRecPtMap>
int TStorePbBlobT<TRecPtMap>::PartialFlush(int WndInMsec) {
    DataBlob->PartialFlush(WndInMsec);
    return 0;
}

/// Retrieve performance statistics for this store
template <class TRecPtMap>
PJsonVal TStorePbBlobT<TRecPtMap>::GetStats() {
    PJsonVal res = TJsonVal::NewObj();
    res->AddToObj("name", GetStoreNm());
    res->AddToObj("blob_storage", DataBlob->GetStats());
    res->AddToObj("mem_storage", DataMem->GetStats());
    return res;
}

/// Run verification for whole store
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::RunVerification() {
    // loop over all pages
    this->DataMem->RunVerification();
    this->DataBlob->RunVerification();
}

/// Run verification for single record
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::RunVerificationForRecord(const uint64& RecId) {
    // do nothing for now
    {
        const TPgBlobPt PgPt = RecIdBlobPtH.GetDat(RecId);
        TThinMIn min = DataBlob->Get(PgPt);
        SerializatorCache->Verify(min.GetBfAddrChar(), min.Len());
    }
    {
        const TPgBlobPt PgPt = RecIdBlobPtHMem.GetDat(RecId);
        TThinMIn min = DataBlob->Get(PgPt);
        SerializatorMem->Verify(min.GetBfAddrChar(), min.Len());
    }
}

/// Purge records that fall out of store window (when it has one)
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::GarbageCollect(const int& MxTimeMSecs) {
    // if no window, nothing to do here
    if (WndDesc.WindowType == swtNone) { return; }
    // if no records, nothing to do here
    if (Empty()) { return; }
    // TODO find records to delete
    // prepare list of records that need to be deleted
    TUInt64V DelRecIdV;
    if (WndDesc.WindowType == swtTime) {
        // get last added record
        const uint64 LastRecId = GetLastRecId();
        // get time window field
        const int TimeFieldId = GetFieldId(WndDesc.TimeFieldNm);
        // get time which we use as end of time-window (could be insert time or field value)
        uint64 CurMSecs = WndDesc.InsertP ? TTm::GetCurUniMSecs() :
            GetFieldTmMSecs(LastRecId, TimeFieldId);
        // get start of time window
        const uint64 WindowStartMSecs = CurMSecs - WndDesc.WindowSize;
        // report what is the established time window used by the garbage collection
        TEnv::Logger->OnStatusFmt("  window: %s - %s",
            TTm::GetTmFromMSecs(WindowStartMSecs).GetWebLogDateTimeStr(true, "T", false).CStr(),
            TTm::GetTmFromMSecs(CurMSecs).GetWebLogDateTimeStr(true, "T", false).CStr());
        // iterate from the start until we hit the time window
        PStoreIter Iter = GetIter();
        while (Iter->Next()) {
            uint64 RecId = Iter->GetRecId();
            // get record time
            uint64 TmMSecs = GetFieldTmMSecs(RecId, TimeFieldId);
            // if we are within time window we stop
            if (TmMSecs >= WindowStartMSecs) break;
            // otherwise we mark the record for deletion
            DelRecIdV.Add(RecId);
        }
    }
    else if (GetRecs() > WndDesc.WindowSize) {
        // we are windowing based on number of records
        TEnv::Logger->OnStatusFmt("  window: last %d records", (int) WndDesc.WindowSize);
        // get number of records which need to be deleted so we are back in the window
        int DelRecs = (int) (GetRecs() - WndDesc.WindowSize);
        // iterate from the start until we hit the time window
        PStoreIter Iter = GetIter();
        while (Iter->Next() && DelRecs > 0) {
            // mark record for deletion
            DelRecIdV.Add(Iter->GetRecId());
            // track progress
            DelRecs--;
        }
    }
    TEnv::Logger->OnStatusFmt("  purging %d records", DelRecIdV.Len());
    TStorePbBlobT<TRecPtMap>::DeleteRecs(DelRecIdV, MxTimeMSecs, false);
}

/// Perform defragmentation
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::Defrag() {
    // TODO merge pages
    // TODO remove empty pages
}


/// Deletes all records
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::DeleteAllRecs() {
    // if no records, nothing to do here
    if (Empty()) { return; }
    TEnv::Logger->OnStatusFmt("Deleting all (%d) records in %s", GetRecs(), GetStoreNm().CStr());

    // delete records from index; snapshot the record ids first - the loop deletes
    // from the map it would otherwise be iterating
    TRecPtMap* Target = (DataMemP ? &RecIdBlobPtHMem : &RecIdBlobPtH);
    TUInt64V DelRecIdV; Target->GetKeyV(DelRecIdV);
    for (int DelRecN = 0; DelRecN < DelRecIdV.Len(); DelRecN++) {
        const uint64 DelRecId = DelRecIdV[DelRecN];
        // executed triggers before deletion
        OnDelete(DelRecId);
        // delete record from name-id map
        if (IsPrimaryField()) { DelPrimaryField(DelRecId); }
        // delete record from indexes
        if (DataBlobP) {
            TPgBlobPt Pt = RecIdBlobPtH.GetDat(DelRecId);
            TMemBase CacheRecMem = DataBlob->GetMemBase(Pt);
            RecIndexer.DeindexRec(CacheRecMem, DelRecId, *SerializatorCache);
            //DataBlob->Del(Pt);
            RecIdBlobPtH.DelKey(DelRecId);
        }
        if (DataMemP) {
            TPgBlobPt Pt = RecIdBlobPtHMem.GetDat(DelRecId);
            TMemBase RecMem = DataMem->GetMemBase(Pt);
            RecIndexer.DeindexRec(RecMem, DelRecId, *SerializatorMem);
            //DataMem->Del(Pt);
            RecIdBlobPtHMem.DelKey(DelRecId);
        }
        // delete record from joins
        TRec Rec(this, DelRecId);
        for (int JoinN = 0; JoinN < GetJoins(); JoinN++) {
            TJoinDesc JoinDesc = GetJoinDesc(JoinN);
            // execute the join
            PRecSet JoinRecSet = Rec.DoJoin(GetBase(), JoinDesc.GetJoinId());
            for (int JoinRecN = 0; JoinRecN < JoinRecSet->GetRecs(); JoinRecN++) {
                // remove joins with all matched records, one by one
                const uint64 JoinRecId = JoinRecSet->GetRecId(JoinRecN);
                const int JoinFq = JoinRecSet->GetRecFq(JoinRecN);
                DelJoin(JoinDesc.GetJoinId(), DelRecId, JoinRecId, JoinFq);
            }
        }
    }
    // delete records from disk
    TEnv::Logger->OnStatus("Internal structures 1");
    PrimaryStrIdH.Clr();
    PrimaryIntIdH.Clr();
    PrimaryUInt64IdH.Clr();
    PrimaryFltIdH.Clr();
    PrimaryTmMSecsIdH.Clr();

    TEnv::Logger->OnStatus("Internal structures 2");
    RecIdBlobPtH.Clr();
    RecIdBlobPtHMem.Clr();
    MetaDirtyP = true;
    DataBlob->Clr();
    DataMem->Clr();
    PartialFlush(TInt::Mx);
}


template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::DeleteFirstRecs(const int& Recs) {
    PRecSet RecSet = GetAllRecs();
    int RecCnt = RecSet->GetRecs();
    if (RecCnt <= 0) {
        return;
    }
    TUInt64V RecIds(RecCnt, 0);
    for (int i = 0; i < RecCnt; i++) {
        RecIds.Add(RecSet->GetRecId(i));
    }
    RecIds.Sort();
    if (RecIds.Len()>Recs) {
        RecIds.Del(Recs, RecIds.Len() - 1);
    }
    DeleteRecs(RecIds, -1, false);
}

template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::DeleteRecs(const TUInt64V& DelRecIdV, const int& MxTimeMSecs, const bool& AssertOK) {
    if (AssertOK) {
        // assert that DelRecIdV is valid
        TRecPtMap* Ht = (DataMemP ? &RecIdBlobPtHMem : &RecIdBlobPtH);
        for (int i = 0; i < DelRecIdV.Len(); i++) {
            QmAssertR(Ht->IsKey(DelRecIdV[i]),
                "TStorePbBlob::DeleteRecs - incorrect record id. Record with specified ID not found.");
        }
    }
    // delete records
    if (!DelRecIdV.Empty()) { MetaDirtyP = true; }
    TTmStopWatch StopWatch(true);
    for (int DelRecN = 0; DelRecN < DelRecIdV.Len(); DelRecN++) {
        // report progress
        if (DelRecN > 0 && DelRecN % 1000 == 0) { TEnv::Logger->OnStatusFmt("    %d\r", DelRecN); }
        // check if we still have time
        if ((MxTimeMSecs != -1) && (StopWatch.GetMSecInt() > MxTimeMSecs)) {
            TEnv::Logger->OnStatusFmt("Reached time limit of %d msecs in TStorePbBlob::DeleteRecs");
            break;
        }
        // what are we deleting now
        const uint64 DelRecId = DelRecIdV[DelRecN];
        // execute triggers before deletion
        OnDelete(DelRecId);
        // delete record from name-id map
        if (IsPrimaryField()) { DelPrimaryField(DelRecId); }

        // delete record from joins
        TRec Rec(this, DelRecId);
        for (int JoinN = 0; JoinN < GetJoins(); JoinN++) {
            TJoinDesc JoinDesc = GetJoinDesc(JoinN);
            // execute the join
            PRecSet JoinRecSet = Rec.DoJoin(GetBase(), JoinDesc.GetJoinId());
            for (int JoinRecN = 0; JoinRecN < JoinRecSet->GetRecs(); JoinRecN++) {
                // remove joins with all matched records, one by one
                const uint64 JoinRecId = JoinRecSet->GetRecId(JoinRecN);
                DelJoin(JoinDesc.GetJoinId(), DelRecId, JoinRecId);
            }
        }

        // delete record from indexes
        if (DataBlobP) {
            TPgBlobPt Pt = RecIdBlobPtH.GetDat(DelRecId);
            TMemBase CacheRecMem = DataBlob->GetMemBase(Pt);
            RecIndexer.DeindexRec(CacheRecMem, DelRecId, *SerializatorCache);
            SerializatorCache->DeleteToast(CacheRecMem);
            DataBlob->Del(Pt);
            RecIdBlobPtH.DelKey(DelRecId);
        }
        if (DataMemP) {
            TPgBlobPt Pt = RecIdBlobPtHMem.GetDat(DelRecId);
            TMemBase RecMem = DataMem->GetMemBase(Pt);
            RecIndexer.DeindexRec(RecMem, DelRecId, *SerializatorMem);
            SerializatorMem->DeleteToast(RecMem);
            DataMem->Del(Pt);
            RecIdBlobPtHMem.DelKey(DelRecId);
        }
    }

    // deleting the oldest records may have freed leading record-map slots (a
    // no-op for the hash representation)
    TrimRecIdMaps();

    // report success :-)
    if (DelRecIdV.Len() > 1000) {
        TEnv::Logger->OnStatusFmt("  %s records at end", TUInt64::GetStr(GetRecs()).CStr());
    }
}

/// Reclaim the record-map memory freed by deleting the smallest record ids: a
/// dense map drops its leading empty slots and advances its id offset (the two
/// sections trim independently - each map has its own offset); the hash
/// representation needs no trimming. Called after the delete paths, so a store
/// whose oldest records are rolling-deleted stays compact between defrags.
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::TrimRecIdMaps() {
    const int64 TrimmedBlob = RecIdBlobPtH.TrimLeadingEmpty();
    const int64 TrimmedMem = RecIdBlobPtHMem.TrimLeadingEmpty();
    if (TrimmedBlob > 0 || TrimmedMem > 0) {
        MetaDirtyP = true;
        TEnv::Logger->OnStatusFmt("Store '%s': trimmed the leading deleted record-map slots (disk %s, memory %s)",
            GetStoreNm().CStr(), TInt64::GetStr(TrimmedBlob).CStr(), TInt64::GetStr(TrimmedMem).CStr());
    }
}

template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::BatchDeleteRecs(const TUInt64V& DelRecIdV, const TBatchDelProgressCb& OnProgress) {
    if (DelRecIdV.Empty()) { return; }

    TUInt64H RecIdSet(DelRecIdV.Len());
    for (int N = 0; N < DelRecIdV.Len(); N++) { RecIdSet.AddKey(DelRecIdV[N]); }

    // smallest record id that survives this delete: every index item below it references either
    // a record deleted right now or one already gone, so the gix scan can drop whole posting-list
    // children below it from their headers without reading them. The threshold is only as good
    // as the oldest survivor - a single very old record that is kept moves it to that record.
    // No survivor means everything goes - TUInt64::Mx then correctly drops every child
    uint64 MinKeepRecId = TUInt64::Mx;
    { PStoreIter Iter = GetIter(); while (Iter->Next()) {
        const uint64 RecId = Iter->GetRecId();
        if (RecId < MinKeepRecId && !RecIdSet.IsKey(RecId)) { MinKeepRecId = RecId; }
    } }
    TEnv::Logger->OnStatusFmt("BatchDeleteRecs: smallest record id kept: %s",
        TUInt64::GetStr(MinKeepRecId).CStr());

    TIntSet KeyIdSet;
    RecIndexer.GetGixKeyIdSet(KeyIdSet);
    GetIndex()->BatchDeleteFromGix(KeyIdSet, RecIdSet, MinKeepRecId, OnProgress);

    // the store phase advances one requested record at a time; ids that are no longer valid are
    // skipped, so Done counts records looked at and Removed counts the ones actually deleted
    const TStr Phase = "5/5 Store";
    const int64 TotalRecs = DelRecIdV.Len();
    const int64 ReportEvery = TotalRecs / 200 + 1;
    int64 DeletedRecs = 0;
    if (OnProgress) { OnProgress(Phase, 0, TotalRecs, 0); }
    for (int N = 0; N < DelRecIdV.Len(); N++) {
        const uint64 DelRecId = DelRecIdV[N];
        if (IsRecId(DelRecId)) {
            OnDelete(DelRecId);
            if (IsPrimaryField()) { DelPrimaryField(DelRecId); }
            TRec Rec(this, DelRecId);
            for (int JoinN = 0; JoinN < GetJoins(); JoinN++) {
                TJoinDesc JoinDesc = GetJoinDesc(JoinN);
                PRecSet JoinRecSet = Rec.DoJoin(GetBase(), JoinDesc.GetJoinId());
                for (int JoinRecN = 0; JoinRecN < JoinRecSet->GetRecs(); JoinRecN++) {
                    DelJoin(JoinDesc.GetJoinId(), DelRecId, JoinRecSet->GetRecId(JoinRecN));
                }
            }
            if (DataBlobP) {
                TPgBlobPt Pt = RecIdBlobPtH.GetDat(DelRecId);
                TMemBase RecMem = DataBlob->GetMemBase(Pt);
                RecIndexer.DeindexRecNonGix(RecMem, DelRecId, *SerializatorCache);
                SerializatorCache->DeleteToast(RecMem);
                DataBlob->Del(Pt);
                RecIdBlobPtH.DelKey(DelRecId);
            }
            if (DataMemP) {
                TPgBlobPt Pt = RecIdBlobPtHMem.GetDat(DelRecId);
                TMemBase RecMem = DataMem->GetMemBase(Pt);
                RecIndexer.DeindexRecNonGix(RecMem, DelRecId, *SerializatorMem);
                SerializatorMem->DeleteToast(RecMem);
                DataMem->Del(Pt);
                RecIdBlobPtHMem.DelKey(DelRecId);
            }
            DeletedRecs++;
        }
        const int64 Done = N + 1;
        if (OnProgress && (Done % ReportEvery == 0 || Done == TotalRecs)) {
            OnProgress(Phase, Done, TotalRecs, DeletedRecs);
        }
    }
    // the maps (and the primary map) changed, so the state file must be rewritten
    // on close - this was missing before: a session that only batch-deleted could
    // skip the save and resurrect the deleted records on the next load
    if (DeletedRecs > 0) { MetaDirtyP = true; }
    // deleting the oldest records may have freed leading record-map slots (a
    // no-op for the hash representation)
    TrimRecIdMaps();
    if (DelRecIdV.Len() > 1000) {
        TEnv::Logger->OnStatusFmt("  %s records at end", TUInt64::GetStr(GetRecs()).CStr());
    }
}

/// Initialize field location flags
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::InitDataFlags() {
    // go over all the fields and remember if we use in-memory or blob storage
    DataBlobP = false;
    DataMemP = false;
    for (int FieldId = 0; FieldId < GetFields(); FieldId++) {
        DataBlobP = DataBlobP || (FieldLocV[FieldId] == slDisk);
        DataMemP = DataMemP || (FieldLocV[FieldId] == slMemory);
    }
    // at least one must be true, otherwise we have no fields, which is not good
    EAssert(DataBlobP || DataMemP);
}

/// Initialize from given store schema
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::InitFromSchema(const TStoreSchema& StoreSchema) {
    // at start there is no primary key
    RecNmFieldP = false;
    PrimaryFieldId = -1;
    PrimaryFieldType = oftUndef;
    // create fields
    for (int i = 0; i < StoreSchema.FieldH.Len(); i++) {
        const TFieldDesc& FieldDesc = StoreSchema.FieldH[i];
        AddFieldDesc(FieldDesc);
        // check if we found a primary field
        if (FieldDesc.IsPrimary()) {
            QmAssertR(PrimaryFieldId == -1, "Store can have only one primary field");
            // only string fields can serve as record name (TODO: extend)
            RecNmFieldP = FieldDesc.IsStr();
            PrimaryFieldId = GetFieldId(FieldDesc.GetFieldNm());
            PrimaryFieldType = FieldDesc.GetFieldType();
        }
    }
    // create index keys
    TWPt<TIndexVoc> IndexVoc = GetIndex()->GetIndexVoc();
    for (int IndexKeyExN = 0; IndexKeyExN < StoreSchema.IndexKeyExV.Len(); IndexKeyExN++) {
        TIndexKeyEx IndexKeyEx = StoreSchema.IndexKeyExV[IndexKeyExN];
        // get associated field
        const int FieldId = GetFieldId(IndexKeyEx.FieldName);
        // if we are given vocabulary name, check if we have one with such name already
        const int WordVocId = GetBase()->NewIndexWordVoc(IndexKeyEx.KeyType, IndexKeyEx.WordVocName);
        // create new index key
        const int KeyId = GetBase()->NewFieldIndexKey(this, IndexKeyEx.KeyIndexName,
            FieldId, WordVocId, IndexKeyEx.KeyType, IndexKeyEx.GixType, IndexKeyEx.SortType);
        // assign tokenizer to it if we have one
        if (IndexKeyEx.IsTokenizer()) { IndexVoc->PutTokenizer(KeyId, IndexKeyEx.Tokenizer); }
    }
    // prepare serializators for disk and in-memory store
    SerializatorCache = new TRecSerializator(this, this, StoreSchema, slDisk);
    SerializatorMem = new TRecSerializator(this, this, StoreSchema, slMemory);
    // initialize field to storage location map
    InitFieldLocV();
    // initialize record indexer
    RecIndexer = TRecIndexer(GetIndex(), this);
    // remember window parameters
    WndDesc = StoreSchema.WndDesc;
}

/// initialize field storage location map
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::InitFieldLocV() {
    for (int FieldId = 0; FieldId < GetFields(); FieldId++) {
        if (SerializatorCache->IsFieldId(FieldId)) {
            FieldLocV.Add(slDisk);
        } else if (SerializatorMem->IsFieldId(FieldId)) {
            FieldLocV.Add(slMemory);
        } else {
            throw TQmExcept::New("Unknown storage location for field " +
                GetFieldNm(FieldId) + " in store " + GetStoreNm());
        }
    }
}

///////////////////////////////////////////////////////////

template <class TRecPtMap>
TStorePbBlobT<TRecPtMap>::TStorePbBlobT(const TWPt<TBase>& Base, const uint& StoreId,
    const TStr& StoreName, const TStoreSchema& StoreSchema, const TStr& _StoreFNm,
    const int64& _MxCacheSize, const int& BlockSize) :
    TStorePbBlobBase(Base, StoreId, StoreName), StoreFNm(_StoreFNm), FAccess(faCreate) {

    SetStoreType(TRecPtMap::GetStoreTypeNm());
    // freshly created store must write its metadata files at least once
    MetaDirtyP = true;
    DataBlob = new TPgBlob(_StoreFNm + "PgBlob", TFAccess::faCreate, _MxCacheSize);
    DataMem = new TPgBlob(_StoreFNm + "PgBlobMem", TFAccess::faCreate, TUInt64::Mx);
    InitFromSchema(StoreSchema);
    InitDataFlags();
}

template <class TRecPtMap>
TStorePbBlobT<TRecPtMap>::TStorePbBlobT(const TWPt<TBase>& Base, const TStr& _StoreFNm,
    const TFAccess& _FAccess, const int64& _MxCacheSize,
    const bool& _Lazy) :
    TStorePbBlobBase(Base, _StoreFNm + ".BaseStore"),
    StoreFNm(_StoreFNm), FAccess(_FAccess), PrimaryFieldType(oftUndef) {

    SetStoreType(TRecPtMap::GetStoreTypeNm());
    DataBlob = new TPgBlob(_StoreFNm + "PgBlob", _FAccess, _MxCacheSize);
    DataMem = new TPgBlob(_StoreFNm + "PgBlobMem", _FAccess, TUInt64::Mx);
    if (!_Lazy) {
        DataMem->LoadAll();
    }

    // load members
    TFIn FIn(StoreFNm + "PgBlobStore");
    RecNmFieldP.Load(FIn);
    PrimaryFieldId.Load(FIn);
    // deduce primary field type
    if (PrimaryFieldId != -1) {
        PrimaryFieldType = GetFieldDesc(PrimaryFieldId).GetFieldType();
        if (PrimaryFieldType == oftStr) {
            PrimaryStrIdH.Load(FIn);
        } else if (PrimaryFieldType == oftInt) {
            PrimaryIntIdH.Load(FIn);
        } else if (PrimaryFieldType == oftUInt64) {
            PrimaryUInt64IdH.Load(FIn);
        } else if (PrimaryFieldType == oftFlt) {
            PrimaryFltIdH.Load(FIn);
        } else if (PrimaryFieldType == oftTm) {
            PrimaryTmMSecsIdH.Load(FIn);
        } else {
            throw TQmExcept::New("Unsupported primary field type!");
        }
    } else {
        // backwards compatibility
        PrimaryStrIdH.Load(FIn);
    }
    // load time window
    WndDesc.Load(FIn);
    // load data
    SerializatorCache = new TRecSerializator(this);
    SerializatorMem = new TRecSerializator(this);
    SerializatorCache->Load(FIn);
    SerializatorMem->Load(FIn);
    RecIdBlobPtH.Load(FIn);
    RecIdBlobPtHMem.Load(FIn);
    RecIdCounter.Load(FIn);

    // initialize field to storage location map
    InitFieldLocV();
    // initialize record indexer
    RecIndexer = TRecIndexer(GetIndex(), this);

    // initialize data storage flags
    InitDataFlags();

    // nothing was modified yet with regards to the loaded metadata
    MetaDirtyP = false;
}

template <class TRecPtMap>
TStorePbBlobT<TRecPtMap>::~TStorePbBlobT() {
    // save if necessary; when the metadata (primary-field maps, record-id
    // blob-pointer maps, record counter) is unchanged, skip the rewrite - it
    // dominated shutdown time on large read-mostly stores
    if (FAccess == faRdOnly) {
        TEnv::Logger->OnStatus("No saving of generic store " + GetStoreNm() + " neccessary!");
    } else if (!MetaDirtyP) {
        TEnv::Logger->OnStatus(TStr::Fmt("Store '%s' metadata unchanged - not saving", GetStoreNm().CStr()));
    } else {
        TEnv::Logger->OnStatus(TStr::Fmt("Saving store '%s'...", GetStoreNm().CStr()));
        // save base store
        TFOut BaseFOut(StoreFNm + ".BaseStore");
        SaveStore(BaseFOut);
        // save store parameters
        TFOut FOut(StoreFNm + "PgBlobStore");
        // save parameters about primary field
        RecNmFieldP.Save(FOut);
        PrimaryFieldId.Save(FOut);
        if (PrimaryFieldType == oftInt) {
            PrimaryIntIdH.Save(FOut);
        } else if (PrimaryFieldType == oftUInt64) {
            PrimaryUInt64IdH.Save(FOut);
        } else if (PrimaryFieldType == oftFlt) {
            PrimaryFltIdH.Save(FOut);
        } else if (PrimaryFieldType == oftTm) {
            PrimaryTmMSecsIdH.Save(FOut);
        } else {
            PrimaryStrIdH.Save(FOut);
        }
        // save time window
        WndDesc.Save(FOut);
        // save data
        SerializatorCache->Save(FOut);
        SerializatorMem->Save(FOut);

        RecIdBlobPtH.Save(FOut);
        RecIdBlobPtHMem.Save(FOut);
        RecIdCounter.Save(FOut);
    }
}

/// Store value into internal storage using TOAST method
template <class TRecPtMap>
TPgBlobPt TStorePbBlobT<TRecPtMap>::ToastVal(const TMemBase& Mem) {
    return ToastValToBlob(ToastWriteRedirectBlob.Empty() ? DataBlob : ToastWriteRedirectBlob, Mem);
}

/// Retrieve value that is saved using TOAST method from storage
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::UnToastVal(const TPgBlobPt& Pt, TMem& Mem) {
    UnToastValFromBlob(ToastReadRedirectBlob.Empty() ? DataBlob : ToastReadRedirectBlob, Pt, Mem);
}

/// Store value into the given page blob using the TOAST method
template <class TRecPtMap>
TPgBlobPt TStorePbBlobT<TRecPtMap>::ToastValToBlob(const PPgBlob& Blob, const TMemBase& Mem) {
    TVec<TPgBlobPt> Pts;
    int BlockLen = Blob->GetMxBlobLen();
    int curr_index = 0;
    while (curr_index < Mem.Len()) {
        int curr_len = MIN(BlockLen, Mem.Len() - curr_index);
        TPgBlobPt PtTmp = Blob->Put(Mem.GetBf() + curr_index, curr_len);
        Pts.Add(PtTmp);
        curr_index += curr_len;
    }
    TMOut SOut;
    Pts.Save(SOut);
    return Blob->Put(SOut.GetBfAddr(), SOut.Len());
}

/// Retrieve a TOAST-ed value from the given page blob
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::UnToastValFromBlob(const PPgBlob& Blob, const TPgBlobPt& Pt, TMem& Mem) {
    TVec<TPgBlobPt> Pts;
    TThinMIn MIn = Blob->Get(Pt);
    Pts.Load(MIn);
    Mem.Clr();
    for (int i = 0; i < Pts.Len(); i++) {
        TMemBase MemTmp = Blob->GetMemBase(Pts[i]);
        Mem.AddBf(MemTmp.GetBf(), MemTmp.Len());
    }
}

/// Copy one record's serialized data into NewBlob, relocating its TOAST-ed
/// values into NewToastBlob. The copied data is read back and verified.
template <class TRecPtMap>
TPgBlobPt TStorePbBlobT<TRecPtMap>::CopyRecToBlob(const uint64& RecId, const bool& UseMem,
        const PPgBlob& NewToastBlob, const PPgBlob& NewBlob) {

    // fetch the serialized record and copy it into a local buffer
    const TPgBlobPt& OldPt = UseMem ? RecIdBlobPtHMem.GetDat(RecId) : RecIdBlobPtH.GetDat(RecId);
    TMemBase OldRecMemBase = UseMem ? DataMem->GetMemBase(OldPt) : DataBlob->GetMemBase(OldPt);
    TMem RecMem; RecMem.AddBf(OldRecMemBase.GetBf(), OldRecMemBase.Len());

    // relocate TOAST-ed values. TOAST-ed values always live in the disk blob
    // (DataBlob), also for fields of records stored in the in-memory blob
    const TRecSerializator* Serializator = UseMem ? SerializatorMem : SerializatorCache;
    TIntV PtOffsetV; Serializator->GetToastBlobPtOffsets(RecMem, PtOffsetV);
    for (int PtOffsetN = 0; PtOffsetN < PtOffsetV.Len(); PtOffsetN++) {
        char* PtBf = RecMem.GetBf() + PtOffsetV[PtOffsetN];
        const TPgBlobPt OldToastPt = *((TPgBlobPt*) PtBf);
        // read the value from the old blob and store it into the new one
        TMem ToastMem; UnToastVal(OldToastPt, ToastMem);
        const TPgBlobPt NewToastPt = ToastValToBlob(NewToastBlob, ToastMem);
        // verify the relocated value reads back the same
        TMem NewToastMem; UnToastValFromBlob(NewToastBlob, NewToastPt, NewToastMem);
        EAssertR(ToastMem.Len() == NewToastMem.Len() && (ToastMem.Len() == 0 ||
            memcmp(ToastMem.GetBf(), NewToastMem.GetBf(), ToastMem.Len()) == 0), TStr::Fmt(
            "[TStorePbBlob::CopyRecToBlob] TOAST-ed value mismatch for record %s in store %s",
            TUInt64::GetStr(RecId).CStr(), GetStoreNm().CStr()));
        // point the record to the relocated value
        *((TPgBlobPt*) PtBf) = NewToastPt;
    }

    // store the record into the destination blob and verify it reads back the same
    const TPgBlobPt NewPt = NewBlob->Put(RecMem.GetBf(), RecMem.Len());
    TMemBase NewRecMemBase = NewBlob->GetMemBase(NewPt);
    EAssertR(NewRecMemBase.Len() == RecMem.Len() &&
        memcmp(NewRecMemBase.GetBf(), RecMem.GetBf(), RecMem.Len()) == 0, TStr::Fmt(
        "[TStorePbBlob::CopyRecToBlob] record data mismatch for record %s in store %s",
        TUInt64::GetStr(RecId).CStr(), GetStoreNm().CStr()));
    return NewPt;
}

/// Rebuild (defragment) the record blobs into new page blob files
template <class TRecPtMap>
uint64 TStorePbBlobT<TRecPtMap>::DefragTo(const TStr& DestStoreFNm, const uint64& CacheSize) {
    TEnv::Logger->OnStatus(TStr::Fmt("Defragmenting store '%s'...", GetStoreNm().CStr()));
    // create the destination page blobs
    PPgBlob NewDataBlob = PPgBlob(new TPgBlob(DestStoreFNm + "PgBlob", TFAccess::faCreate, CacheSize));
    PPgBlob NewDataMem = PPgBlob(new TPgBlob(DestStoreFNm + "PgBlobMem", TFAccess::faCreate, CacheSize));
    TRecPtMap NewRecIdBlobPtH;
    TRecPtMap NewRecIdBlobPtHMem;

    // --- source-side accounting, so any record-count change after the rebuild
    // can be explained instead of discovered later as a shrunken store ---
    const int SrcBlobRecs = RecIdBlobPtH.Len();
    const int SrcMemRecs = RecIdBlobPtHMem.Len();
    int SrcPrimaryRecs = -1;
    if (PrimaryFieldId != -1) {
        if (PrimaryFieldType == oftInt) { SrcPrimaryRecs = PrimaryIntIdH.Len(); }
        else if (PrimaryFieldType == oftUInt64) { SrcPrimaryRecs = PrimaryUInt64IdH.Len(); }
        else if (PrimaryFieldType == oftFlt) { SrcPrimaryRecs = PrimaryFltIdH.Len(); }
        else if (PrimaryFieldType == oftTm) { SrcPrimaryRecs = PrimaryTmMSecsIdH.Len(); }
        else if (PrimaryFieldType == oftStr) { SrcPrimaryRecs = PrimaryStrIdH.Len(); }
    }
    TEnv::Logger->OnStatus(TStr::Fmt(
        "[Defrag] store '%s' source counts: GetRecs=%s, blob-hash=%s, mem-hash=%s, primary-hash=%s, RecIdCounter=%s",
        GetStoreNm().CStr(), TStrUtil::GetStr(GetRecs()).CStr(), TStrUtil::GetStr(SrcBlobRecs).CStr(), TStrUtil::GetStr(SrcMemRecs).CStr(),
        SrcPrimaryRecs >= 0 ? TStrUtil::GetStr(SrcPrimaryRecs).CStr() : "n/a",
        TStrUtil::GetStr(RecIdCounter).CStr()));

    // collect the record ids and sort them, so the records are written in
    // ascending record id order. Records keep their ids - gaps left by deleted
    // records are preserved, so the ids stay valid for the index.
    // For stores with BOTH a disk and an in-memory section the two id hashes
    // must agree; iterate their UNION so a desynced record is copied (for the
    // sections that have it) and REPORTED, instead of silently dropped (id only
    // in the blob hash) or crashing GetDat (id only in the mem hash), which is
    // what iterating a single hash did.
    TUInt64V RecIdV;
    int BlobOnlyRecs = 0, MemOnlyRecs = 0;
    TUInt64V BlobOnlySampleV, MemOnlySampleV;
    const int MxSamples = 10;
    CollectRebuildRecIdV(RecIdV, BlobOnlyRecs, MemOnlyRecs, BlobOnlySampleV, MemOnlySampleV, MxSamples);

    for (int RecN = 0; RecN < RecIdV.Len(); RecN++) {
        const uint64 RecId = RecIdV[RecN];
        if (DataBlobP && RecIdBlobPtH.IsKey(RecId)) {
            NewRecIdBlobPtH.AddDat(RecId, CopyRecToBlob(RecId, false, NewDataBlob, NewDataBlob));
        }
        if (DataMemP && RecIdBlobPtHMem.IsKey(RecId)) {
            NewRecIdBlobPtHMem.AddDat(RecId, CopyRecToBlob(RecId, true, NewDataBlob, NewDataMem));
        }
        if (RecN % 100000 == 0) {
            printf("%d / %d records copied (%.1f%%)\r", RecN, RecIdV.Len(),
                RecIdV.Len() > 0 ? 100.0 * RecN / RecIdV.Len() : 100.0);
        }
    }
    printf("%d / %d records copied (100.0%%)\n", RecIdV.Len(), RecIdV.Len());

    // --- cross-check the primary-key hash: every entry must point at a live
    // record id; dangling entries make by-name lookups and primary-based counts
    // disagree with the record hashes (a further source of "count changed") ---
    int DanglingPrimaryRecs = 0;
    TUInt64V DanglingPrimarySampleV;
    if (PrimaryFieldId != -1) {
        #define QM_DEFRAG_CHECK_PRIMARY(PrimaryH) { \
            int KeyId = PrimaryH.FFirstKeyId(); \
            while (PrimaryH.FNextKeyId(KeyId)) { \
                const uint64 PrimaryRecId = PrimaryH[KeyId]; \
                const bool LiveP = (DataBlobP && RecIdBlobPtH.IsKey(PrimaryRecId)) || \
                                   (DataMemP && RecIdBlobPtHMem.IsKey(PrimaryRecId)); \
                if (!LiveP) { \
                    DanglingPrimaryRecs++; \
                    if (DanglingPrimarySampleV.Len() < MxSamples) { DanglingPrimarySampleV.Add(PrimaryRecId); } \
                } \
            } }
        if (PrimaryFieldType == oftInt) { QM_DEFRAG_CHECK_PRIMARY(PrimaryIntIdH); }
        else if (PrimaryFieldType == oftUInt64) { QM_DEFRAG_CHECK_PRIMARY(PrimaryUInt64IdH); }
        else if (PrimaryFieldType == oftFlt) { QM_DEFRAG_CHECK_PRIMARY(PrimaryFltIdH); }
        else if (PrimaryFieldType == oftTm) { QM_DEFRAG_CHECK_PRIMARY(PrimaryTmMSecsIdH); }
        else if (PrimaryFieldType == oftStr) { QM_DEFRAG_CHECK_PRIMARY(PrimaryStrIdH); }
        #undef QM_DEFRAG_CHECK_PRIMARY
    }

    // --- rebuilt-side accounting: report and explain every difference ---
    const auto SampleStr = [](const TUInt64V& SampleV) {
        TChA ChA;
        for (int SampleN = 0; SampleN < SampleV.Len(); SampleN++) {
            if (SampleN > 0) { ChA += ", "; }
            ChA += TUInt64::GetStr(SampleV[SampleN]);
        }
        return TStr(ChA);
    };
    TEnv::Logger->OnStatus(TStr::Fmt(
        "[Defrag] store '%s' rebuilt counts: blob-hash %s -> %s, mem-hash %s -> %s",
        GetStoreNm().CStr(), TStrUtil::GetStr(SrcBlobRecs).CStr(), TStrUtil::GetStr((uint64)NewRecIdBlobPtH.Len()).CStr(),
        TStrUtil::GetStr(SrcMemRecs).CStr(), TStrUtil::GetStr((uint64)NewRecIdBlobPtHMem.Len()).CStr()));
    if (DataBlobP && DataMemP && SrcBlobRecs != SrcMemRecs) {
        TEnv::Logger->OnStatus(TStr::Fmt(
            "[Defrag] WARNING: store '%s' disk and in-memory sections disagree: %s records have only a disk part "
            "(previously silently dropped by defrag; sample ids: %s), %s records have only an in-memory part "
            "(previously crashed/corrupted the rebuild; sample ids: %s). Such records are typically left behind "
            "by a partial delete or an interrupted add; their sections were copied as-is - inspect the sample ids "
            "to decide whether they are live or garbage.",
            GetStoreNm().CStr(), TStrUtil::GetStr(BlobOnlyRecs).CStr(), SampleStr(BlobOnlySampleV).CStr(),
            TStrUtil::GetStr(MemOnlyRecs).CStr(), SampleStr(MemOnlySampleV).CStr()));
    }
    if (DanglingPrimaryRecs > 0) {
        TEnv::Logger->OnStatus(TStr::Fmt(
            "[Defrag] WARNING: store '%s' primary-key hash has %s entries pointing at record ids with no data "
            "(sample ids: %s). Counts based on the primary hash (%s) will disagree with the record hashes; "
            "these entries are typically left behind when a record delete removed the data but not the "
            "primary-key entry.",
            GetStoreNm().CStr(), TStrUtil::GetStr(DanglingPrimaryRecs).CStr(), SampleStr(DanglingPrimarySampleV).CStr(),
            TStrUtil::GetStr(SrcPrimaryRecs).CStr()));
    }
    if (NewRecIdBlobPtH.Len() != SrcBlobRecs || NewRecIdBlobPtHMem.Len() != SrcMemRecs) {
        TEnv::Logger->OnStatus(TStr::Fmt(
            "[Defrag] WARNING: store '%s' rebuilt record count differs from the source - see the reasons above. "
            "The rebuilt store keeps every record that had data in at least one section.",
            GetStoreNm().CStr()));
    }

    // write the destination store state file - the format and content must match
    // what the destructor writes, with the new record-id-to-blob-pointer maps
    TFOut FOut(DestStoreFNm + "PgBlobStore");
    RecNmFieldP.Save(FOut);
    PrimaryFieldId.Save(FOut);
    if (PrimaryFieldType == oftInt) {
        PrimaryIntIdH.Save(FOut);
    } else if (PrimaryFieldType == oftUInt64) {
        PrimaryUInt64IdH.Save(FOut);
    } else if (PrimaryFieldType == oftFlt) {
        PrimaryFltIdH.Save(FOut);
    } else if (PrimaryFieldType == oftTm) {
        PrimaryTmMSecsIdH.Save(FOut);
    } else {
        PrimaryStrIdH.Save(FOut);
    }
    WndDesc.Save(FOut);
    SerializatorCache->Save(FOut);
    SerializatorMem->Save(FOut);
    NewRecIdBlobPtH.Save(FOut);
    NewRecIdBlobPtHMem.Save(FOut);
    RecIdCounter.Save(FOut);

    TEnv::Logger->OnStatus(TStr::Fmt("Defragmenting store '%s' done: %d records", GetStoreNm().CStr(), RecIdV.Len()));
    // releasing the new page blobs flushes them to disk
    return (uint64) RecIdV.Len();
}

/// Collect the union of record ids of the disk and in-memory sections in ascending order
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::CollectRebuildRecIdV(TUInt64V& RecIdV, int& BlobOnlyRecs, int& MemOnlyRecs,
        TUInt64V& BlobOnlySampleV, TUInt64V& MemOnlySampleV, const int& MxSamples) const {
    RecIdV.Clr();
    BlobOnlyRecs = 0; MemOnlyRecs = 0;
    BlobOnlySampleV.Clr(); MemOnlySampleV.Clr();
    if (DataBlobP && DataMemP) {
        TUInt64V BlobIdV; RecIdBlobPtH.GetKeyV(BlobIdV); BlobIdV.Sort();
        TUInt64V MemIdV; RecIdBlobPtHMem.GetKeyV(MemIdV); MemIdV.Sort();
        RecIdV.Gen(TInt::GetMx(BlobIdV.Len(), MemIdV.Len()), 0);
        int BlobN = 0, MemN = 0;
        while (BlobN < BlobIdV.Len() || MemN < MemIdV.Len()) {
            const bool TakeBlob = MemN >= MemIdV.Len() || (BlobN < BlobIdV.Len() && BlobIdV[BlobN] <= MemIdV[MemN]);
            const bool TakeMem = BlobN >= BlobIdV.Len() || (MemN < MemIdV.Len() && MemIdV[MemN] <= BlobIdV[BlobN]);
            const uint64 RecId = TakeBlob ? BlobIdV[BlobN] : MemIdV[MemN];
            if (TakeBlob && !TakeMem) {
                BlobOnlyRecs++;
                if (BlobOnlySampleV.Len() < MxSamples) { BlobOnlySampleV.Add(RecId); }
            } else if (TakeMem && !TakeBlob) {
                MemOnlyRecs++;
                if (MemOnlySampleV.Len() < MxSamples) { MemOnlySampleV.Add(RecId); }
            }
            RecIdV.Add(RecId);
            if (TakeBlob) { BlobN++; }
            if (TakeMem) { MemN++; }
        }
    } else if (DataMemP) {
        RecIdBlobPtHMem.GetKeyV(RecIdV); RecIdV.Sort();
    } else {
        RecIdBlobPtH.GetKeyV(RecIdV); RecIdV.Sort();
    }
}

/// Verify one section of a record rebuilt by MigrateSchemaTo
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::VerifyMigratedRec(const uint64& RecId, const TRecSerializator& NewSer,
        const TMemBase& NewRec, const TMemBase& OldMemRec, const TMemBase& OldCacheRec,
        const PPgBlob& NewToastBlob, const TIntV& OldToNewFieldIdV) {

    for (int FieldId = 0; FieldId < GetFields(); FieldId++) {
        // the rebuilt store's id of this field (they differ when the migration
        // dropped fields); dropped fields have no new value to verify
        const int NewFieldId = OldToNewFieldIdV.Empty() ? FieldId : OldToNewFieldIdV[FieldId].Val;
        if (NewFieldId == -1 || !NewSer.IsFieldId(NewFieldId)) { continue; }
        const TFieldDesc& FieldDesc = GetFieldDesc(FieldId);
        // the source serializator/section the field was written with (old layout)
        const TRecSerializator* OldSer = GetFieldSerializator(FieldId);
        const TMemBase& OldRec = (OldSer == SerializatorMem) ? OldMemRec : OldCacheRec;

        // null flags must agree
        const bool OldNullP = OldSer->IsFieldNull(OldRec, FieldId);
        const bool NewNullP = NewSer.IsFieldNull(NewRec, NewFieldId);
        EAssertR(OldNullP == NewNullP, TStr::Fmt(
            "[MigrateSchemaTo] null-flag mismatch for field %s of record %s in store %s",
            FieldDesc.GetFieldNm().CStr(), TUInt64::GetStr(RecId).CStr(), GetStoreNm().CStr()));
        if (OldNullP) { continue; }

        // read the old value from the live blobs and the new value from the rebuilt
        // blob (TOAST-ed values of the rebuilt record live in NewToastBlob) and
        // compare them. EQ evaluates OldExpr with TOAST reads from the live blob
        // and NewExpr with TOAST reads redirected to the rebuilt blob.
        #define QM_MIGRATE_FIELD_EQ(OldExpr, NewExpr, EqExpr) { \
            ToastReadRedirectBlob = NULL; \
            const auto OldVal = (OldExpr); (void) OldVal; \
            ToastReadRedirectBlob = NewToastBlob; \
            const auto NewVal = (NewExpr); (void) NewVal; \
            ToastReadRedirectBlob = NULL; \
            EAssertR((EqExpr), TStr::Fmt( \
                "[MigrateSchemaTo] value mismatch for field %s of record %s in store %s", \
                FieldDesc.GetFieldNm().CStr(), TUInt64::GetStr(RecId).CStr(), GetStoreNm().CStr())); }
        #define QM_MIGRATE_FIELD_EQ_SIMPLE(Getter) \
            QM_MIGRATE_FIELD_EQ(OldSer->Getter(OldRec, FieldId), NewSer.Getter(NewRec, NewFieldId), OldVal == NewVal)

        switch (FieldDesc.GetFieldType()) {
            case oftByte: QM_MIGRATE_FIELD_EQ_SIMPLE(GetFieldByte); break;
            case oftInt: QM_MIGRATE_FIELD_EQ_SIMPLE(GetFieldInt); break;
            case oftInt16: QM_MIGRATE_FIELD_EQ_SIMPLE(GetFieldInt16); break;
            case oftInt64: QM_MIGRATE_FIELD_EQ_SIMPLE(GetFieldInt64); break;
            case oftUInt: QM_MIGRATE_FIELD_EQ_SIMPLE(GetFieldUInt); break;
            case oftUInt16: QM_MIGRATE_FIELD_EQ_SIMPLE(GetFieldUInt16); break;
            case oftUInt64: QM_MIGRATE_FIELD_EQ_SIMPLE(GetFieldUInt64); break;
            case oftStr: QM_MIGRATE_FIELD_EQ_SIMPLE(GetFieldStr); break;
            case oftBool: QM_MIGRATE_FIELD_EQ_SIMPLE(GetFieldBool); break;
            case oftFlt: QM_MIGRATE_FIELD_EQ_SIMPLE(GetFieldFlt); break;
            case oftSFlt: QM_MIGRATE_FIELD_EQ_SIMPLE(GetFieldSFlt); break;
            case oftFltPr: QM_MIGRATE_FIELD_EQ_SIMPLE(GetFieldFltPr); break;
            case oftTm: QM_MIGRATE_FIELD_EQ_SIMPLE(GetFieldTmMSecs); break;
            case oftIntV: {
                TIntV OldV; TIntV NewV;
                QM_MIGRATE_FIELD_EQ((OldSer->GetFieldIntV(OldRec, FieldId, OldV), 0), (NewSer.GetFieldIntV(NewRec, NewFieldId, NewV), 0), OldV == NewV);
                break;
            }
            case oftStrV: {
                TStrV OldV; TStrV NewV;
                QM_MIGRATE_FIELD_EQ((OldSer->GetFieldStrV(OldRec, FieldId, OldV), 0), (NewSer.GetFieldStrV(NewRec, NewFieldId, NewV), 0), OldV == NewV);
                break;
            }
            case oftFltV: {
                TFltV OldV; TFltV NewV;
                QM_MIGRATE_FIELD_EQ((OldSer->GetFieldFltV(OldRec, FieldId, OldV), 0), (NewSer.GetFieldFltV(NewRec, NewFieldId, NewV), 0), OldV == NewV);
                break;
            }
            case oftNumSpV: {
                TIntFltKdV OldV; TIntFltKdV NewV;
                QM_MIGRATE_FIELD_EQ((OldSer->GetFieldNumSpV(OldRec, FieldId, OldV), 0), (NewSer.GetFieldNumSpV(NewRec, NewFieldId, NewV), 0), OldV == NewV);
                break;
            }
            case oftBowSpV: {
                // compare the serialized form - PBowSpV has no value comparison
                PBowSpV OldV; PBowSpV NewV;
                QM_MIGRATE_FIELD_EQ((OldSer->GetFieldBowSpV(OldRec, FieldId, OldV), 0), (NewSer.GetFieldBowSpV(NewRec, NewFieldId, NewV), 0), 0 == 0);
                TMOut OldMOut; OldV->Save(OldMOut);
                TMOut NewMOut; NewV->Save(NewMOut);
                EAssertR(OldMOut.Len() == NewMOut.Len() && (OldMOut.Len() == 0 ||
                    memcmp(OldMOut.GetBfAddr(), NewMOut.GetBfAddr(), OldMOut.Len()) == 0), TStr::Fmt(
                    "[MigrateSchemaTo] value mismatch for field %s of record %s in store %s",
                    FieldDesc.GetFieldNm().CStr(), TUInt64::GetStr(RecId).CStr(), GetStoreNm().CStr()));
                break;
            }
            case oftTMem: {
                TMem OldV; TMem NewV;
                QM_MIGRATE_FIELD_EQ((OldSer->GetFieldTMem(OldRec, FieldId, OldV), 0), (NewSer.GetFieldTMem(NewRec, NewFieldId, NewV), 0),
                    OldV.Len() == NewV.Len() && (OldV.Len() == 0 || memcmp(OldV.GetBf(), NewV.GetBf(), OldV.Len()) == 0));
                break;
            }
            case oftJson: {
                QM_MIGRATE_FIELD_EQ(TJsonVal::GetStrFromVal(OldSer->GetFieldJsonVal(OldRec, FieldId)),
                    TJsonVal::GetStrFromVal(NewSer.GetFieldJsonVal(NewRec, NewFieldId)), OldVal == NewVal);
                break;
            }
            default: throw TQmExcept::New("[MigrateSchemaTo] unsupported field type " +
                FieldDesc.GetFieldTypeStr() + " for field " + FieldDesc.GetFieldNm());
        }
        #undef QM_MIGRATE_FIELD_EQ_SIMPLE
        #undef QM_MIGRATE_FIELD_EQ
    }
}

/// Rebuild the record blobs with the field placement of NewSchema; the rebuilt
/// record maps use the representation the schema's "recIdMap" option selects
template <class TRecPtMap>
uint64 TStorePbBlobT<TRecPtMap>::MigrateSchemaTo(const TStr& DestStoreFNm, const TStoreSchema& NewSchema,
        const uint64& CacheSize, TIntV& OldToNewFieldIdVOut) {
    return NewSchema.DenseRecIdMapP ?
        MigrateSchemaToT<TPbBlobRecMapDense>(DestStoreFNm, NewSchema, CacheSize, OldToNewFieldIdVOut) :
        MigrateSchemaToT<TPbBlobRecMapHash>(DestStoreFNm, NewSchema, CacheSize, OldToNewFieldIdVOut);
}

/// MigrateSchemaTo body, parameterized on the rebuilt record-map representation
template <class TRecPtMap>
template <class TDstMap>
uint64 TStorePbBlobT<TRecPtMap>::MigrateSchemaToT(const TStr& DestStoreFNm, const TStoreSchema& NewSchema,
        const uint64& CacheSize, TIntV& OldToNewFieldIdVOut) {

    TEnv::Logger->OnStatus(TStr::Fmt("Migrating store '%s' to the new schema field placement...", GetStoreNm().CStr()));
    if (GetStoreType() != TDstMap::GetStoreTypeNm()) {
        TEnv::Logger->OnStatus(TStr::Fmt(
            "[Migrate] store '%s' also converts its record-map type: %s -> %s (per the schema's recIdMap option)",
            GetStoreNm().CStr(), GetStoreType().CStr(), TDstMap::GetStoreTypeNm()));
    }

    // validate the new schema against this store's fields. The migration
    // relocates values between the two sections and may DROP fields the schema
    // no longer defines; it can never add or retype fields (there would be no
    // values for them). Every schema field must therefore exist in the store
    // with the same type.
    QmAssertR(NewSchema.StoreName == GetStoreNm(), "[MigrateSchemaTo] the schema is for store " +
        NewSchema.StoreName + ", not for store " + GetStoreNm());
    for (int SchemaFieldN = 0; SchemaFieldN < NewSchema.FieldH.Len(); SchemaFieldN++) {
        const TFieldDesc& SchemaFieldDesc = NewSchema.FieldH[SchemaFieldN];
        const TStr& FieldNm = SchemaFieldDesc.GetFieldNm();
        QmAssertR(IsFieldNm(FieldNm), "[MigrateSchemaTo] the new schema adds field " + FieldNm +
            " that store " + GetStoreNm() + " does not have - fields cannot be added by the migration");
        QmAssertR(GetFieldDesc(GetFieldId(FieldNm)).GetFieldType() == SchemaFieldDesc.GetFieldType(),
            "[MigrateSchemaTo] field " + FieldNm + " of store " + GetStoreNm() + " changes type in the new schema");
    }

    // collect the fields the new schema drops and build the field id mapping
    // (dropping renumbers the ids of the remaining fields)
    TIntV OldToNewFieldIdV; TIntV NewToOldFieldIdV; TStrV DroppedFieldNmV;
    for (int FieldId = 0; FieldId < GetFields(); FieldId++) {
        const TFieldDesc& FieldDesc = GetFieldDesc(FieldId);
        if (NewSchema.FieldH.IsKey(FieldDesc.GetFieldNm())) {
            QmAssertR(NewSchema.FieldExH.IsKey(FieldDesc.GetFieldNm()),
                "[MigrateSchemaTo] field " + FieldDesc.GetFieldNm() + " has no extended description in the new schema");
            OldToNewFieldIdV.Add(NewToOldFieldIdV.Add(FieldId));
        } else {
            OldToNewFieldIdV.Add(-1);
            DroppedFieldNmV.Add(FieldDesc.GetFieldNm());
        }
    }
    const bool DropFieldsP = !DroppedFieldNmV.Empty();
    if (DropFieldsP) {
        // a dropped field must not be load-bearing: not the primary field, not
        // the record/frequency field of a field join and not linked to an index
        // key (the first two would lose data the store needs, the last would
        // require reindexing; index keys on OTHER fields survive the renumbering
        // via TIndexVoc::RemapStoreFieldIds, which the caller must invoke)
        for (int FieldId = 0; FieldId < GetFields(); FieldId++) {
            if (OldToNewFieldIdV[FieldId] != -1) { continue; }
            const TFieldDesc& FieldDesc = GetFieldDesc(FieldId);
            QmAssertR(FieldId != PrimaryFieldId,
                "[MigrateSchemaTo] cannot drop the primary field " + FieldDesc.GetFieldNm() + " of store " + GetStoreNm());
            QmAssertR(!FieldDesc.IsKeys(), "[MigrateSchemaTo] cannot drop field " + FieldDesc.GetFieldNm() +
                " of store " + GetStoreNm() + " - it is linked to an index key");
            for (int JoinId = 0; JoinId < GetJoins(); JoinId++) {
                const TJoinDesc& JoinDesc = GetJoinDesc(JoinId);
                QmAssertR(!JoinDesc.IsFieldJoin() || (JoinDesc.GetJoinRecFieldId() != FieldId &&
                    JoinDesc.GetJoinFqFieldId() != FieldId), "[MigrateSchemaTo] cannot drop field " +
                    FieldDesc.GetFieldNm() + " of store " + GetStoreNm() + " - it carries the field join " + JoinDesc.GetJoinNm());
            }
        }
        TStr DroppedStr;
        for (int DroppedN = 0; DroppedN < DroppedFieldNmV.Len(); DroppedN++) {
            if (DroppedN > 0) { DroppedStr += ", "; }
            DroppedStr += DroppedFieldNmV[DroppedN];
        }
        TEnv::Logger->OnStatus(TStr::Fmt(
            "[Migrate] store '%s' DROPS %d field(s) the new schema no longer defines: %s "
            "(their values are not copied into the rebuilt store; the remaining field ids are renumbered)",
            GetStoreNm().CStr(), DroppedFieldNmV.Len(), DroppedStr.CStr()));
    }

    // the rebuilt store's field table: the remaining fields in their current
    // order, renumbered to consecutive ids (identical to the current table when
    // nothing is dropped)
    TFieldDescV NewFieldDescV;
    for (int FieldId = 0; FieldId < GetFields(); FieldId++) {
        if (OldToNewFieldIdV[FieldId] == -1) { continue; }
        const int NewFieldId = NewFieldDescV.Add(GetFieldDesc(FieldId));
        EAssert(NewFieldId == OldToNewFieldIdV[FieldId]);
        NewFieldDescV[NewFieldId].PutFieldId(NewFieldId);
    }

    // build the target serializators; the constructor takes each field's section
    // (memory vs cache) from the new schema; without dropped fields the ids stay
    // those of this store, so the index, joins and the .BaseStore file remain valid
    TRecSerializator NewSerCache(this, NewFieldDescV, NewSchema, slDisk);
    TRecSerializator NewSerMem(this, NewFieldDescV, NewSchema, slMemory);
    const bool NewCacheUsedP = !NewSerCache.IsEmpty();
    const bool NewMemUsedP = !NewSerMem.IsEmpty();
    // id maps for the record copy/verify loop; empty means identity (no renumbering)
    TIntV CopyNewToOldFieldIdV; TIntV CopyOldToNewFieldIdV;
    if (DropFieldsP) {
        CopyNewToOldFieldIdV = NewToOldFieldIdV;
        CopyOldToNewFieldIdV = OldToNewFieldIdV;
    }

    // create the destination page blobs
    PPgBlob NewDataBlob = PPgBlob(new TPgBlob(DestStoreFNm + "PgBlob", TFAccess::faCreate, CacheSize));
    PPgBlob NewDataMem = PPgBlob(new TPgBlob(DestStoreFNm + "PgBlobMem", TFAccess::faCreate, CacheSize));
    TDstMap NewRecIdBlobPtH;
    TDstMap NewRecIdBlobPtHMem;

    // source-side accounting (see DefragTo for the reasoning)
    const int SrcBlobRecs = RecIdBlobPtH.Len();
    const int SrcMemRecs = RecIdBlobPtHMem.Len();
    TEnv::Logger->OnStatus(TStr::Fmt(
        "[Migrate] store '%s' source counts: GetRecs=%s, blob-hash=%s, mem-hash=%s, RecIdCounter=%s",
        GetStoreNm().CStr(), TStrUtil::GetStr(GetRecs()).CStr(), TStrUtil::GetStr(SrcBlobRecs).CStr(),
        TStrUtil::GetStr(SrcMemRecs).CStr(), TStrUtil::GetStr(RecIdCounter).CStr()));

    // collect the union of record ids of both sections in ascending id order
    TUInt64V RecIdV;
    int BlobOnlyRecs = 0, MemOnlyRecs = 0;
    TUInt64V BlobOnlySampleV, MemOnlySampleV;
    const int MxSamples = 10;
    CollectRebuildRecIdV(RecIdV, BlobOnlyRecs, MemOnlyRecs, BlobOnlySampleV, MemOnlySampleV, MxSamples);

    // re-serialize the records; TOAST-ed values written along the way must go
    // into the rebuilt disk blob while reads keep coming from the live blobs
    ToastWriteRedirectBlob = NewDataBlob;
    uint64 WrittenRecs = 0; int DroppedRecs = 0;
    try {
        for (int RecN = 0; RecN < RecIdV.Len(); RecN++) {
            const uint64 RecId = RecIdV[RecN];
            const bool InBlobP = DataBlobP && RecIdBlobPtH.IsKey(RecId);
            const bool InMemP = DataMemP && RecIdBlobPtHMem.IsKey(RecId);
            // a record missing one of its sections cannot be re-serialized - the
            // values of the missing section's fields are gone; drop it (reported below)
            if ((DataBlobP && !InBlobP) || (DataMemP && !InMemP)) { DroppedRecs++; continue; }

            // copy both sections into local buffers - the getters below load other
            // pages (TOAST-ed values), which may evict the record's own page
            TMem OldCacheMem;
            if (InBlobP) {
                TMemBase MemBase = DataBlob->GetMemBase(RecIdBlobPtH.GetDat(RecId));
                OldCacheMem.AddBf(MemBase.GetBf(), MemBase.Len());
            }
            TMem OldMemMem;
            if (InMemP) {
                TMemBase MemBase = DataMem->GetMemBase(RecIdBlobPtHMem.GetDat(RecId));
                OldMemMem.AddBf(MemBase.GetBf(), MemBase.Len());
            }

            // rebuild and verify the disk section
            if (NewCacheUsedP) {
                TMem NewRecMem;
                NewSerCache.SerializeCopyRec(this, *SerializatorMem, *SerializatorCache, OldMemMem, OldCacheMem, NewRecMem, CopyNewToOldFieldIdV);
                const TPgBlobPt NewPt = NewDataBlob->Put(NewRecMem.GetBf(), NewRecMem.Len());
                NewRecIdBlobPtH.AddDat(RecId, NewPt);
                // copy the stored record out of the page cache before verifying -
                // the verification reads TOAST-ed values, which may evict its page
                TMem StoredRecMem;
                { TMemBase MemBase = NewDataBlob->GetMemBase(NewPt); StoredRecMem.AddBf(MemBase.GetBf(), MemBase.Len()); }
                VerifyMigratedRec(RecId, NewSerCache, StoredRecMem, OldMemMem, OldCacheMem, NewDataBlob, CopyOldToNewFieldIdV);
            }
            // rebuild and verify the in-memory section
            if (NewMemUsedP) {
                TMem NewRecMem;
                NewSerMem.SerializeCopyRec(this, *SerializatorMem, *SerializatorCache, OldMemMem, OldCacheMem, NewRecMem, CopyNewToOldFieldIdV);
                const TPgBlobPt NewPt = NewDataMem->Put(NewRecMem.GetBf(), NewRecMem.Len());
                NewRecIdBlobPtHMem.AddDat(RecId, NewPt);
                TMem StoredRecMem;
                { TMemBase MemBase = NewDataMem->GetMemBase(NewPt); StoredRecMem.AddBf(MemBase.GetBf(), MemBase.Len()); }
                VerifyMigratedRec(RecId, NewSerMem, StoredRecMem, OldMemMem, OldCacheMem, NewDataBlob, CopyOldToNewFieldIdV);
            }
            WrittenRecs++;
            if (RecN % 100000 == 0) {
                printf("%d / %d records migrated (%.1f%%)\r", RecN, RecIdV.Len(),
                    RecIdV.Len() > 0 ? 100.0 * RecN / RecIdV.Len() : 100.0);
            }
        }
    } catch (...) {
        ToastWriteRedirectBlob = NULL;
        ToastReadRedirectBlob = NULL;
        throw;
    }
    ToastWriteRedirectBlob = NULL;
    printf("%d / %d records migrated (100.0%%)\n", RecIdV.Len(), RecIdV.Len());

    // report and explain every difference
    const auto SampleStr = [](const TUInt64V& SampleV) {
        TChA ChA;
        for (int SampleN = 0; SampleN < SampleV.Len(); SampleN++) {
            if (SampleN > 0) { ChA += ", "; }
            ChA += TUInt64::GetStr(SampleV[SampleN]);
        }
        return TStr(ChA);
    };
    TEnv::Logger->OnStatus(TStr::Fmt(
        "[Migrate] store '%s' rebuilt counts: blob-hash %s -> %s, mem-hash %s -> %s",
        GetStoreNm().CStr(), TStrUtil::GetStr(SrcBlobRecs).CStr(), TStrUtil::GetStr((uint64)NewRecIdBlobPtH.Len()).CStr(),
        TStrUtil::GetStr(SrcMemRecs).CStr(), TStrUtil::GetStr((uint64)NewRecIdBlobPtHMem.Len()).CStr()));
    if (DroppedRecs > 0) {
        TEnv::Logger->OnStatus(TStr::Fmt(
            "[Migrate] WARNING: store '%s' dropped %s records that have data in only one of the two sections "
            "(%s only in the disk section, sample ids: %s; %s only in the in-memory section, sample ids: %s). "
            "Such fragments cannot be re-serialized into the new layout because the values of the missing "
            "section's fields are gone. Run DefragStores instead to keep them as-is, or inspect the sample "
            "ids to confirm they are garbage.",
            GetStoreNm().CStr(), TStrUtil::GetStr(DroppedRecs).CStr(),
            TStrUtil::GetStr(BlobOnlyRecs).CStr(), SampleStr(BlobOnlySampleV).CStr(),
            TStrUtil::GetStr(MemOnlyRecs).CStr(), SampleStr(MemOnlySampleV).CStr()));
    }

    // write the destination store state file - the format and content must match
    // what the destructor writes, with the NEW serializators and the new
    // record-id-to-blob-pointer maps (and, when fields were dropped, the
    // renumbered primary field id)
    TFOut FOut(DestStoreFNm + "PgBlobStore");
    RecNmFieldP.Save(FOut);
    TInt NewPrimaryFieldId = (PrimaryFieldId != -1 && DropFieldsP) ?
        OldToNewFieldIdV[PrimaryFieldId] : PrimaryFieldId;
    NewPrimaryFieldId.Save(FOut);
    if (PrimaryFieldType == oftInt) {
        PrimaryIntIdH.Save(FOut);
    } else if (PrimaryFieldType == oftUInt64) {
        PrimaryUInt64IdH.Save(FOut);
    } else if (PrimaryFieldType == oftFlt) {
        PrimaryFltIdH.Save(FOut);
    } else if (PrimaryFieldType == oftTm) {
        PrimaryTmMSecsIdH.Save(FOut);
    } else {
        PrimaryStrIdH.Save(FOut);
    }
    WndDesc.Save(FOut);
    NewSerCache.Save(FOut);
    NewSerMem.Save(FOut);
    NewRecIdBlobPtH.Save(FOut);
    NewRecIdBlobPtHMem.Save(FOut);
    RecIdCounter.Save(FOut);

    // when fields were dropped the remaining field ids were renumbered, so the
    // rebuilt store also needs a new ".BaseStore" file: the renumbered field
    // table and the field joins repointed at the new ids (join ids, join names
    // and index-join key ids are untouched). Written in the exact format of
    // TStore::SaveStore. The caller must swap this file in together with the
    // blob files and remap the index vocabulary (TIndexVoc::RemapStoreFieldIds).
    if (DropFieldsP) {
        TJoinDescV NewJoinDescV; TStrH NewJoinNmToIdH; TStrH NewFieldNmToIdH;
        for (int JoinId = 0; JoinId < GetJoins(); JoinId++) {
            TJoinDesc JoinDesc = GetJoinDesc(JoinId);
            if (JoinDesc.IsFieldJoin()) {
                // the guards above ensured neither field is dropped
                JoinDesc.PutFieldJoinIds(OldToNewFieldIdV[JoinDesc.GetJoinRecFieldId()],
                    JoinDesc.GetJoinFqFieldId() == -1 ? -1 : OldToNewFieldIdV[JoinDesc.GetJoinFqFieldId()].Val);
            }
            NewJoinDescV.Add(JoinDesc);
            NewJoinNmToIdH.AddDat(JoinDesc.GetJoinNm()) = JoinId;
        }
        for (int NewFieldId = 0; NewFieldId < NewFieldDescV.Len(); NewFieldId++) {
            NewFieldNmToIdH.AddDat(NewFieldDescV[NewFieldId].GetFieldNm()) = NewFieldId;
        }
        TFOut BaseFOut(DestStoreFNm + ".BaseStore");
        TUInt(GetStoreId()).Save(BaseFOut);
        GetStoreNm().Save(BaseFOut);
        NewJoinDescV.Save(BaseFOut);
        NewJoinNmToIdH.Save(BaseFOut);
        NewFieldDescV.Save(BaseFOut);
        NewFieldNmToIdH.Save(BaseFOut);
        OldToNewFieldIdVOut = OldToNewFieldIdV;
    } else {
        OldToNewFieldIdVOut.Clr();
    }

    TEnv::Logger->OnStatus(TStr::Fmt("Migrating store '%s' done: %s records%s",
        GetStoreNm().CStr(), TStrUtil::GetStr(WrittenRecs).CStr(),
        DropFieldsP ? TStr::Fmt(" (%d field(s) dropped)", DroppedFieldNmV.Len()).CStr() : ""));
    // releasing the new page blobs flushes them to disk
    return WrittenRecs;
}

/// Build the record maps in the TDstMap representation and write the state file
template <class TRecPtMap>
template <class TDstMap>
uint64 TStorePbBlobT<TRecPtMap>::ConvertMapsTo(const TStr& DestStoreFNm) const {
    // convert both maps; fill in ascending record id order (irrelevant for the
    // hash, keeps the dense vector's gap filling to a single pass)
    TDstMap NewBlobMap, NewMemMap;
    TUInt64V RecIdV;
    RecIdBlobPtH.GetKeyV(RecIdV); RecIdV.Sort();
    for (int RecN = 0; RecN < RecIdV.Len(); RecN++) {
        NewBlobMap.AddDat(RecIdV[RecN], RecIdBlobPtH.GetDat(RecIdV[RecN]));
    }
    RecIdBlobPtHMem.GetKeyV(RecIdV); RecIdV.Sort();
    for (int RecN = 0; RecN < RecIdV.Len(); RecN++) {
        NewMemMap.AddDat(RecIdV[RecN], RecIdBlobPtHMem.GetDat(RecIdV[RecN]));
    }
    EAssert(NewBlobMap.Len() == RecIdBlobPtH.Len() && NewMemMap.Len() == RecIdBlobPtHMem.Len());

    // write the destination store state file - the format and content must match
    // what the destructor writes, with the maps in the destination representation.
    // The record blobs are untouched: both representations address the same
    // PgBlob/PgBlobMem files
    TFOut FOut(DestStoreFNm + "PgBlobStore");
    RecNmFieldP.Save(FOut);
    PrimaryFieldId.Save(FOut);
    if (PrimaryFieldType == oftInt) {
        PrimaryIntIdH.Save(FOut);
    } else if (PrimaryFieldType == oftUInt64) {
        PrimaryUInt64IdH.Save(FOut);
    } else if (PrimaryFieldType == oftFlt) {
        PrimaryFltIdH.Save(FOut);
    } else if (PrimaryFieldType == oftTm) {
        PrimaryTmMSecsIdH.Save(FOut);
    } else {
        PrimaryStrIdH.Save(FOut);
    }
    WndDesc.Save(FOut);
    SerializatorCache->Save(FOut);
    SerializatorMem->Save(FOut);
    NewBlobMap.Save(FOut);
    NewMemMap.Save(FOut);
    RecIdCounter.Save(FOut);

    TEnv::Logger->OnStatus(TStr::Fmt(
        "[ConvertStoreType] store '%s': %s -> %s state file written (blob-map %s records, mem-map %s records)",
        GetStoreNm().CStr(), TRecPtMap::GetStoreTypeNm(), TDstMap::GetStoreTypeNm(),
        TStrUtil::GetStr((int)NewBlobMap.Len()).CStr(), TStrUtil::GetStr((int)NewMemMap.Len()).CStr()));
    return GetRecs();
}

/// Write a state file with the record maps converted to the target representation
template <class TRecPtMap>
uint64 TStorePbBlobT<TRecPtMap>::ConvertStoreTypeTo(const TStr& DestStoreFNm, const TStr& TargetStoreTypeNm) {
    QmAssertR(TargetStoreTypeNm != GetStoreType(), "[ConvertStoreType] store " + GetStoreNm() +
        " is already of type " + TargetStoreTypeNm);
    if (TargetStoreTypeNm == TPbBlobRecMapHash::GetStoreTypeNm()) {
        return ConvertMapsTo<TPbBlobRecMapHash>(DestStoreFNm);
    } else if (TargetStoreTypeNm == TPbBlobRecMapDense::GetStoreTypeNm()) {
        return ConvertMapsTo<TPbBlobRecMapDense>(DestStoreFNm);
    }
    throw TQmExcept::New("[ConvertStoreType] unknown target store type " + TargetStoreTypeNm);
}

/// Delete TOAST-ed value from storage
template <class TRecPtMap>
void TStorePbBlobT<TRecPtMap>::DelToastVal(const TPgBlobPt& Pt) {
    TVec<TPgBlobPt> Pts;
    TThinMIn MIn = DataBlob->Get(Pt);
    Pts.Load(MIn);
    for (int i = 0; i < Pts.Len(); i++) {
        DataBlob->Del(Pts[i]);
    }
    DataBlob->Del(Pt);
}

////////////////////////////////////////////////////////////////////////////////////

/// Create new store with given ID and name
TStoreNotImpl::TStoreNotImpl(const TWPt<TBase>& _Base, uint _StoreId, const TStr& _StoreNm) : TStore(_Base, _StoreId, _StoreNm) {}
/// Load store from input stream
TStoreNotImpl::TStoreNotImpl(const TWPt<TBase>& _Base, TSIn& SIn) : TStore(_Base, SIn) {}
/// Load store from file
TStoreNotImpl::TStoreNotImpl(const TWPt<TBase>& _Base, const TStr& FNm) : TStore(_Base, FNm) {}

int TStoreNotImpl::GetFieldInt(const uint64& RecId, const int& FieldId) const {
    throw FieldError(FieldId, "Int");
}

int16 TStoreNotImpl::GetFieldInt16(const uint64& RecId, const int& FieldId) const {
    throw FieldError(FieldId, "Int16");
}

int64 TStoreNotImpl::GetFieldInt64(const uint64& RecId, const int& FieldId) const {
    throw FieldError(FieldId, "Int64");
}

uchar TStoreNotImpl::GetFieldByte(const uint64& RecId, const int& FieldId) const {
    throw FieldError(FieldId, "Byte");
}

void TStoreNotImpl::GetFieldIntV(const uint64& RecId, const int& FieldId, TIntV& IntV) const {
    throw FieldError(FieldId, "IntV");
}

uint TStoreNotImpl::GetFieldUInt(const uint64& RecId, const int& FieldId) const {
    throw FieldError(FieldId, "UInt");
}
uint16 TStoreNotImpl::GetFieldUInt16(const uint64& RecId, const int& FieldId) const {
    throw FieldError(FieldId, "UInt16");
}
uint64 TStoreNotImpl::GetFieldUInt64(const uint64& RecId, const int& FieldId) const {
    throw FieldError(FieldId, "UInt64");
}

TStr TStoreNotImpl::GetFieldStr(const uint64& RecId, const int& FieldId) const {
    throw FieldError(FieldId, "Str");
}

void TStoreNotImpl::GetFieldStrV(const uint64& RecId, const int& FieldId, TStrV& StrV) const {
    throw FieldError(FieldId, "StrV");
}

bool TStoreNotImpl::GetFieldBool(const uint64& RecId, const int& FieldId) const {
    throw FieldError(FieldId, "Bool");
}

double TStoreNotImpl::GetFieldFlt(const uint64& RecId, const int& FieldId) const {
    throw FieldError(FieldId, "Flt");
}

float TStoreNotImpl::GetFieldSFlt(const uint64& RecId, const int& FieldId) const {
    throw FieldError(FieldId, "Flt");
}

TFltPr TStoreNotImpl::GetFieldFltPr(const uint64& RecId, const int& FieldId) const {
    throw FieldError(FieldId, "FltPr");
}

void TStoreNotImpl::GetFieldFltV(const uint64& RecId, const int& FieldId, TFltV& FltV) const {
    throw FieldError(FieldId, "FltV");
}

void TStoreNotImpl::GetFieldTm(const uint64& RecId, const int& FieldId, TTm& Tm) const {
    throw FieldError(FieldId, "Tm");
}

uint64 TStoreNotImpl::GetFieldTmMSecs(const uint64& RecId, const int& FieldId) const {
    TTm Tm; GetFieldTm(RecId, FieldId, Tm);
    return Tm.IsDef() ? TTm::GetMSecsFromTm(Tm) : TUInt64::Mx;
}

void TStoreNotImpl::GetFieldNumSpV(const uint64& RecId, const int& FieldId, TIntFltKdV& SpV) const {
    throw FieldError(FieldId, "NumSpV");
}

void TStoreNotImpl::GetFieldBowSpV(const uint64& RecId, const int& FieldId, PBowSpV& SpV) const {
    throw FieldError(FieldId, "BowSpV");
}

void TStoreNotImpl::GetFieldTMem(const uint64& RecId, const int& FieldId, TMem& Mem) const {
    throw FieldError(FieldId, "TMem");
}

PJsonVal TStoreNotImpl::GetFieldJsonVal(const uint64& RecId, const int& FieldId) const {
    throw FieldError(FieldId, "Json");
}
void TStoreNotImpl::SetFieldNull(const uint64& RecId, const int& FieldId) {
    throw FieldError(FieldId, "SetNull");
}

void TStoreNotImpl::SetFieldByte(const uint64& RecId, const int& FieldId, const uchar& Byte) {
    throw FieldError(FieldId, "Byte");
}

void TStoreNotImpl::SetFieldInt(const uint64& RecId, const int& FieldId, const int& Int) {
    throw FieldError(FieldId, "Int");
}

void TStoreNotImpl::SetFieldInt16(const uint64& RecId, const int& FieldId, const int16& Int16) {
    throw FieldError(FieldId, "Int16");
}

void TStoreNotImpl::SetFieldInt64(const uint64& RecId, const int& FieldId, const int64& Int64) {
    throw FieldError(FieldId, "Int64");
}

void TStoreNotImpl::SetFieldIntV(const uint64& RecId, const int& FieldId, const TIntV& IntV) {
    throw FieldError(FieldId, "IntV");
}

void TStoreNotImpl::SetFieldUInt(const uint64& RecId, const int& FieldId, const uint& UInt16) {
    throw FieldError(FieldId, "UInt");
}

void TStoreNotImpl::SetFieldUInt16(const uint64& RecId, const int& FieldId, const uint16& UInt16) {
    throw FieldError(FieldId, "UInt16");
}

void TStoreNotImpl::SetFieldUInt64(const uint64& RecId, const int& FieldId, const uint64& UInt64) {
    throw FieldError(FieldId, "UInt64");
}

void TStoreNotImpl::SetFieldStr(const uint64& RecId, const int& FieldId, const TStr& Str) {
    throw FieldError(FieldId, "Str");
}

void TStoreNotImpl::SetFieldStrV(const uint64& RecId, const int& FieldId, const TStrV& StrV) {
    throw FieldError(FieldId, "StrV");
}

void TStoreNotImpl::SetFieldBool(const uint64& RecId, const int& FieldId, const bool& Bool) {
    throw FieldError(FieldId, "Bool");
}

void TStoreNotImpl::SetFieldFlt(const uint64& RecId, const int& FieldId, const double& Flt) {
    throw FieldError(FieldId, "Flt");
}

void TStoreNotImpl::SetFieldSFlt(const uint64& RecId, const int& FieldId, const float& Flt) {
    throw FieldError(FieldId, "SFlt");
}

void TStoreNotImpl::SetFieldFltPr(const uint64& RecId, const int& FieldId, const TFltPr& FltPr) {
    throw FieldError(FieldId, "FltPr");
}

void TStoreNotImpl::SetFieldFltV(const uint64& RecId, const int& FieldId, const TFltV& FltV) {
    throw FieldError(FieldId, "FltV");
}

void TStoreNotImpl::SetFieldTm(const uint64& RecId, const int& FieldId, const TTm& Tm) {
    throw FieldError(FieldId, "Tm");
}

void TStoreNotImpl::SetFieldTmMSecs(const uint64& RecId, const int& FieldId, const uint64& TmMSecs) {
    throw FieldError(FieldId, "TmMSecs");
}

void TStoreNotImpl::SetFieldNumSpV(const uint64& RecId, const int& FieldId, const TIntFltKdV& SpV) {
    throw FieldError(FieldId, "NumSpV");
}

void TStoreNotImpl::SetFieldBowSpV(const uint64& RecId, const int& FieldId, const PBowSpV& SpV) {
    throw FieldError(FieldId, "BowSpV");
}

void TStoreNotImpl::SetFieldTMem(const uint64& RecId, const int& FieldId, const TMem& Mem) {
    throw FieldError(FieldId, "TMem");
}

void TStoreNotImpl::SetFieldJsonVal(const uint64& RecId, const int& FieldId, const PJsonVal& Json) {
    throw FieldError(FieldId, "Json");
}

void TStoreNotImpl::RunVerification() { }
void TStoreNotImpl::RunVerificationForRecord(const uint64& RecId) { }

///////////////////////////////
/// Create new stores in an existing base from a schema definition
TVec<TWPt<TStore> > CreateStoresFromSchema(const TWPt<TBase>& Base, const PJsonVal& SchemaVal,
    const uint64& DefStoreCacheSize, const TStrUInt64H& StoreNmCacheSizeH, bool UsePaged) {

    // parse and validate the schema
    InfoLog("Parsing schema");
    TStoreSchemaV SchemaV; TStoreSchema::ParseSchema(Base, SchemaVal, SchemaV);
    TStoreSchema::ValidateSchema(Base, SchemaV);

    // create stores
    TVec<TWPt<TStore> > NewStoreV;
    for (int SchemaN = 0; SchemaN < SchemaV.Len(); SchemaN++) {
        TStoreSchema& StoreSchema = SchemaV[SchemaN];
        TStr StoreNm = StoreSchema.StoreName;
        InfoLog("Creating " + StoreNm);
        // figure out store id
        uint StoreId = 0;
        if (StoreSchema.HasStoreIdP) {
            StoreId = StoreSchema.StoreId;
            // check if we already have store with same ID
            QmAssertR(!Base->IsStoreId(StoreId), "Store id for " + StoreNm + " already in use.");
        } else {
            // find lowest unused StoreId
            while (Base->IsStoreId(StoreId)) {
                StoreId++;
                QmAssertR(StoreId < TEnv::GetMxStores(), "Out of store Ids -- to many stores!");
            }
        }
        // get cache size for the store
        const uint64 StoreCacheSize = StoreNmCacheSizeH.IsKey(StoreNm) ?
            StoreNmCacheSizeH.GetDat(StoreNm).Val : DefStoreCacheSize;
        // create new store from the schema
        PStore Store;
        if (UsePaged && StoreSchema.StoreType == "paged") {
            if (StoreSchema.DenseRecIdMapP) {
                Store = new TStorePbBlobDense(Base, StoreId, StoreNm,
                    StoreSchema, Base->GetFPath() + StoreNm, StoreCacheSize, StoreSchema.BlockSizeMem);
            } else {
                Store = new TStorePbBlob(Base, StoreId, StoreNm,
                    StoreSchema, Base->GetFPath() + StoreNm, StoreCacheSize, StoreSchema.BlockSizeMem);
            }
        } else {
            Store = new TStoreImpl(Base, StoreId, StoreNm,
                StoreSchema, Base->GetFPath() + StoreNm, StoreCacheSize,
                StoreSchema.BlockSizeMem);
        }
        // add store to base
        Base->AddStore(Store);
        // remember we create the store
        NewStoreV.Add(Store);
    }

    // Create joins
    InfoLog("Creating joins");
    for (int SchemaN = 0; SchemaN < SchemaV.Len(); SchemaN++) {
        // get store
        TStoreSchema StoreSchema = SchemaV[SchemaN];
        TWPt<TStore> Store = Base->GetStoreByStoreNm(StoreSchema.StoreName);
        // go over all outgoing joins
        for (int JoinDescExN = 0; JoinDescExN < StoreSchema.JoinDescExV.Len(); JoinDescExN++) {
            TJoinDescEx& JoinDescEx = StoreSchema.JoinDescExV[JoinDescExN];
            // get join store
            TWPt<TStore> JoinStore = Base->GetStoreByStoreNm(JoinDescEx.JoinStoreName);
            // check join type
            if (JoinDescEx.JoinType == osjtField) {
                // field join
                int JoinRecFieldId = Store->GetFieldId(JoinDescEx.JoinName + "Id");
                int JoinFqFieldId = -1; // frequency field is optional - in that case store -1
                if (Store->IsFieldNm(JoinDescEx.JoinName + "Fq")) {
                    JoinFqFieldId = Store->GetFieldId(JoinDescEx.JoinName + "Fq");
                }
                Store->AddJoinDesc(TJoinDesc(Base, JoinDescEx.JoinName,
                    JoinStore->GetStoreId(), JoinRecFieldId, JoinFqFieldId));
            } else if (JoinDescEx.JoinType == osjtIndex) {
                // index join
                Store->AddJoinDesc(TJoinDesc(Base, JoinDescEx.JoinName,
                    JoinStore->GetStoreId(), Store->GetStoreId(),
                    Base->GetIndexVoc(), JoinDescEx.GixType));
            } else {
                ErrorLog("Unknown join type for join " + JoinDescEx.JoinName);
            }
        }
    }

    // Update inverse joins IDs
    InfoLog("Updating inverse join maps");
    for (int SchemaN = 0; SchemaN < SchemaV.Len(); SchemaN++) {
        // get store
        TStoreSchema StoreSchema = SchemaV[SchemaN];
        TWPt<TStore> Store = Base->GetStoreByStoreNm(StoreSchema.StoreName);
        // go over outgoing joins
        for (int JoinDescExN = 0; JoinDescExN < StoreSchema.JoinDescExV.Len(); JoinDescExN++) {
            // check if we have inverse join
            TJoinDescEx& JoinDescEx = StoreSchema.JoinDescExV[JoinDescExN];
            if (!JoinDescEx.InverseJoinName.Empty()) {
                // we do, get inverse join id
                const int JoinId = Store->GetJoinId(JoinDescEx.JoinName);
                const TJoinDesc& JoinDesc = Store->GetJoinDesc(JoinId);
                TWPt<TStore> JoinStore = Base->GetStoreByStoreId(JoinDesc.GetJoinStoreId());
                QmAssertR(JoinStore->IsJoinNm(JoinDescEx.InverseJoinName),
                    "Invalid inverse join " + JoinDescEx.InverseJoinName);
                const int InverseJoinId = JoinStore->GetJoinId(JoinDescEx.InverseJoinName);
                // mark the map
                Store->PutInverseJoinId(JoinId, InverseJoinId);
            }
        }
    }

    // apply per-key split length overrides from the schema
    ApplyIndexKeySplitLen(Base, SchemaVal);
    // apply the requested key-dictionary representations to the (still empty)
    // gixes of the just-created index. The data window is conservatively
    // reported as open (conditional values resolve to hash) - callers that
    // know their window is closed re-apply while the gixes are still empty
    ApplyIndexKeyDictTypes(Base, SchemaVal, false, true);

    // done
    return NewStoreV;
}

void ApplyIndexKeySplitLen(const TWPt<TBase>& Base, const PJsonVal& SchemaVal) {
    // the schema can be a single store definition or an array of them
    PJsonVal SchemaArrVal = SchemaVal;
    if (!SchemaVal->IsArr()) {
        TJsonValV StoreValV; StoreValV.Add(SchemaVal);
        SchemaArrVal = TJsonVal::NewArr(StoreValV);
    }
    for (int SchemaN = 0; SchemaN < SchemaArrVal->GetArrVals(); SchemaN++) {
        PJsonVal StoreVal = SchemaArrVal->GetArrVal(SchemaN);
        QmAssertR(StoreVal->IsObjKey("name"), "Missing store name in schema definition");
        const TStr StoreNm = StoreVal->GetObjStr("name");
        // index keys: key name defaults to the indexed field name
        if (StoreVal->IsObjKey("keys")) {
            PJsonVal KeyDefsVal = StoreVal->GetObjKey("keys");
            for (int KeyN = 0; KeyN < KeyDefsVal->GetArrVals(); KeyN++) {
                PJsonVal KeyVal = KeyDefsVal->GetArrVal(KeyN);
                const int SplitLen = KeyVal->GetObjInt("splitLen", -1);
                if (SplitLen > 0) {
                    const TStr KeyNm = KeyVal->GetObjStr("name", KeyVal->GetObjStr("field", ""));
                    Base->PutIndexKeySplitLen(StoreNm, KeyNm, SplitLen);
                    InfoLog("Using split length " + TInt::GetStr(SplitLen) + " for index key " + StoreNm + "." + KeyNm);
                }
            }
        }
        // index joins: the internal index key is named "Join" + join name
        if (StoreVal->IsObjKey("joins")) {
            PJsonVal JoinDefsVal = StoreVal->GetObjKey("joins");
            for (int JoinN = 0; JoinN < JoinDefsVal->GetArrVals(); JoinN++) {
                PJsonVal JoinVal = JoinDefsVal->GetArrVal(JoinN);
                const int SplitLen = JoinVal->GetObjInt("splitLen", -1);
                if (SplitLen > 0) {
                    QmAssertR(JoinVal->GetObjStr("type", "index") == "index",
                        "'splitLen' is only supported on index joins (store " + StoreNm + ")");
                    const TStr JoinNm = JoinVal->GetObjStr("name");
                    Base->PutIndexKeySplitLen(StoreNm, "Join" + JoinNm, SplitLen);
                    InfoLog("Using split length " + TInt::GetStr(SplitLen) + " for join key " + StoreNm + "." + JoinNm);
                }
            }
        }
    }
}

void ParseIndexKeyDictTypes(const PJsonVal& SchemaVal, const bool& DataWindowClosedP,
        THash<TStr, TInt>& GixNmTypeH) {
    GixNmTypeH.Clr();
    // the schema can be a single store definition or an array of them
    PJsonVal SchemaArrVal = SchemaVal;
    if (!SchemaVal->IsArr()) {
        TJsonValV StoreValV; StoreValV.Add(SchemaVal);
        SchemaArrVal = TJsonVal::NewArr(StoreValV);
    }
    bool FoundP = false;
    for (int SchemaN = 0; SchemaN < SchemaArrVal->GetArrVals(); SchemaN++) {
        PJsonVal StoreVal = SchemaArrVal->GetArrVal(SchemaN);
        // the (base-wide) index options ride as an extra attribute of one store
        // definition - older binaries ignore unknown store attributes, so a
        // schema carrying them stays readable by exes that predate this option
        if (!StoreVal->IsObj() || !StoreVal->IsObjKey("indexOptions")) { continue; }
        QmAssertR(!FoundP, "The schema contains more than one indexOptions entry");
        FoundP = true;
        PJsonVal IndexOptionsVal = StoreVal->GetObjKey("indexOptions");
        if (!IndexOptionsVal->IsObjKey("keyDict")) { continue; }
        PJsonVal KeyDictVal = IndexOptionsVal->GetObjKey("keyDict");
        QmAssertR(KeyDictVal->IsObj(), "indexOptions.keyDict must be an object mapping gix names to representations");
        for (int KeyN = 0; KeyN < KeyDictVal->GetObjKeys(); KeyN++) {
            TStr GixNm; PJsonVal TypeVal;
            KeyDictVal->GetObjKeyVal(KeyN, GixNm, TypeVal);
            GixNm = GixNm.GetLc();
            QmAssertR(GixNm == "full" || GixNm == "small" || GixNm == "tiny" || GixNm == "pos",
                "indexOptions.keyDict: unknown gix name '" + GixNm + "' (expected full, small, tiny or pos)");
            QmAssertR(TypeVal->IsStr(), "indexOptions.keyDict." + GixNm + " must be a string");
            const TStr TypeStr = TypeVal->GetStr();
            QmAssertR(TypeStr == "hash" || TypeStr == "sorted" || TypeStr == "sortedWhenClosed",
                "indexOptions.keyDict." + GixNm + ": unknown representation '" + TypeStr + "' (expected hash, sorted or sortedWhenClosed)");
            // the conditional value keeps write-heavy instances (data window
            // still open) on the hash and switches to sorted once it closes
            const bool SortedP = (TypeStr == "sorted") ||
                (TypeStr == "sortedWhenClosed" && DataWindowClosedP);
            GixNmTypeH.AddDat(GixNm, SortedP ? (int) gkdtSorted : (int) gkdtHash);
        }
    }
}

void ApplyIndexKeyDictTypes(const TWPt<TBase>& Base, const PJsonVal& SchemaVal,
        const bool& DataWindowClosedP, const bool& CreatedP) {
    THash<TStr, TInt> GixNmTypeH; ParseIndexKeyDictTypes(SchemaVal, DataWindowClosedP, GixNmTypeH);
    if (GixNmTypeH.Empty()) { return; }
    int GixKeyId = GixNmTypeH.FFirstKeyId();
    while (GixNmTypeH.FNextKeyId(GixKeyId)) {
        const TStr& GixNm = GixNmTypeH.GetKey(GixKeyId);
        const TGixKeyDictType Type = (TGixKeyDictType) (int) GixNmTypeH[GixKeyId];
        const TGixKeyDictType CurrentType = Base->GetIndex()->GetGixKeyDictType(GixNm);
        if (CurrentType == Type) { continue; }
        const TStr TypeStr = (Type == gkdtHash) ? "hash" : "sorted";
        if (CreatedP) {
            // a just-created gix is empty, so the switch is free
            Base->GetIndex()->PutGixKeyDictType(GixNm, Type);
            InfoLog("Using the " + TypeStr + " key dictionary for the " + GixNm + " index");
        } else {
            // an existing index keeps what its files say - converting multi-GB
            // dictionaries must be an explicit offline action, not a side
            // effect of opening the base; just make the drift visible
            InfoLog("NOTE: the schema requests the " + TypeStr + " key dictionary for the " +
                GixNm + " index, but the existing index uses " +
                ((CurrentType == gkdtHash) ? "hash" : "sorted") +
                " - run the ConvertIndexKeyDict action to convert it");
        }
    }
}

///////////////////////////////
/// Create new base given a schema definition
TWPt<TBase> NewBase(const TStr& FPath, const PJsonVal& SchemaVal, const uint64& IndexCacheSize,
    const uint64& DefStoreCacheSize, const bool& StrictNameP,
    const TStrUInt64H& StoreNmCacheSizeH, const TStrUInt64H& IndexTypeCacheSizeH,
    const bool& InitP, const int& SplitLen, bool UsePaged) {

    // create empty base
    InfoLog("Creating new base from schema");
    TWPt<TBase> Base = TBase::New(FPath, IndexCacheSize, IndexTypeCacheSizeH, SplitLen, StrictNameP);
    // parse and apply the schema
    CreateStoresFromSchema(Base, SchemaVal, DefStoreCacheSize, StoreNmCacheSizeH, UsePaged);
    // finish base initialization if so required (default is true)
    if (InitP) { Base->Init(); }
    // done
    return Base;
}


///////////////////////////////
/// Load base created from a schema definition
TWPt<TBase> LoadBase(const TStr& FPath, const TFAccess& FAccess, const uint64& IndexCacheSize,
    const uint64& DefStoreCacheSize,
    const TStrUInt64H& StoreNmCacheSizeH, const TStrUInt64H& IndexTypeCacheSizeH,
    const bool& InitP, const int& SplitLen) {

    InfoLog("Loading base created from schema definition");
    TWPt<TBase> Base = TBase::Load(FPath, FAccess, IndexCacheSize, IndexTypeCacheSizeH, SplitLen);
    // load stores
    InfoLog("Loading stores");
    // read store names from file
    PJsonVal RootVal = TJsonVal::GetValFromStr(TStr::LoadTxt(FPath + "StoreList.json"));
    QmAssert(!RootVal->IsNull() && RootVal->IsObjKey("stores"));
    PJsonVal StoresVal = RootVal->GetObjKey("stores");
    for (int i = 0; i < StoresVal->GetArrVals(); i++) {
        PJsonVal StoreVal = StoresVal->GetArrVal(i);
        TStr StoreNm = StoreVal->GetObjStr("name");
        TStr StoreType = StoreVal->GetObjStr("type");
        InfoLog("  " + StoreNm);
        // get cache size for the store
        const uint64 StoreCacheSize = StoreNmCacheSizeH.IsKey(StoreNm) ?
            StoreNmCacheSizeH.GetDat(StoreNm).Val : DefStoreCacheSize;
        PStore Store;
        if (StoreType == "TStorePbBlob") {
            Store = new TStorePbBlob(Base, FPath + StoreNm, FAccess, StoreCacheSize);
        } else if (StoreType == "TStorePbBlobDense") {
            Store = new TStorePbBlobDense(Base, FPath + StoreNm, FAccess, StoreCacheSize);
        } else {
            Store = new TStoreImpl(Base, FPath + StoreNm, StoreCacheSize);
        }
        Base->AddStore(Store);
    }
    InfoLog("Stores loaded");
    // finish base initialization if so required (default is true)
    if (InitP) { Base->Init(); }
    // done
    return Base;
}

///////////////////////////////
/// Save base created from a schema definition
void SaveBase(const TWPt<TBase>& Base) {
    if (Base->IsRdOnly()) {
        InfoLog("No saving of generic base necessary!");
    } else {
        // Saving list of stores so we know what to load next time
        // Stores are saved automatically in destructor
        InfoLog("Saving list of stores ... ");
        PJsonVal StoresVal = TJsonVal::NewArr();
        for (int StoreN = 0; StoreN < Base->GetStores(); StoreN++) {
            PStore Store = Base->GetStoreByStoreN(StoreN);
            PJsonVal StoreVal = TJsonVal::NewObj();
            StoreVal->AddToObj("name", Store->GetStoreNm());
            StoreVal->AddToObj("type", Store->GetStoreType());
            StoresVal->AddToArr(StoreVal);
        }
        PJsonVal RootVal = TJsonVal::NewObj("stores", StoresVal);
        RootVal->SaveStr().SaveTxt(Base->GetFPath() + "StoreList.json");
    }
}

// instantiate the two paged-store variants: the hash-backed record maps
// ("TStorePbBlob", the original representation) and the direct-indexed dense
// maps ("TStorePbBlobDense"). The store bodies are identical by construction -
// only the TRecPtMap policy differs
template class TStorePbBlobT<TPbBlobRecMapHash>;
template class TStorePbBlobT<TPbBlobRecMapDense>;

} // TStorage

}
