/**
 * Copyright (c) 2015, Jozef Stefan Institute, Quintelligence d.o.o. and contributors
 * All rights reserved.
 *
 * This source code is licensed under the FreeBSD license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef GIX_H
#define GIX_H

/////////////////////////////////////////////////
// Forward-declarations
template <class TKey, class TItem> class TGix;

/////////////////////////////////////////////////
/// Item Handler.
/// Used when indexing and deindexing items in TGix.
template <class TKey, class TItem>
class TGixItemHandler {
public:
    virtual ~TGixItemHandler() {}

    /// Merge repeated items in the ItemV vector.
    /// TODO: What is IsLocal parameter?
    virtual void Merge(TVec<TItem>& ItemV, const bool& IsLocal) const = 0;

    /// Remove all occurences of Item in MainV
    virtual void Delete(const TItem& Item, TVec<TItem>& MainV) const = 0;
    /// Is Item1 < Item2?
    virtual bool IsLt(const TItem& Item1, const TItem& Item2) const = 0;
    /// Is Item1 <= Item2?
    virtual bool IsLtE(const TItem& Item1, const TItem& Item2) const = 0;

    /// Memory footprint
    virtual uint64 GetMemUsed() const = 0;
};

/////////////////////////////////////////////////
/// Default Item Handler.
/// Uses basic set operations defined on TVec and TItem.
template <class TKey, class TItem>
class TGixDefItemHandler : public TGixItemHandler <TKey, TItem> {
public:
    void Merge(TVec<TItem>& ItemV, const bool& IsLocal) const { ItemV.Merge(); }
    void Delete(const TItem& Item, TVec<TItem>& MainV) const { return MainV.DelAll(Item); }
    bool IsLt(const TItem& Item1, const TItem& Item2) const { return Item1 < Item2; }
    bool IsLtE(const TItem& Item1, const TItem& Item2) const { return Item1 <= Item2; }

    uint64 GetMemUsed() const { return sizeof(TGixDefItemHandler<TKey, TItem>); }
};

/////////////////////////////////////////////////
/// Comparator for delete markers, i.e. (item-to-delete, position in the work buffer) pairs.
/// Orders by item through the item handler - the handler's IsLt is the authoritative order of a
/// gix, and equality under it coincides with TItem::operator==, which is what a delete matches on.
/// Ties (repeats of the same value) are broken by ascending position, so the last marker of a
/// value sorts last within its group.
template <class TKey, class TItem>
class TGixDelMarkerCmp {
private:
    const TGixItemHandler<TKey, TItem>* ItemHandler;
public:
    TGixDelMarkerCmp(const TGixItemHandler<TKey, TItem>* _ItemHandler) : ItemHandler(_ItemHandler) {}
    bool operator () (const TPair<TItem, TInt>& Pr1, const TPair<TItem, TInt>& Pr2) const {
        if (ItemHandler->IsLt(Pr1.Val1, Pr2.Val1)) { return true; }
        if (ItemHandler->IsLt(Pr2.Val1, Pr1.Val1)) { return false; }
        return Pr1.Val2 < Pr2.Val2;
    }
};

/////////////////////////////////////////////////
/// Split-Length provider.
/// Allows specifying per-key work-buffer/child vector lengths. Keys for which
/// the provider returns a value <= 0 use the default split length of the gix.
template <class TKey>
class TGixSplitLenProvider {
public:
    virtual ~TGixSplitLenProvider() {}

    /// Return split length for given key, or -1 to use the gix default
    virtual int GetSplitLen(const TKey& Key) const = 0;
};

/////////////////////////////////////////////////
/// Key filter.
/// Selects a subset of keys for full-scan operations (CopyTo, VerifySample).
template <class TKey>
class TGixKeyFilter {
public:
    virtual ~TGixKeyFilter() {}

    /// Return true when the key should be processed, false to skip it
    virtual bool KeepKeyP(const TKey& Key) const = 0;
};

/////////////////////////////////////////////////
/// Gix key dictionary representation
typedef enum {
    gkdtHash = 0,   ///< THash-backed - the default; best for write-heavy indexes
    gkdtSorted = 1  ///< sorted parallel arrays + overlay hash - ~45% less memory,
                    ///< bulk (single-read) load; best for read-mostly indexes
} TGixKeyDictType;

/////////////////////////////////////////////////
/// Gix key dictionary: key -> blob pointer of the key's itemset.
/// Two interchangeable representations behind one THash-like interface:
///
/// - gkdtHash: a plain THash<TKey, TBlobPt> (~32 B/key incl. buckets for the
///   qminer (keyId, wordId) keys). Saved in the LEGACY format, so hash-type
///   files remain readable by older binaries and all existing files load
///   unchanged.
/// - gkdtSorted: three parallel arrays sorted by key (key, blob segment, blob
///   address; ~18 B/key, no buckets) plus a small overlay hash. Lookups check
///   the overlay first, then binary-search the arrays. Existing keys update
///   their blob pointer in place; keys added in ascending order (how CopyTo
///   and the defrag/reindex rebuilds emit them) extend the arrays directly;
///   out-of-order additions go to the overlay; deletions of array keys leave
///   a tombstone (empty blob address - itemset pointers are never empty).
///   Overlay and tombstones are folded away by the next conversion or rebuild.
///   The arrays are flat-serializable, so the dictionary loads with bulk reads
///   instead of element-wise deserialization.
///
/// The representation is chosen when the gix is created and persisted in the
/// .Gix file itself (self-describing - the sorted format starts with a marker
/// that cannot appear in a legacy hash file), so opening auto-detects it.
/// Use ConvertTo/SaveFileAsType to switch an existing gix, e.g. hash while an
/// index is being written heavily, sorted once it turns read-mostly.
template <class TKey>
class TGixKeyDict {
private:
    /// First int of the sorted-format file. A legacy THash file starts with
    /// PortV.MxVals, which is never negative in a saved hash
    static const int FormatMarker = -1;
    static const int FormatVersion = 1;

    TGixKeyDictType Type;
    /// hash representation (gkdtHash)
    THash<TKey, TBlobPt> KeyBlobPtH;
    /// sorted representation (gkdtSorted): parallel arrays sorted by key
    TVec<TKey> SortedKeyV;
    TVec<TUInt16> SortedSegV;
    TVec<TUInt> SortedAddrV;
    /// number of tombstones (deleted keys) in the sorted arrays
    TInt SortedDels;
    /// out-of-order additions that could not extend the sorted arrays
    THash<TKey, TBlobPt> OverlayH;

    /// Binary search; position in the sorted arrays or -1 (tombstones are found)
    int GetSortedKeyN(const TKey& Key) const {
        int LoN = 0; int HiN = SortedKeyV.Len() - 1;
        while (LoN <= HiN) {
            const int MidN = LoN + (HiN - LoN) / 2;
            if (SortedKeyV[MidN] < Key) { LoN = MidN + 1; }
            else if (Key < SortedKeyV[MidN]) { HiN = MidN - 1; }
            else { return MidN; }
        }
        return -1;
    }
    /// Is the sorted-array entry a tombstone? Live itemset pointers always
    /// have a valid address (they come from TBlobBs::PutBlob)
    bool IsSortedTomb(const int& KeyN) const { return SortedAddrV[KeyN] == TUInt::Mx; }
    TBlobPt GetSortedPt(const int& KeyN) const {
        return TBlobPt((uint16)SortedSegV[KeyN].Val, SortedAddrV[KeyN].Val); }
    void PutSortedPt(const int& KeyN, const TBlobPt& Pt) {
        SortedSegV[KeyN] = Pt.Seg; SortedAddrV[KeyN] = Pt.Addr; }

public:
    TGixKeyDict(const TGixKeyDictType& _Type = gkdtHash): Type(_Type), SortedDels(0) {}

    TGixKeyDictType GetType() const { return Type; }
    int Len() const {
        return Type == gkdtHash ? KeyBlobPtH.Len() :
            SortedKeyV.Len() - SortedDels + OverlayH.Len(); }
    void Clr() {
        KeyBlobPtH.Clr(); SortedKeyV.Clr(); SortedSegV.Clr(); SortedAddrV.Clr();
        SortedDels = 0; OverlayH.Clr(); }

    bool IsKey(const TKey& Key) const {
        if (Type == gkdtHash) { return KeyBlobPtH.IsKey(Key); }
        if (OverlayH.IsKey(Key)) { return true; }
        const int KeyN = GetSortedKeyN(Key);
        return (KeyN != -1) && !IsSortedTomb(KeyN);
    }
    /// Get the blob pointer of an EXISTING key
    TBlobPt GetDat(const TKey& Key) const {
        if (Type == gkdtHash) { return KeyBlobPtH.GetDat(Key); }
        if (OverlayH.IsKey(Key)) { return OverlayH.GetDat(Key); }
        const int KeyN = GetSortedKeyN(Key);
        EAssert(KeyN != -1 && !IsSortedTomb(KeyN));
        return GetSortedPt(KeyN);
    }
    /// Add a key or update an existing key's blob pointer
    void AddDat(const TKey& Key, const TBlobPt& Pt) {
        if (Type == gkdtHash) { KeyBlobPtH.AddDat(Key, Pt); return; }
        if (OverlayH.IsKey(Key)) { OverlayH.AddDat(Key, Pt); return; }
        const int KeyN = GetSortedKeyN(Key);
        if (KeyN != -1) {
            // in-place update; also revives a tombstoned key
            if (IsSortedTomb(KeyN)) { SortedDels--; }
            PutSortedPt(KeyN, Pt);
            return;
        }
        // keys arriving in ascending order extend the arrays directly - this is
        // how the sorted key-by-key rebuilds (CopyTo, defrag, reindex) and the
        // conversion build the compact form without going through the overlay
        if (SortedKeyV.Empty() || SortedKeyV.Last() < Key) {
            SortedKeyV.Add(Key);
            SortedSegV.Add(TUInt16(Pt.Seg));
            SortedAddrV.Add(TUInt(Pt.Addr));
            return;
        }
        OverlayH.AddDat(Key, Pt);
    }
    /// Delete an EXISTING key
    void DelKey(const TKey& Key) {
        if (Type == gkdtHash) { KeyBlobPtH.DelKey(Key); return; }
        if (OverlayH.IsKey(Key)) { OverlayH.DelKey(Key); return; }
        const int KeyN = GetSortedKeyN(Key);
        EAssert(KeyN != -1 && !IsSortedTomb(KeyN));
        SortedAddrV[KeyN] = TUInt::Mx;
        SortedDels++;
    }
    /// All live keys (unsorted for the hash representation)
    void GetKeyV(TVec<TKey>& KeyV) const {
        if (Type == gkdtHash) { KeyBlobPtH.GetKeyV(KeyV); return; }
        KeyV.Clr(); KeyV.Reserve(Len());
        for (int KeyN = 0; KeyN < SortedKeyV.Len(); KeyN++) {
            if (!IsSortedTomb(KeyN)) { KeyV.Add(SortedKeyV[KeyN]); }
        }
        int OverlayKeyId = OverlayH.FFirstKeyId();
        while (OverlayH.FNextKeyId(OverlayKeyId)) { KeyV.Add(OverlayH.GetKey(OverlayKeyId)); }
    }

    // key-id based iteration, mirroring THash. For the sorted representation the
    // ids are the array positions, followed by the overlay hash's key ids offset
    // by the array length. Ids stay valid for the lifetime of the loaded gix
    int FFirstKeyId() const {
        return Type == gkdtHash ? KeyBlobPtH.FFirstKeyId() : -1; }
    bool FNextKeyId(int& KeyId) const {
        if (Type == gkdtHash) { return KeyBlobPtH.FNextKeyId(KeyId); }
        const int SortedLen = SortedKeyV.Len();
        while (KeyId + 1 < SortedLen) {
            KeyId++;
            if (!IsSortedTomb(KeyId)) { return true; }
        }
        int OverlayKeyId = (KeyId < SortedLen) ? OverlayH.FFirstKeyId() : KeyId - SortedLen;
        if (OverlayH.FNextKeyId(OverlayKeyId)) { KeyId = SortedLen + OverlayKeyId; return true; }
        return false;
    }
    bool IsKeyId(const int& KeyId) const {
        if (Type == gkdtHash) { return KeyBlobPtH.IsKeyId(KeyId); }
        if (KeyId >= 0 && KeyId < SortedKeyV.Len()) { return !IsSortedTomb(KeyId); }
        return OverlayH.IsKeyId(KeyId - SortedKeyV.Len());
    }
    const TKey& GetKey(const int& KeyId) const {
        if (Type == gkdtHash) { return KeyBlobPtH.GetKey(KeyId); }
        if (KeyId < SortedKeyV.Len()) { return SortedKeyV[KeyId]; }
        return OverlayH.GetKey(KeyId - SortedKeyV.Len());
    }
    TBlobPt operator[](const int& KeyId) const {
        if (Type == gkdtHash) { return KeyBlobPtH[KeyId]; }
        if (KeyId < SortedKeyV.Len()) { return GetSortedPt(KeyId); }
        return OverlayH[KeyId - SortedKeyV.Len()];
    }

    /// Sort the hash representation by key; the sorted representation's arrays
    /// are ordered already (its overlay stays in insertion order)
    void SortByKey(const bool& Asc) {
        if (Type == gkdtHash) { KeyBlobPtH.SortByKey(Asc); }
    }

    /// Convert in place between the representations. Hash -> sorted folds the
    /// keys into the arrays in sorted order; sorted -> hash also folds in the
    /// overlay and drops the tombstones
    void ConvertTo(const TGixKeyDictType& NewType) {
        if (NewType == Type) { return; }
        if (NewType == gkdtSorted) {
            TVec<TKey> KeyV; KeyBlobPtH.GetKeyV(KeyV); KeyV.Sort();
            SortedKeyV.Gen(KeyV.Len(), 0);
            SortedSegV.Gen(KeyV.Len(), 0);
            SortedAddrV.Gen(KeyV.Len(), 0);
            for (int KeyN = 0; KeyN < KeyV.Len(); KeyN++) {
                const TBlobPt Pt = KeyBlobPtH.GetDat(KeyV[KeyN]);
                SortedKeyV.Add(KeyV[KeyN]);
                SortedSegV.Add(TUInt16(Pt.Seg));
                SortedAddrV.Add(TUInt(Pt.Addr));
            }
            SortedDels = 0; OverlayH.Clr();
            KeyBlobPtH.Clr();
            Type = gkdtSorted;
        } else {
            KeyBlobPtH.Gen(Len());
            int KeyId = FFirstKeyId();
            while (FNextKeyId(KeyId)) { KeyBlobPtH.AddDat(GetKey(KeyId), operator[](KeyId)); }
            SortedKeyV.Clr(); SortedSegV.Clr(); SortedAddrV.Clr();
            SortedDels = 0; OverlayH.Clr();
            Type = gkdtHash;
        }
    }

    /// Load from a .Gix file, auto-detecting the representation: files written
    /// by older binaries (or for a hash-type dictionary) hold a raw THash
    void LoadFile(const TStr& FNm) {
        Clr();
        bool LegacyP;
        {
            // peek the first int: a legacy THash file starts with the (never
            // negative) bucket-vector size, the new format with the marker
            TFIn PeekFIn(FNm);
            const TInt FirstInt(PeekFIn);
            LegacyP = (FirstInt != FormatMarker);
        }
        TFIn FIn(FNm);
        if (LegacyP) {
            Type = gkdtHash;
            KeyBlobPtH.Load(FIn);
        } else {
            const TInt Marker(FIn); EAssert(Marker == FormatMarker);
            const TInt Version(FIn);
            EAssertR(Version <= FormatVersion, "Unsupported .Gix key dictionary version " + TInt::GetStr(Version));
            const TInt TypeInt(FIn);
            Type = (TGixKeyDictType)(int)TypeInt;
            if (Type == gkdtHash) {
                KeyBlobPtH.Load(FIn);
            } else {
                SortedKeyV.Load(FIn);
                SortedSegV.Load(FIn);
                SortedAddrV.Load(FIn);
                SortedDels.Load(FIn);
                OverlayH.Load(FIn);
            }
        }
    }
    /// Save to a .Gix file. The hash representation is written in the legacy
    /// format (readable by older binaries); the sorted one with the marker
    void SaveFile(const TStr& FNm) const {
        TFOut FOut(FNm);
        if (Type == gkdtHash) {
            KeyBlobPtH.Save(FOut);
        } else {
            TInt(FormatMarker).Save(FOut);
            TInt(FormatVersion).Save(FOut);
            TInt((int)Type).Save(FOut);
            SortedKeyV.Save(FOut);
            SortedSegV.Save(FOut);
            SortedAddrV.Save(FOut);
            SortedDels.Save(FOut);
            OverlayH.Save(FOut);
        }
    }
    /// Save to a .Gix file in the given representation without changing this
    /// instance - used to convert an existing index offline (only the key
    /// dictionary file is rewritten, the posting blobs are untouched)
    void SaveFileAsType(const TStr& FNm, const TGixKeyDictType& TargetType) const {
        if (TargetType == Type) { SaveFile(FNm); return; }
        if (TargetType == gkdtSorted) {
            // collect the live keys in sorted order and write the arrays directly
            TVec<TKey> KeyV; GetKeyV(KeyV); KeyV.Sort();
            TVec<TUInt16> SegV(KeyV.Len(), 0);
            TVec<TUInt> AddrV(KeyV.Len(), 0);
            for (int KeyN = 0; KeyN < KeyV.Len(); KeyN++) {
                const TBlobPt Pt = GetDat(KeyV[KeyN]);
                SegV.Add(TUInt16(Pt.Seg));
                AddrV.Add(TUInt(Pt.Addr));
            }
            TFOut FOut(FNm);
            TInt(FormatMarker).Save(FOut);
            TInt(FormatVersion).Save(FOut);
            TInt((int)gkdtSorted).Save(FOut);
            KeyV.Save(FOut);
            SegV.Save(FOut);
            AddrV.Save(FOut);
            TInt(0).Save(FOut);
            THash<TKey, TBlobPt>().Save(FOut);
        } else {
            THash<TKey, TBlobPt> KeyPtH; KeyPtH.Gen(Len());
            int KeyId = FFirstKeyId();
            while (FNextKeyId(KeyId)) { KeyPtH.AddDat(GetKey(KeyId), operator[](KeyId)); }
            TFOut FOut(FNm);
            KeyPtH.Save(FOut);
        }
    }

    uint64 GetMemUsed(const bool& DeepP = false) const {
        return sizeof(TGixKeyDictType) +
            KeyBlobPtH.GetMemUsed(DeepP) +
            SortedKeyV.GetMemUsed() +
            SortedSegV.GetMemUsed() +
            SortedAddrV.GetMemUsed() +
            sizeof(TInt) +
            OverlayH.GetMemUsed(DeepP);
    }
};

/////////////////////////////////////////////////
/// Key-To-String transformer
template <class TKey>
class TGixKeyStr {
protected:
    TCRef CRef;
    typedef TPt<TGixKeyStr<TKey> > PGixKeyStr;

public:
    virtual ~TGixKeyStr() {}
    static PGixKeyStr New() { return new TGixKeyStr <TKey> ; }

    // by default we cannot assume much about key, so just return empty string
    virtual TStr GetKeyNm(const TKey& Key) const { return TStr(); }

    friend class TPt<TGixKeyStr<TKey> >;
};

/////////////////////////////////////////////////
/// Item Set.
/// Holds set of items that correspond to one key. Itemset supports supports splitting of
/// data into child vectors so there is no need to store complete item set in memory.
/// Assumes child vectors are individually and globaly merged.
template <class TKey, class TItem>
class TGixItemSet {
private:
    TCRef CRef;
    typedef TPt<TGixItemSet<TKey, TItem> > PGixItemSet;

private:
    /// Meta-data about child vector
    struct TChildInfo {
    public:
        /// Value of the smallest item in the vector
        TItem MinItem;
        /// Value of the largest item in the vector
        TItem MaxItem;
        /// Number of elements in the vector
        TInt Len;
        /// Pointer to the vector in the blob base
        TBlobPt Pt;
        /// Did we load the vector from blob base to memory?
        TBool LoadedP;
        /// Is the version of vector in the memory different to the one in blob base?
        TBool DirtyP;

    public:
        /// Empty child vector info
        TChildInfo(): Len(0), LoadedP(false), DirtyP(false) {}
        /// Create non-emtpy child vector info
        TChildInfo(const TItem& _MinItem, const TItem& _MaxItem, const TInt& _Len, const TBlobPt& _Pt):
            MinItem(_MinItem), MaxItem(_MaxItem), Len(_Len), Pt(_Pt), LoadedP(false), DirtyP(false) {}

        /// Load child info from stream
        TChildInfo(TSIn& SIn): LoadedP(false), DirtyP(false) { Load(SIn); }
        /// Save child info to stream
        void Load(TSIn& SIn);
        /// serialize to stream
        void Save(TSOut& SOut) const;

        /// Memory footprint
        uint64 GetMemUsed() const;
    };

private:
    /// The key of this itemset
    TKey ItemSetKey;

    /// Working buffer of items of this itemset.
    /// Could be only part of them, others can be stored in child vectors
    TVec<TItem> ItemV;
    /// List of indices with "deleted" items
    TVec<TInt> ItemVDel;
    /// Combined count - from this itemset and children
    TInt TotalCnt;

    /// optional data about child vectors - will be populated only for frequent keys
    mutable TVec<TChildInfo> ChildInfoV;
    /// optional list of child vector contents - will be populated only for frequent keys
    mutable TVec<TVec<TItem> > ChildV;

    /// For keeping the items unique and sorted
    TBool MergedP;
    /// Should this itemset be stored to disk?
    TBool DirtyP;

    /// Pointer to gix used to access storage and merger.
    /// (serialization of self, loading children, notifying about changes...)
    const TGix<TKey, TItem>* Gix;

    /// Size of work-buffer and child vectors for this itemset's key.
    /// Resolved once at construction from the gix (which can have per-key overrides).
    TInt SplitLen;
    /// Minimal tolerated length for child vectors of this itemset
    TInt SplitLenMin;
    /// Maximal tolerated length for child vectors of this itemset
    TInt SplitLenMax;

private:
    /// Resolve split lengths for this itemset's key from the gix
    void ResolveSplitLen();
    /// Load single child vector into memory if not present already
    void LoadChildVector(const int& ChildN) const;
    /// Load all child vectors into memory and get pointers to them
    void LoadChildVectors() const;
    /// Refresh total count
    void RecalcTotalCnt();
    /// Check if there are any dirty child vectors with size outside the tolerance
    int FirstDirtyChild();
    /// Get the index of the first child index from which onward the content needs to be merged
    /// There can be other children with smaller indices that are dirty, but we might not want to merge them
    int GetFirstChildToMerge();
    /// True if any child vector exceeds SplitLenMax. Only adds can oversize a child, so a
    /// deletes-only flush never sees one.
    bool HasOversizedChild() const;
    /// Drop empty children and merge adjacent undersized ones. Used by deletes-only flushes in
    /// Def() instead of the global merge: deletes leave the children sorted and disjoint, so the
    /// itemset is already merged and only fragmentation needs fixing. Copy cost is bounded by the
    /// children actually touched, not by the length of the whole posting list.
    void CoalesceUndersizedChildren();
    /// Work buffer is merged and still full, add new children collections with the data in work buffer
    void PushWorkBufferToChildren();
    /// If work buffer contains data that belongs to child vectors then push that content to them
    void InjectWorkBufferToChildren();
    /// Data has been merged in memory and needs to be pushed to child vectors (overwrite them)
    void PushMergedDataBackToChildren(const int& FirstChildToMerge, const TVec<TItem>& MergedItems);
    /// Index of the child vector whose [MinItem, MaxItem] range can hold Item, or -1 if none can.
    /// Child ranges are disjoint and ascending, so this is a binary search rather than a scan.
    int FindChildToDeleteFrom(const TItem& Item) const;
    /// Position of Item in SortedV (sorted under the item handler's order), or -1 if not present
    int FindInSorted(const TVec<TItem>& SortedV, const TItem& Item) const;
    /// Process any pending "delete" commands
    void ProcessDeletes();

    /// Ask child vectors about their memory usage
    uint64 GetChildMemUsed() const { return TMemUtils::GetExtraMemberSize(ChildV); }

public:
    /// Create empty itemset
    TGixItemSet(const TKey& _ItemSetKey, const TGix<TKey, TItem>* _Gix) :
        ItemSetKey(_ItemSetKey), TotalCnt(0), MergedP(true), DirtyP(true), Gix(_Gix) {
        ResolveSplitLen(); }
    /// Create empty itemset
    static PGixItemSet New(const TKey& ItemSetKey, const TGix<TKey, TItem>* Gix) {
        return new TGixItemSet(ItemSetKey, Gix); }

    /// Load itemset from stream
    TGixItemSet(TSIn& SIn, const TGix<TKey, TItem>* _Gix);
    /// Load itemset from stream
    static PGixItemSet Load(TSIn& SIn, const TGix<TKey, TItem>* Gix) {
        return new TGixItemSet(SIn, Gix); }
    /// Saves this itemset to stream
    void Save(TMOut& SOut);

    // functions called by TCache
    /// Report memory footprint
    uint64 GetMemUsed() const;
    /// Callback when items sets are kicked out from cache and need to be serialized
    void OnDelFromCache(const TBlobPt& BlobPt, void* Gix);

    /// Get key that this itme set represent
    const TKey& GetKey() const { return ItemSetKey; }

    /// Add new item to the item set. When NotifyCacheOnlyDelta is set to true,
    /// only item set memory footprint differences are sent to gix.
    void AddItem(const TItem& NewItem, const bool& NotifyCacheOnlyDelta = true);
    /// Add a set of items at once
    void AddItemV(const TVec<TItem>& NewItemV);

    /// Check if this itemset is empty
    bool Empty() const { return GetItems() == 0; }
    /// Get number of items (including child itemsets)
    int GetItems() const { return TotalCnt; }
    /// Get item at given index (including child itemsets)
    const TItem& GetItem(const int& ItemN) const;
    /// Get items into vector
    void GetItemV(TVec<TItem>& _ItemV);
    /// Like GetItemV, but only collects items from child vectors whose stored [MinItem, MaxItem]
    /// range overlaps the half-open query range [MinItem, MaxItem). Children that lie entirely
    /// outside the range are skipped without being loaded from disk (they are the expensive part).
    /// The working buffer is always included. Relies only on the per-child min/max metadata, so it
    /// is correct regardless of whether children overlap each other.
    void GetItemVInRange(const TItem& MinItem, const TItem& MaxItem, TVec<TItem>& _ItemV);
    /// Go over all children and working buffer and pass it to HandleItemV function
    template <typename THandler> void GetItemV(THandler& Handler);
    /// Delete specified item from this itemset
    void DelItem(const TItem& Item);
    /// Delete a batch of items from this itemset in one go. Appends all delete markers before
    /// any flush, so the whole batch is drained by a single linear ProcessDeletes pass instead
    /// of paying a Def() per deleted item (the work buffer may temporarily exceed SplitLen).
    void DelItemV(const TVec<TItem>& DelV);
    /// Delete every item below MinKeepItem, reading almost none of the affected data: whole
    /// children with MaxItem below the threshold are dropped from the header alone (their blobs
    /// freed unread), the single straddling child is loaded and its prefix cut, and the sorted
    /// work buffer is cut at the threshold. Returns the number of items removed. Only acts on a
    /// merged itemset with no pending deletes (the state every itemset is saved in) - otherwise
    /// the sorted-prefix reasoning does not hold and the call is a no-op returning 0.
    uint64 DelItemsBelow(const TItem& MinKeepItem);
    /// Clear all items from this itemset
    void Clr();

    /// Pack/merge this itemset
    void Def();
    /// Pack/merge working buffer from this itemset
    void DefLocal();

    /// Flag if itemset is merged
    bool IsMerged() const { return MergedP; }
    /// Flag if itemset is dirty
    bool IsDirty() const { return DirtyP; }
    /// Tests if current itemset is full and subsequent item should be pushed to children
    bool IsFull() const { return (ItemV.Len() >= SplitLen); }
    /// Get number of child vectors of this itemset
    int GetChildVectors() const { return ChildInfoV.Len(); }
    /// Get split length used by this itemset
    int GetSplitLen() const { return SplitLen; }

    /// Compute percentage of loaded child vectors
    double GetLoadedPerc() const;

#ifdef XTEST
    // only exposed in test
    friend class XTest;
    void Print() const;
#endif

    /// Smart pointer is a friend
    friend class TPt<TGixItemSet>;
};

//////////////////////////////////////////////////
// Basic statistics for TGix
struct TGixStats {
public:
    /// Number of itemsets in cache
    TInt CacheAll;
    /// Number of dirty itemsets in cache
    TInt CacheDirty;
    /// Percentage of loaded content for itemsets in cache
    TFlt CacheAllLoadedPerc;
    /// Percentage of loaded content for dirty itemsets in cache
    TFlt CacheDirtyLoadedPerc;
    /// Average length of itemsets in cache
    TFlt AvgLen;
    /// memory usage for gix
    TUInt64 MemUsed;

public:

    /// This method combines statistics from to Gix objects
    void Add(const TGixStats& Stats) {
        // compute summed stats
        TGixStats NewStats;
        NewStats.CacheAll = CacheAll + Stats.CacheAll;
        NewStats.CacheDirty = CacheDirty + Stats.CacheDirty;
        if (NewStats.CacheAll > 0) {
            NewStats.CacheAllLoadedPerc = (CacheAll * CacheAllLoadedPerc + Stats.CacheAll * Stats.CacheAllLoadedPerc) / NewStats.CacheAll;
            NewStats.AvgLen = (CacheAll * AvgLen + Stats.CacheAll * Stats.AvgLen) / NewStats.CacheAll;
        }
        if (NewStats.CacheDirty > 0) {
            NewStats.CacheDirtyLoadedPerc = (CacheDirty * CacheDirtyLoadedPerc + Stats.CacheDirty * Stats.CacheDirtyLoadedPerc) / NewStats.CacheDirty;
        }
        NewStats.MemUsed = MemUsed + Stats.MemUsed;
        // replace this stats with summed up ones
        *this = NewStats;
    }
};

/////////////////////////////////////////////////
// General Inverted Index
template <class TKey, class TItem>
class TGix {
private:
    TCRef CRef;
    typedef TPt<TGix<TKey, TItem> > PGix;
    typedef TPt<TGixItemSet<TKey, TItem> > PGixItemSet;
    typedef TPt<TGixKeyStr<TKey> > PGixKeyStr;

private:
    /// File access mode - checked during index operations
    TFAccess Access;
    /// Name of the main file
    TStr GixFNm;
    /// Name of the BLOB file
    TStr GixBlobFNm;
    /// mapping between key and BLOB pointer. The representation (hash or
    /// sorted arrays) is chosen at creation and self-described in the file
    TGixKeyDict<TKey> KeyIdH;
    /// set when KeyIdH was modified since load; when still false at destruction
    /// time the (potentially huge) dictionary is not rewritten to GixFNm
    bool KeyIdHDirtyP;

    /// ItemHandler used for packing item vectors in item sets
    const TGixItemHandler<TKey, TItem>* ItemHandler;

    /// Item set cache
    mutable TCache<TBlobPt, PGixItemSet> ItemSetCache;
    /// Disk storage (blob base)
    PBlobBs ItemSetBlobBs;

    /// Threshold for recomputing size of the cache
    uint64 CacheResetThreshold;
    /// Cache size change since last reset
    mutable uint64 NewCacheSizeInc;
    /// flag indicating cache is full (mutable - the cache is refreshed/purged also
    /// from the const read path, see GetItemSet)
    mutable bool CacheFullP;
    /// When set, dirty itemsets leaving the cache (LRU purge, DropFromCache) are
    /// DISCARDED instead of stored. Only safe when every itemset's blob content is
    /// current (call Flush() first): from then on itemsets only become dirty through
    /// content-preserving transformations (Def() merging), so their stored form is
    /// equivalent. Used on throwaway gixes scanned by CopyTo (the reindex stage),
    /// where the write-back would pointlessly rewrite the blob while it is read.
    TBool DiscardDirtyOnDropP;

    /// Size of work-buffer
    TInt SplitLen;
    /// Minimal length for child vectors
    TInt SplitLenMin;
    /// Maximal length for child vectors
    TInt SplitLenMax;
    /// Can the first child vector be of any non-empty size and not be merged with following vectors
    /// This can significantly speed-up deleting items from Gix
    TBool FirstChildBeUnfilledP;
    /// Optional provider of per-key split lengths. When NULL or when the provider
    /// returns -1 for a key, the default SplitLen/SplitLenMin/SplitLenMax are used.
    /// Not owned by gix. Must be set before any itemsets are created or loaded.
    const TGixSplitLenProvider<TKey>* SplitLenProvider;

    /// Internal member for holding statistics
    mutable TGixStats Stats;

private:
    /// Handler used by CopyTo that appends the streamed item vectors
    /// (child vectors + work buffer) into another gix under a fixed key
    class TCopyToHandler {
    private:
        TGix<TKey, TItem>& DestGix;
        TKey Key;
    public:
        TCopyToHandler(TGix<TKey, TItem>& _DestGix, const TKey& _Key): DestGix(_DestGix), Key(_Key) {}
        void operator()(const TVec<TItem>& ItemV) { if (!ItemV.Empty()) { DestGix.AddItemV(Key, ItemV); } }
    };

private:
    /// Returns pointer to this object. Used in cache call-backs
    void* GetVoidThis() const { return (void*)this; }
    /// asserts if we are allowed to change this index
    void AssertReadOnly() const;

    /// Remove the itemset for given key from the cache without storing it.
    /// Used during full index scans (CopyTo, VerifySample) - loading child vectors
    /// is not accounted in the cache size, so a scan would otherwise grow the
    /// cache without bound.
    void DropFromCache(const TKey& Key) const;
    /// Store the itemset for given key (if dirty) and remove it from the cache.
    /// Used by CopyTo on the DESTINATION gix: keys are copied in sorted order and
    /// never revisited, so keeping every finished itemset cached until the LRU
    /// purge would make the periodic clean-up walks scale with the number of
    /// copied keys instead of the cache limit.
    void StoreAndDropFromCache(const TKey& Key);

    /// cache-lookup / blob-load / cache-insert core of GetItemSet. Deliberately
    /// does NOT call RefreshMemUsed: the purge it triggers stores dirty itemsets,
    /// and a store can RELOCATE an itemset's blob (freeing the old one) and update
    /// KeyIdH - so the blob pointer passed here must be resolved AFTER the last
    /// potential purge, never before (the pre-2026-08 GetItemSet did it the other
    /// way around and read freed/reused blobs when the purge evicted the very key
    /// being fetched - the ER7 reindex stage "corruption")
    PGixItemSet GetItemSetNoRefresh(const TBlobPt& Pt) const;

    /// get keyid of a given key and create it if does not exist
    TBlobPt AddKeyId(const TKey& Key);
    /// get keyid of a given key
    TBlobPt GetKeyId(const TKey& Key) const;

    /// Get handle to the merger
    const TGixItemHandler<TKey, TItem>* GetItemHandler() const { return ItemHandler; }

    /// Load child vector for given blob pointer from disk
    void GetChildVector(const TBlobPt& Pt, TVec<TItem>& Dest) const;
    /// Store child vectors to disk and get back pointer to where it was stored.
    TBlobPt StoreChildVector(const TBlobPt& ExistingKeyId, const TVec<TItem>& Data) const;
    /// Delete child vectors from cache and disk
    void DeleteChildVector(const TBlobPt& KeyId) const;
    /// For enlisting new child vectors into blob
    TBlobPt EnlistChildVector(const TVec<TItem>& Data) const;

    /// This method refreshes gix statistics
    void RefreshStats() const;

    TGix(const TStr& Nm, const TStr& FPath, const TFAccess& _Access,
        const TGixItemHandler<TKey, TItem>* ItemHandler, const int64& CacheSize,
        const int _SplitLen, const bool _FirstChildBeUnfilledP,
        const int _SplitLenMin, const int _SplitLenMax,
        const TGixKeyDictType _KeyDictType);
public:
    /// KeyDictType selects the key-dictionary representation for a NEWLY
    /// CREATED gix (Access == faCreate); an existing gix always opens with the
    /// representation persisted in its .Gix file
    static PGix New(const TStr& Nm, const TStr& FPath, const TFAccess& Access,
        const TGixItemHandler<TKey, TItem>* ItemHandler, const int64& CacheSize = 100000000,
        const int SplitLen = 1024, const bool FirstChildBeUnfilledP = true,
        const int SplitLenMin = 512, const int SplitLenMax = 2048,
        const TGixKeyDictType KeyDictType = gkdtHash) {
        return new TGix(Nm, FPath, Access, ItemHandler, CacheSize, SplitLen,
            FirstChildBeUnfilledP, SplitLenMin, SplitLenMax, KeyDictType);
    }

    ~TGix();

    // Gix properties
    bool IsReadOnly() const { return Access == faRdOnly; }
    bool IsCacheFullP() const { return CacheFullP; }
    TStr GetFPath() const { return GixFNm.GetFPath(); }
    int64 GetMxCacheSize() const { return GetMxMemUsed(); }
    int GetSplitLen() const { return SplitLen; }
    int GetSplitLenMax() const { return SplitLenMax; }
    int GetSplitLenMin() const { return SplitLenMin; }
    bool CanFirstChildBeUnfilled() const { return FirstChildBeUnfilledP; }

    /// Set provider of per-key split lengths. Must be called right after creating
    /// the gix, before any itemsets are created or loaded (their split lengths are
    /// resolved once, at construction). Provider is not owned by the gix.
    void SetSplitLenProvider(const TGixSplitLenProvider<TKey>* Provider) { SplitLenProvider = Provider; }
    /// Get split length for given key (per-key override or default)
    int GetSplitLen(const TKey& Key) const {
        if (SplitLenProvider != NULL) {
            const int KeySplitLen = SplitLenProvider->GetSplitLen(Key);
            if (KeySplitLen > 0) { return KeySplitLen; }
        }
        return SplitLen;
    }
    /// Get minimal tolerated child vector length for given key
    int GetSplitLenMin(const TKey& Key) const {
        if (SplitLenProvider != NULL) {
            const int KeySplitLen = SplitLenProvider->GetSplitLen(Key);
            // derive min the same way as the defaults (SplitLenMin = SplitLen / 2)
            if (KeySplitLen > 0) { return KeySplitLen / 2; }
        }
        return SplitLenMin;
    }
    /// Get maximal tolerated child vector length for given key
    int GetSplitLenMax(const TKey& Key) const {
        if (SplitLenProvider != NULL) {
            const int KeySplitLen = SplitLenProvider->GetSplitLen(Key);
            // derive max the same way as the defaults (SplitLenMax = SplitLen * 2)
            if (KeySplitLen > 0) { return 2 * KeySplitLen; }
        }
        return SplitLenMax;
    }

    /// do we have Key in the index?
    bool IsKey(const TKey& Key) const { return KeyIdH.IsKey(Key); }
    /// number of keys in the index
    int GetKeys() const { return KeyIdH.Len(); }
    /// sort keys
    void SortKeys() { KeyIdH.SortByKey(true); KeyIdHDirtyP = true; }

    /// was the key hash modified since the gix was created/loaded?
    bool IsKeyIdHDirty() const { return KeyIdHDirtyP; }

    /// representation of the key dictionary (hash or sorted arrays)
    TGixKeyDictType GetKeyDictType() const { return KeyIdH.GetType(); }
    /// convert the key dictionary to the given representation in memory (the
    /// posting blobs are untouched). Free on an empty gix - used right after
    /// creation to apply a schema-requested representation; on a populated gix
    /// both representations exist transiently while converting
    void ConvertKeyDictTo(const TGixKeyDictType& NewType) {
        if (KeyIdH.GetType() == NewType) { return; }
        AssertReadOnly();
        KeyIdH.ConvertTo(NewType);
        KeyIdHDirtyP = true;
    }
    /// write the key dictionary to FNm in the given representation without
    /// modifying this gix - the posting blobs are referenced unchanged, so the
    /// written file can replace this gix's .Gix file to convert the index
    void SaveKeyDictFileAsType(const TStr& FNm, const TGixKeyDictType& TargetType) const {
        KeyIdH.SaveFileAsType(FNm, TargetType); }

    /// get item set for given key. Verifies that the itemset loaded from the blob
    /// really belongs to Key, so a wrong-content read (stale pointer into a freed
    /// and reused blob slot) fails loudly instead of serving another key's postings
    PGixItemSet GetItemSet(const TKey& Key) const;
    /// Get items for given key
    void GetItemV(const TKey& Key, TVec<TItem>& ItemV) const;
    /// Like GetItemV, but only returns items whose value lies in the half-open range [MinItem, MaxItem).
    /// Uses per-child min/max metadata to avoid loading child vectors that fall entirely outside the range.
    void GetItemVInRange(const TKey& Key, const TItem& MinItem, const TItem& MaxItem, TVec<TItem>& ItemV) const;
    /// Go over all children and working buffer and pass it to HandleItemV function
    template <typename THandler> void GetItemV(const TKey& Key, THandler& Handler) const;
    /// for storing item sets from cache to blob
    TBlobPt StoreItemSet(const TBlobPt& KeyId);
    /// for deleting itemset from cache and blob
    void DeleteItemSet(const TKey& Key);
    /// For enlisting new itemsets into blob
    TBlobPt EnlistItemSet(const PGixItemSet& ItemSet) const;

    /// adding new item to the inverted index
    void AddItem(const TKey& Key, const TItem& Item);
    /// adding new items to the inverted index
    void AddItemV(const TKey& Key, const TVec<TItem>& ItemV);
    // delete one item
    void DelItem(const TKey& Key, const TItem& Item);
    /// delete a batch of items under one key with a single work-buffer flush
    void DelItemV(const TKey& Key, const TVec<TItem>& DelV);
    /// delete every item below MinKeepItem under one key without reading the affected
    /// children (see TGixItemSet::DelItemsBelow). Returns the number of items removed.
    uint64 DelItemsBelow(const TKey& Key, const TItem& MinKeepItem);
    /// clears items
    void Clr(const TKey& Key);
    /// flush all data from cache to disk
    void Flush() { ItemSetCache.FlushAndClr(); }
    /// flush a portion of data from cache to disk
    int PartialFlush(int WndInMsec = 500);

    /// get first key id
    int FFirstKeyId() const { return KeyIdH.FFirstKeyId(); }
    /// get next key id
    bool FNextKeyId(int& KeyId) const { return KeyIdH.FNextKeyId(KeyId); }
    /// get key for given key id
    const TKey& GetKey(const int& KeyId) const { return KeyIdH.GetKey(KeyId); }
    /// is the given key id still valid (its key not deleted)?
    bool IsKeyId(const int& KeyId) const { return KeyIdH.IsKeyId(KeyId); }
    /// blob pointer of the itemset stored under the given key id. Sorting key ids by their
    /// blob pointer lets a whole-index scan read the itemsets in disk order (mostly
    /// sequential I/O) instead of a random read per key in hash order
    TBlobPt GetKeyBlobPt(const int& KeyId) const { return KeyIdH[KeyId]; }

    /// Get amount of memory currently used
    int64 GetMemUsed() const;
    /// Get current cache increment size count
    int GetNewCacheSizeInc() const { return NewCacheSizeInc; }
    /// Get current cache size
    uint64 GetCacheSize() const { return ItemSetCache.GetMemUsed(); }
    /// Get maximal memory that can be used by the cache
    uint64 GetMxMemUsed() const { return ItemSetCache.GetMxMemUsed(); }
    /// Is cache full?
    bool IsCacheFull() const { return CacheFullP; }
    /// Cache growth after which RefreshMemUsed recomputes and purges (default: 10% of cache size)
    uint64 GetCacheResetThreshold() const { return CacheResetThreshold; }
    /// Override the clean-up threshold. Every clean-up pass walks the whole cache, so bulk
    /// operations (e.g. batch deletes) can raise it to trade fewer passes for the cache
    /// temporarily overshooting its limit by up to the threshold.
    void SetCacheResetThreshold(const uint64& Threshold) { CacheResetThreshold = Threshold; }
    /// Refresh current memory computations and purge the cache if needed.
    /// const so the read path (GetItemSet) can trigger it as well - reads grow the
    /// cache too (loading itemsets and their child vectors)
    void RefreshMemUsed() const;
    /// Update cache increment
    void AddToNewCacheSizeInc(const uint64& Diff) const { NewCacheSizeInc += Diff; }
    /// Update cache increment (or decrement)
    void AddToNewCacheSizeInc(const uint64& OldSize, const uint64& NewSize) const;

    /// Discard (instead of store) dirty itemsets that leave the cache. ONLY safe
    /// right after Flush(): every blob is then current and itemsets re-dirty only
    /// through content-preserving merges (Def()), so the stored form stays
    /// equivalent. See DiscardDirtyOnDropP.
    void SetDiscardDirtyOnDrop(const bool& DiscardP) { DiscardDirtyOnDropP = DiscardP; }
    /// Are dirty itemsets leaving the cache discarded instead of stored?
    bool IsDiscardDirtyOnDrop() const { return DiscardDirtyOnDropP; }


    /// Copy the complete content of this gix into DestGix. Keys are processed in
    /// sorted order and one key at a time, so all child vectors of one key (and of
    /// neighboring words of the same index key) end up stored contiguously in the
    /// destination blob base. Data is streamed one child vector at a time, so memory
    /// use stays bounded even for very large keys. The destination applies its own
    /// (possibly different) split lengths. Item counts are verified for every key.
    /// Copy the full content of this gix into DestGix. Every copied key's item count
    /// is asserted against the destination. Optionally reports the total items copied
    /// and the number of source keys with no items (fully deleted posting lists) -
    /// such keys are not created in the destination, which is the one legitimate way
    /// the destination key count may be lower than the source key count.
    /// An optional KeyFilter restricts the copy to the keys it keeps; filtered-out
    /// keys are not read at all and are not counted as copied or empty.
    /// When FailedKeyV is given, a key whose source itemset cannot be read (or whose
    /// copy fails verification) is recorded and reported instead of aborting the
    /// whole copy, and the scan continues - the caller gets the complete list of
    /// broken keys in one pass. The destination MUST NOT be used when any key
    /// failed (it may hold a partial itemset for such keys). Without FailedKeyV the
    /// first failure propagates as before.
    void CopyTo(TGix<TKey, TItem>& DestGix, uint64* CopiedItemsOut = NULL, int* EmptyKeysOut = NULL,
        const TGixKeyFilter<TKey>* KeyFilter = NULL, TVec<TKey>* FailedKeyV = NULL) const;
    /// Diagnostic full scan: try to completely read every itemset (header blob and
    /// all child vectors) without modifying anything. Keys are visited in blob-pointer
    /// order, so the scan reads the blob mostly sequentially. Each unreadable key is
    /// reported into FailedKeyStrV as "<seg:addr> <error message>". Returns the
    /// number of keys scanned.
    int VerifyAllKeys(TStrV& FailedKeyStrV) const;
    /// Compare data stored under the given key in this and the other gix.
    /// Keys with more than MxItems items are compared by count and first/last item only.
    bool IsKeyDataEqual(const TGix<TKey, TItem>& OtherGix, const TKey& Key, const int& MxItems = 5000000) const;
    /// Compare data of (approximately) SampleKeys keys, evenly sampled over all keys,
    /// between this and the other gix. Returns false if any compared key differs.
    /// An optional KeyFilter restricts the sampling to the keys it keeps.
    bool VerifySample(const TGix<TKey, TItem>& OtherGix, const int& SampleKeys,
        const TGixKeyFilter<TKey>* KeyFilter = NULL) const;

    /// print statistics for index keys
    void SaveTxt(const TStr& FNm, const PGixKeyStr& KeyStr) const;
    /// print simple statistics for cache
    void PrintStats();
    /// get blob stats
    const TBlobBsStats& GetBlobStats() { return ItemSetBlobBs->GetStats(); }
    /// get gix stats
    const TGixStats& GetGixStats(const bool& RefreshP = true) const {
        if (RefreshP) { RefreshStats(); } return Stats; }
    /// reset blob stats
    void ResetStats() { ItemSetBlobBs->ResetStats(); }

#ifdef XTEST
    friend class XTest;
    void KillHash() { this->KeyIdH.Clr(); this->KeyIdHDirtyP = true; }
    void KillCache() { this->ItemSetCache.FlushAndClr(); }
#endif

    /// Smartpointer friend
    friend class TPt<TGix>;
    /// Itemset needs access to the disk storage and merger
    friend class TGixItemSet<TKey, TItem>;
};

/////////////////////////////////////////////////
/// Item Vector Merger.
/// Used when evaluating queries to apply logical operators to item vectors.
template <class TKey, class TItem, class TResItem>
class TGixMerger {
public:
    virtual ~TGixMerger() {}

    /// Add elements in JoinV that are not yet in MainV. Both vectors must be sorted.
    virtual void Union(TVec<TResItem>& MainV, const TVec<TResItem>& JoinV) const = 0;
    /// Removed elements from MainV that are not in JoinV. Both vectors must be sorted.
    virtual void Intrs(TVec<TResItem>& MainV, const TVec<TResItem>& JoinV) const = 0;
    /// Adds elements from MainV that are not in JoinV to ResV. MainV and JoinV must be sorted.
    virtual void Minus(const TVec<TResItem>& MainV, const TVec<TResItem>& JoinV, TVec<TResItem>& ResV) const = 0;

    /// Initialize vector of items for given key.
    virtual void Def(const TKey& Key, TVec<TItem>& MainV, TVec<TResItem>& ResV) const = 0;

    /// Memory footprint
    virtual uint64 GetMemUsed() const = 0;
};

/////////////////////////////////////////////////
/// Default Item Vector Merger.
/// Uses basic set operations defined on TVec. Assumes TItem == TResItem.
template <class TKey, class TItem, class TResItem>
class TGixDefMerger : public TGixMerger <TKey, TItem, TResItem> {
public:
    void Union(TVec<TResItem>& MainV, const TVec<TResItem>& JoinV) const { MainV.Union(JoinV); }
    void Intrs(TVec<TResItem>& MainV, const TVec<TResItem>& JoinV) const { MainV.Intrs(JoinV); }
    void Minus(const TVec<TResItem>& MainV, const TVec<TResItem>& JoinV, TVec<TResItem>& ResV) const { MainV.Diff(JoinV, ResV); }
    void Def(const TKey& Key, TVec<TItem>& MainV, TVec<TResItem>& ResV) const { ResV.MoveFrom(MainV); }

    uint64 GetMemUsed() const { return sizeof(TGixDefMerger<TKey, TItem, TResItem>); }
};

/////////////////////////////////////////////////
/// Expression item types
typedef enum {
    getUndef,
    getEmpty, //< Empty item
    getOr,    //< Logical or
    getAnd,   //< Logical and
    getNot,   //< Logical not
    getKey    //< Lookup for items index under key in Gix
} TGixExpType;

/////////////////////////////////////////////////
/// Expression item
template <class TKey, class TItem, class TResItem>
class TGixExpItem {
private:
    TCRef CRef;
    typedef TPt<TGixExpItem<TKey, TItem, TResItem> > PGixExpItem;
    typedef TPt<TGixItemSet<TKey, TItem> > PGixItemSet;
    typedef TPt<TGix<TKey, TItem> > PGix;

private:
    /// Type of the opreation handled by this expression item
    TGixExpType ExpType;
    /// Left subtree (used by getOr and getAnd)
    PGixExpItem LeftExpItem;
    /// Right subtree (used by getOr, getAnd and getNot)
    PGixExpItem RightExpItem;
    /// Search key (used by getKey)
    TKey Key;

private:
    /// Convert expression item to AND
    void PutAnd(const PGixExpItem& _LeftExpItem, const PGixExpItem& _RightExpItem);
    /// Convert expression item to OR
    void PutOr(const PGixExpItem& _LeftExpItem, const PGixExpItem& _RightExpItem);

    TGixExpItem(const TGixExpType& _ExpType, const PGixExpItem& _LeftExpItem,
        const PGixExpItem& _RightExpItem) : ExpType(_ExpType),
        LeftExpItem(_LeftExpItem), RightExpItem(_RightExpItem) {}
    TGixExpItem(const TKey& _Key) : ExpType(getKey), Key(_Key) {}
    TGixExpItem() : ExpType(getEmpty) {}
    TGixExpItem(const TGixExpItem& ExpItem) : ExpType(ExpItem.ExpType),
        LeftExpItem(ExpItem.LeftExpItem), RightExpItem(ExpItem.RightExpItem),
        Key(ExpItem.Key) {}

public:
    // elementary operations
    static PGixExpItem NewOr(const PGixExpItem& LeftExpItem, const PGixExpItem& RightExpItem) {
        return new TGixExpItem(getOr, LeftExpItem, RightExpItem); }
    static PGixExpItem NewAnd(const PGixExpItem& LeftExpItem, const PGixExpItem& RightExpItem) {
        return new TGixExpItem(getAnd, LeftExpItem, RightExpItem); }
    static PGixExpItem NewNot(const PGixExpItem& RightExpItem) {
        return new TGixExpItem(getNot, NULL, RightExpItem); }
    static PGixExpItem NewItem(const TKey& Key) {
        return new TGixExpItem(Key); }
    static PGixExpItem NewEmpty() {
        return new TGixExpItem(); }

    /// Create an AND tree from given array of expression items
    static PGixExpItem NewAndV(const TVec<PGixExpItem>& ExpItemV);
    /// Create an OR tree from given array of expression items
    static PGixExpItem NewOrV(const TVec<PGixExpItem>& ExpItemV);
    /// Create an AND tree from given array of leaf items
    static PGixExpItem NewAndV(const TVec<TKey>& KeyV);
    /// Create an OR tree from given array of leaf items
    static PGixExpItem NewOrV(const TVec<TKey>& KeyV);

    /// Is current item empty?
    bool IsEmpty() const { return (ExpType == getEmpty); }
    /// Get type of expression item
    TGixExpType GetExpType() const { return ExpType; }
    /// Get expression item Key
    TKey GetKey() const { return Key; }
    /// Clone expression item
    PGixExpItem Clone() const { return new TGixExpItem(*this); }

    /// Evaluate expression item using given merger and return mathed items
    bool Eval(const PGix& Gix, TVec<TResItem>& ResItemV, const TGixMerger<TKey, TItem, TResItem>* Merger);

    friend class TPt<TGixExpItem>;
};

#include "gix.hpp"

#endif
