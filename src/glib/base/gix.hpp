/////////////////////////////////////////////////
/// General Inverted Index Item Set.
template <class TKey, class TItem>
void TGixItemSet<TKey, TItem>::TChildInfo::Load(TSIn& SIn) {
    MinItem = TItem(SIn);
    MaxItem = TItem(SIn);
    Len.Load(SIn);
    Pt = TBlobPt(SIn);
}

template <class TKey, class TItem>
void TGixItemSet<TKey, TItem>::TChildInfo::Save(TSOut& SOut) const {
    MinItem.Save(SOut);
    MaxItem.Save(SOut);
    Len.Save(SOut);
    Pt.Save(SOut);
}

template <class TKey, class TItem>
uint64 TGixItemSet<TKey, TItem>::TChildInfo::GetMemUsed() const {
    return sizeof(TChildInfo) +
        TMemUtils::GetExtraMemberSize(MinItem) +
        TMemUtils::GetExtraMemberSize(MaxItem) +
        TMemUtils::GetExtraMemberSize(Len) +
        TMemUtils::GetExtraMemberSize(Pt) +
        TMemUtils::GetExtraMemberSize(LoadedP) +
        TMemUtils::GetExtraMemberSize(DirtyP);
}

template <class TKey, class TItem>
void TGixItemSet<TKey, TItem>::ResolveSplitLen() {
    SplitLen = Gix->GetSplitLen(ItemSetKey);
    SplitLenMin = Gix->GetSplitLenMin(ItemSetKey);
    SplitLenMax = Gix->GetSplitLenMax(ItemSetKey);
}

template <class TKey, class TItem>
void TGixItemSet<TKey, TItem>::LoadChildVector(const int& ChildN) const {
    if (!ChildInfoV[ChildN].LoadedP) {
        // load child vector from disk
        Gix->GetChildVector(ChildInfoV[ChildN].Pt, ChildV[ChildN]);
        // mark that it is freshly loaded
        ChildInfoV[ChildN].LoadedP = true;
        ChildInfoV[ChildN].DirtyP = false;
        // report the growth to the gix - this itemset most likely lives in the
        // itemset cache and loaded children are its dominant memory (they stay
        // loaded until the whole itemset is evicted). without this the cache
        // accounting never saw read-driven growth
        Gix->AddToNewCacheSizeInc(TMemUtils::GetExtraMemberSize(ChildV[ChildN]));
    }
}

template <class TKey, class TItem>
void TGixItemSet<TKey, TItem>::LoadChildVectors() const {
    for (int ChildN = 0; ChildN < ChildInfoV.Len(); ChildN++) {
        LoadChildVector(ChildN);
    }
}

template <class TKey, class TItem>
void TGixItemSet<TKey, TItem>::RecalcTotalCnt() {
    TotalCnt = ItemV.Len();
    for (int ChildN = 0; ChildN < ChildInfoV.Len(); ChildN++) {
        TotalCnt += ChildInfoV[ChildN].Len;
    }
}

template <class TKey, class TItem>
int TGixItemSet<TKey, TItem>::FirstDirtyChild() {
    for (int ChildN = 0; ChildN < ChildInfoV.Len(); ChildN++) {
        if (ChildInfoV[ChildN].DirtyP && ChildInfoV[ChildN].Len < SplitLenMin) {
            return ChildN;
        }
        if (ChildInfoV[ChildN].DirtyP && ChildInfoV[ChildN].Len > SplitLenMax) {
            return ChildN;
        }
    }
    return -1;
}

template <class TKey, class TItem>
int TGixItemSet<TKey, TItem>::GetFirstChildToMerge() {
    // start checking either from 0 or 1 depending on whether we allow the first child to be
    for (int ChildN = 0; ChildN < ChildInfoV.Len(); ChildN++) {
        // the child at least needs to be dirty to be merged
        if (!ChildInfoV[ChildN].DirtyP) {
            continue;
        }
        // if child is not out of the size boundaries it also doesn't need to be merged
        if (ChildInfoV[ChildN].Len >= SplitLenMin && ChildInfoV[ChildN].Len <= SplitLenMax) {
            continue;
        }
        // for the first child we might allow it to be extra short, without need for merge
        // when removing oldest items, the first vector will be becoming shorter and shorter
        // and will be removed completely once empty
        if (ChildN == 0 && Gix->CanFirstChildBeUnfilled() && ChildInfoV[ChildN].Len <= SplitLenMax) {
            continue;
        }
        // otherwise, yes, it needs to be merged
        return ChildN;
    }
    return -1;
}

template <class TKey, class TItem>
bool TGixItemSet<TKey, TItem>::HasOversizedChild() const {
    for (int ChildN = 0; ChildN < ChildInfoV.Len(); ChildN++) {
        if (ChildInfoV[ChildN].Len > SplitLenMax) { return true; }
    }
    return false;
}

template <class TKey, class TItem>
void TGixItemSet<TKey, TItem>::CoalesceUndersizedChildren() {
    // drop children that deletes emptied completely - anywhere, not just at the front
    for (int ChildN = ChildInfoV.Len() - 1; ChildN >= 0; ChildN--) {
        if (ChildInfoV[ChildN].Len == 0) {
            Gix->DeleteChildVector(ChildInfoV[ChildN].Pt);
            ChildInfoV.Del(ChildN);
            ChildV.Del(ChildN);
        }
    }
    // merge adjacent undersized children so repeated batch deletes don't accumulate fragments.
    // items are globally sorted across children, so gluing neighbors preserves the order
    int ChildN = 0;
    while (ChildN + 1 < ChildInfoV.Len()) {
        if (ChildInfoV[ChildN].Len < SplitLenMin && ChildInfoV[ChildN + 1].Len < SplitLenMin &&
            ChildInfoV[ChildN].Len + ChildInfoV[ChildN + 1].Len <= SplitLenMax) {
            LoadChildVector(ChildN);
            LoadChildVector(ChildN + 1);
            ChildV[ChildN].AddV(ChildV[ChildN + 1]);
            ChildInfoV[ChildN].Len = ChildV[ChildN].Len();
            // stats stay usable the same way deletes keep them usable: Min can only be
            // conservative, Max comes from the absorbed right neighbor
            ChildInfoV[ChildN].MaxItem = ChildInfoV[ChildN + 1].MaxItem;
            ChildInfoV[ChildN].DirtyP = true;
            Gix->DeleteChildVector(ChildInfoV[ChildN + 1].Pt);
            ChildInfoV.Del(ChildN + 1);
            ChildV.Del(ChildN + 1);
            // stay on ChildN - the merged child may absorb its next neighbor too
        } else {
            ChildN++;
        }
    }
    DirtyP = true;
}

template <class TKey, class TItem>
void TGixItemSet<TKey, TItem>::PushWorkBufferToChildren() {
    // push work-buffer into children array
    while (ItemV.Len() >= SplitLen) {
        // create a vector of SplitLen items
        TVec<TItem> SplitItemV;
        ItemV.GetSubValV(0, SplitLen - 1, SplitItemV);
        // create the child info for the vector and also push the vector to a blob
        TChildInfo ChildInfo(SplitItemV[0], SplitItemV.Last(), SplitLen, Gix->EnlistChildVector(SplitItemV));
        ChildInfo.LoadedP = false;
        ChildInfo.DirtyP = false;
        ChildInfoV.Add(ChildInfo);
        // add an empty vector to ChildV - the data for this vector will be loaded from the blob when necessary
        ChildV.Add(TVec<TItem>());
        ItemV.Del(0, SplitLen - 1);
        DirtyP = true;
    }
}

template <class TKey, class TItem>
void TGixItemSet<TKey, TItem>::InjectWorkBufferToChildren() {
    AssertR(ItemV.IsSorted(), "Items in working buffer ItemV should be sorted");
    if (ChildInfoV.Len() > 0 && ItemV.Len() > 0) {
        // find the first Child index into which we need to insert the first value
        // since items in ItemV will most likely have the highest values, it makes sense to go from end backwards
        int ChildN = ChildInfoV.Len()-1;
        const TItem& FirstItem = ItemV[0];
        while (ChildN > 0 && Gix->GetItemHandler()->IsLt(FirstItem, ChildInfoV[ChildN].MinItem)) {
            ChildN--;
        }
        // go from ChildN onward, inserting items from ItemV into ChildInfoV
        int ItemN = 0;
        TIntSet TouchedVectorH;
        while (ItemN < ItemV.Len()) {
            const TItem& Item = ItemV[ItemN];
            while (ChildN < ChildInfoV.Len() && Gix->GetItemHandler()->IsLt(ChildInfoV[ChildN].MaxItem, Item)) {
                ChildN++;
            }
            // if val is larger than MaxItem in last ChildInfoV vector, then all remaining values in input buffer will not be inserted into child vectors
            if (ChildN >= ChildInfoV.Len()) {
                break;
            }
            // ok, insert into j-th child
            LoadChildVector(ChildN);
            ChildV[ChildN].Add(Item);
            ChildInfoV[ChildN].Len = ChildV[ChildN].Len();
            ChildInfoV[ChildN].DirtyP = true;
            TouchedVectorH.AddKey(ChildN);
            ItemN++;
        }

        // delete items from work-buffer that have been inserted into child vectors
        if (ItemN > 0) {
            // we made at least one insertion into the children, mark itemset as dirty
            DirtyP = true;
            if (ItemN == ItemV.Len()) {
                // we inserted all items into children - clear the working buffer
                ItemV.Clr();
            } else {
                // clear only the items that were already inserted into children
                ItemV.Del(0, ItemN - 1);
            }
        }

        // go over all the vectors that we modified and merge + update stats for them
        for (int KeyId = TouchedVectorH.FFirstKeyId(); TouchedVectorH.FNextKeyId(KeyId); ) {
            int ind = TouchedVectorH.GetKey(KeyId);
            LoadChildVector(ind); // just in case - they should be in memory at this point anyway
            TVec<TItem>& cd = ChildV[ind];
            Gix->GetItemHandler()->Merge(cd, false);
            ChildInfoV[ind].Len = cd.Len();
            ChildInfoV[ind].DirtyP = true;
            if (cd.Len() > 0) {
                ChildInfoV[ind].MinItem = cd[0];
                ChildInfoV[ind].MaxItem = cd.Last();
            }
        }
    }
}

template <class TKey, class TItem>
void TGixItemSet<TKey, TItem>::PushMergedDataBackToChildren(
        const int& FirstChildToMerge, const TVec<TItem>& MergedItems) {

    int MergedItemN = 0;
    int Remaining = MergedItems.Len() - MergedItemN;
    int ChildN = FirstChildToMerge;
    while (MergedItemN < MergedItems.Len()) {
        if (ChildN < ChildInfoV.Len() && Remaining > SplitLen) {
            ChildV[ChildN].Clr();
            MergedItems.GetSubValV(MergedItemN, MergedItemN + SplitLen - 1, ChildV[ChildN]);
            ChildInfoV[ChildN].Len = ChildV[ChildN].Len();
            ChildInfoV[ChildN].MinItem = ChildV[ChildN][0];
            ChildInfoV[ChildN].MaxItem = ChildV[ChildN].Last();
            ChildInfoV[ChildN].DirtyP = true;
            ChildInfoV[ChildN].LoadedP = true;
            MergedItemN += ChildInfoV[ChildN].Len;
            Remaining = MergedItems.Len() - MergedItemN;
            ChildN++;
        } else {
            // put the remaining data into work-buffer
            ItemV.Clr();
            MergedItems.GetSubValV(MergedItemN, MergedItemN + Remaining - 1, ItemV);
            break;
        }
    }

    // remove children that became empty
    // remove them first from BLOB storage
    for (int Ind = ChildN; Ind < ChildInfoV.Len(); Ind++) {
        Gix->DeleteChildVector(ChildInfoV[Ind].Pt);
    }

    // finally remove them from memory
    if (ChildN < ChildInfoV.Len()) {
        ChildInfoV.Del(ChildN, ChildInfoV.Len() - 1);
        ChildV.Del(ChildN, ChildV.Len() - 1);
    }
    DirtyP = true;
}

template <class TKey, class TItem>
int TGixItemSet<TKey, TItem>::FindInSorted(const TVec<TItem>& SortedV, const TItem& Item) const {
    const TGixItemHandler<TKey, TItem>* ItemHandler = Gix->GetItemHandler();
    int Lo = 0, Hi = SortedV.Len() - 1;
    while (Lo <= Hi) {
        const int Mid = Lo + (Hi - Lo) / 2;
        if (ItemHandler->IsLt(SortedV[Mid], Item)) { Lo = Mid + 1; }
        else if (ItemHandler->IsLt(Item, SortedV[Mid])) { Hi = Mid - 1; }
        else { return Mid; }
    }
    return -1;
}

template <class TKey, class TItem>
int TGixItemSet<TKey, TItem>::FindChildToDeleteFrom(const TItem& Item) const {
    const TGixItemHandler<TKey, TItem>* ItemHandler = Gix->GetItemHandler();
    // child vectors hold disjoint item ranges in ascending order, so the only child that can
    // hold Item is the first one whose MaxItem reaches it
    int Lo = 0, Hi = ChildInfoV.Len() - 1, ChildN = -1;
    while (Lo <= Hi) {
        const int Mid = Lo + (Hi - Lo) / 2;
        if (ItemHandler->IsLtE(Item, ChildInfoV[Mid].MaxItem)) { ChildN = Mid; Hi = Mid - 1; }
        else { Lo = Mid + 1; }
    }
    // that child actually holds Item only if its range starts at or below it
    if (ChildN >= 0 && ItemHandler->IsLtE(ChildInfoV[ChildN].MinItem, Item)) { return ChildN; }
    return -1;
}

template <class TKey, class TItem>
void TGixItemSet<TKey, TItem>::ProcessDeletes() {
    if (ItemVDel.Empty()) { return; }
    const TGixItemHandler<TKey, TItem>* ItemHandler = Gix->GetItemHandler();

    // ItemVDel holds ascending positions into the work buffer ItemV; ItemV[Pos] is the value that
    // the marker at Pos deletes. The previous implementation replayed the markers one at a time,
    // and each replay called Delete() - a linear DelAll - once on the child vector and once on the
    // output buffer built so far. That is O(#markers * #items), and both counts are bounded by
    // SplitLen, so with a per-key split length of 100k a single full work buffer of deletes cost
    // on the order of 10^10 operations. A batch delete fills the work buffer with markers over and
    // over, which is what made deleting a large number of records collapse.
    //
    // Instead, collect the markers once, then do a single pass over the work buffer and a single
    // pass over each affected child. Semantics are unchanged: a marker removes every occurrence of
    // its value at or *before* its own position and leaves later re-adds of the same value alone,
    // so an item at position N survives iff no marker for the same value sits at a position >= N.
    // Only the last marker position per distinct value matters.
    TVec<TPair<TItem, TInt> > DelMarkerV(ItemVDel.Len(), 0);
    for (int MarkerN = 0; MarkerN < ItemVDel.Len(); MarkerN++) {
        const int Pos = ItemVDel[MarkerN];
        DelMarkerV.Add(TPair<TItem, TInt>(ItemV[Pos], Pos));
    }
    DelMarkerV.SortCmp(TGixDelMarkerCmp<TKey, TItem>(ItemHandler));

    // collapse the markers to distinct values, keeping the last marker position of each. the sort
    // tie-breaks on ascending position, so within a run of equal values the last one wins.
    TVec<TItem> DelItemV(DelMarkerV.Len(), 0);
    TVec<TInt> DelLastPosV(DelMarkerV.Len(), 0);
    for (int MarkerN = 0; MarkerN < DelMarkerV.Len(); MarkerN++) {
        const TItem& DelItem = DelMarkerV[MarkerN].Val1;
        if (!DelItemV.Empty() && !ItemHandler->IsLt(DelItemV.Last(), DelItem)) {
            DelLastPosV.Last() = DelMarkerV[MarkerN].Val2;
        } else {
            DelItemV.Add(DelItem);
            DelLastPosV.Add(DelMarkerV[MarkerN].Val2);
        }
    }

    // rebuild the work buffer in one pass, dropping each item killed by a marker at or after it
    TVec<TItem> ItemVNew(ItemV.Len(), 0);
    for (int ItemN = 0; ItemN < ItemV.Len(); ItemN++) {
        const int DelN = FindInSorted(DelItemV, ItemV[ItemN]);
        if (DelN >= 0 && DelLastPosV[DelN] >= ItemN) { continue; }
        ItemVNew.Add(ItemV[ItemN]);
    }

    // apply the deletes to the child vectors. group the values by the child that holds them so
    // each affected child is rewritten once, instead of being rescanned once per deleted value.
    // DelItemV is sorted and we append in order, so every per-child list comes out sorted too.
    THash<TInt, TVec<TItem> > ChildDelH;
    for (int DelN = 0; DelN < DelItemV.Len(); DelN++) {
        const int ChildN = FindChildToDeleteFrom(DelItemV[DelN]);
        if (ChildN >= 0) { ChildDelH.AddDat(ChildN).Add(DelItemV[DelN]); }
    }
    for (int KeyId = ChildDelH.FFirstKeyId(); ChildDelH.FNextKeyId(KeyId); ) {
        const int ChildN = ChildDelH.GetKey(KeyId);
        const TVec<TItem>& ChildDelItemV = ChildDelH[KeyId];
        LoadChildVector(ChildN);
        TVec<TItem>& ChildItemV = ChildV[ChildN];
        int KeepN = 0;
        for (int ItemN = 0; ItemN < ChildItemV.Len(); ItemN++) {
            if (FindInSorted(ChildDelItemV, ChildItemV[ItemN]) >= 0) { continue; }
            ChildItemV[KeepN++] = ChildItemV[ItemN];
        }
        ChildItemV.Trunc(KeepN);
        ChildInfoV[ChildN].Len = ChildItemV.Len();
        ChildInfoV[ChildN].DirtyP = true;
        // we don't update stats (min & max), because they are still usable.
    }

    ItemV.Clr();
    ItemVDel.Clr();
    ItemV.AddV(ItemVNew);
    DirtyP = true;
}

template <class TKey, class TItem>
TGixItemSet<TKey, TItem>::TGixItemSet(TSIn& SIn, const TGix<TKey, TItem>* _Gix):
    ItemSetKey(SIn), ItemV(SIn), ChildInfoV(SIn), MergedP(true), DirtyP(false), Gix(_Gix) {

    ResolveSplitLen();
    for (int ChildN = 0; ChildN < ChildInfoV.Len(); ChildN++) {
        ChildV.Add(TVec<TItem>());
    };
    RecalcTotalCnt();
}

template <class TKey, class TItem>
void TGixItemSet<TKey, TItem>::Save(TMOut& SOut) {
    // make sure all is merged before saving
    Def();
    // save child vectors separately
    for (int ChildN = 0; ChildN < ChildInfoV.Len(); ChildN++) {
        if (ChildInfoV[ChildN].DirtyP && ChildInfoV[ChildN].LoadedP) {
            ChildInfoV[ChildN].Pt = Gix->StoreChildVector(ChildInfoV[ChildN].Pt, ChildV[ChildN]);
            ChildInfoV[ChildN].DirtyP = false;
        }
    }

    // save item key and set
    ItemSetKey.Save(SOut);
    //ItemV.SaveMemCpy(SOut);
    ItemV.Save(SOut);
    ChildInfoV.Save(SOut);
    DirtyP = false;
}

template <class TKey, class TItem>
uint64 TGixItemSet<TKey, TItem>::GetMemUsed() const {
    return sizeof(TGixItemSet) +
        TMemUtils::GetExtraMemberSize(CRef) +
        TMemUtils::GetExtraMemberSize(ItemSetKey) +
        TMemUtils::GetExtraMemberSize(ItemV) +
        TMemUtils::GetExtraMemberSize(ItemVDel) +
        TMemUtils::GetExtraMemberSize(TotalCnt) +
        TMemUtils::GetExtraMemberSize(ChildInfoV) +
        TMemUtils::GetExtraMemberSize(ChildV) +
        TMemUtils::GetExtraMemberSize(MergedP) +
        TMemUtils::GetExtraMemberSize(DirtyP);
}

template <class TKey, class TItem>
void TGixItemSet<TKey, TItem>::OnDelFromCache(const TBlobPt& BlobPt, void* Gix) {
    // read-only gixes dirty itemsets through content-preserving merges (Def()) only,
    // so their in-memory changes can be discarded; the same holds for any gix in
    // discard-dirty-on-drop mode (its blobs were flushed current first)
    if (!((TGix<TKey, TItem>*)Gix)->IsReadOnly() &&
        !((TGix<TKey, TItem>*)Gix)->IsDiscardDirtyOnDrop() && DirtyP) {
        ((TGix<TKey, TItem>*)Gix)->StoreItemSet(BlobPt);
    }
}

template <class TKey, class TItem>
void TGixItemSet<TKey, TItem>::AddItem(const TItem& NewItem, const bool& NotifyCacheOnlyDelta) {
    // if NotifyCacheOnlyDelta is false we have just added a new itemset and we have to report to gix
    // the base size used by the empty itemset itself
    if (NotifyCacheOnlyDelta == false) {
        Gix->AddToNewCacheSizeInc(GetMemUsed());
    }

    if (IsFull()) {
        // if we will do a cleanup of data we need to update the cache size used
        const uint64 OldSize = GetMemUsed();
        Def();
        if (IsFull()) {
            PushWorkBufferToChildren();
        }
        // TODO: why is next RecalcTotalCnt needed? Def() already calls it if anything is changed.
        // It might be needed only if IsFull() was true.
        RecalcTotalCnt(); // work buffer might have been merged
        Gix->AddToNewCacheSizeInc(OldSize, GetMemUsed());
    }

    if (MergedP) {
        // if itemset is merged and the newly added item is bigger than the last one
        // the itemset remains merged
        if (ItemV.Len() == 0 && ChildInfoV.Len() == 0) {
            // the first item in whole itemset
            MergedP = true;
        } else if (ItemV.Len() == 0 && ChildInfoV.Len() != 0) {
            // compare with the last item of the last child
            MergedP = Gix->GetItemHandler()->IsLt(ChildInfoV.Last().MaxItem, NewItem);
        } else {
            // compare to the last item in the work buffer
            MergedP = Gix->GetItemHandler()->IsLt(ItemV.Last(), NewItem);
        }
    }
    const uint64 OldItemVSize = ItemV.GetMemUsed();
    // if first item we are adding to the itemset, we start with size 2 to avoid default of 16
    if (ItemV.Len() == 0) { ItemV.Reserve(2); }
    // add item to the end of the list
    ItemV.Add(NewItem);
    // Update the cache size (for the newly added item). In general we could just add
    // sizeof(TItem) to cache size, however we would underestimate the used size since
    // the arrays allocate extra buffer
    Gix->AddToNewCacheSizeInc(OldItemVSize, ItemV.GetMemUsed());

    DirtyP = true;
    TotalCnt++;
}

template <class TKey, class TItem>
void TGixItemSet<TKey, TItem>::AddItemV(const TVec<TItem>& NewItemV) {
    for (int i = 0; i < NewItemV.Len(); i++) {
        AddItem(NewItemV[i]);
    }
}

template <class TKey, class TItem>
const TItem& TGixItemSet<TKey, TItem>::GetItem(const int& ItemN) const {
    AssertR(ItemN >= 0 && ItemN < TotalCnt, TStr() + "Index: " + TInt::GetStr(ItemN) + ", TotalCnt: " + TInt::GetStr(TotalCnt));
    int Offset = ItemN;
    for (int ChildN = 0; ChildN < ChildInfoV.Len(); ChildN++) {
        if (Offset < ChildInfoV[ChildN].Len) {
            // load child vector only if needed
            LoadChildVector(ChildN);
            return ChildV[ChildN][Offset];
        }
        Offset -= ChildInfoV[ChildN].Len;
    }
    return ItemV[Offset];
}

template <class TKey, class TItem>
void TGixItemSet<TKey, TItem>::GetItemV(TVec<TItem>& _ItemV) {
    // reserve place for all the elements
    _ItemV.Gen(TotalCnt, 0);
    // load items
    if (ChildInfoV.Len() > 0) {
        // collect data from child itemsets
        LoadChildVectors();
        for (int i = 0; i < ChildInfoV.Len(); i++) {
            _ItemV.AddV(ChildV[i]);
        }
    }
    _ItemV.AddV(ItemV);
}

template <class TKey, class TItem>
void TGixItemSet<TKey, TItem>::GetItemVInRange(const TItem& MinItem, const TItem& MaxItem, TVec<TItem>& _ItemV) {
    // collect items only from child vectors whose stored [MinItem, MaxItem] range overlaps the
    // half-open query range [MinItem, MaxItem). Children fully outside the range are not loaded.
    if (ChildInfoV.Len() > 0) {
        for (int i = 0; i < ChildInfoV.Len(); i++) {
            // skip children that lie entirely below the range (their largest item is < MinItem)
            if (ChildInfoV[i].MaxItem < MinItem) { continue; }
            // skip children that lie entirely at or above the (exclusive) upper bound
            if (!(ChildInfoV[i].MinItem < MaxItem)) { continue; }
            LoadChildVector(i);
            _ItemV.AddV(ChildV[i]);
        }
    }
    // the working buffer is small (bounded by the split length) and may hold recently added items
    // with arbitrary values, so always include it
    _ItemV.AddV(ItemV);
}

template <class TKey, class TItem>
template <typename THandler>
void TGixItemSet<TKey, TItem>::GetItemV(THandler& Handler) {
    if (ChildInfoV.Len() > 0) {
        // collect data from child itemsets
        LoadChildVectors();
        for (int i = 0; i < ChildInfoV.Len(); i++) {
            Handler(ChildV[i]);
        }
    }
    Handler(ItemV);
}

template <class TKey, class TItem>
void TGixItemSet<TKey, TItem>::DelItem(const TItem& Item) {
    if (IsFull()) {
        const uint64 OldSize = GetMemUsed();
        Def();
        if (IsFull()) {
            PushWorkBufferToChildren();
        }
        RecalcTotalCnt(); // work buffer might have been merged
        Gix->AddToNewCacheSizeInc(OldSize, GetMemUsed());
    }

    const uint64 OldSize = ItemVDel.GetMemUsed() + ItemV.GetMemUsed();
    ItemVDel.Add(ItemV.Len());
    ItemV.Add(Item);
    const uint64 NewSize = ItemVDel.GetMemUsed() + ItemV.GetMemUsed();
    Gix->AddToNewCacheSizeInc(OldSize,  NewSize);
    MergedP = false;
    DirtyP = true;
    TotalCnt++;
}

template <class TKey, class TItem>
void TGixItemSet<TKey, TItem>::DelItemV(const TVec<TItem>& DelV) {
    if (DelV.Empty()) { return; }
    if (IsFull()) {
        const uint64 OldSize = GetMemUsed();
        Def();
        if (IsFull()) {
            PushWorkBufferToChildren();
        }
        RecalcTotalCnt(); // work buffer might have been merged
        Gix->AddToNewCacheSizeInc(OldSize, GetMemUsed());
    }

    // append every delete marker before any further flushing - the work buffer may temporarily
    // exceed SplitLen, but in exchange the whole batch is drained by ONE linear ProcessDeletes
    // pass on the next Def(). Calling DelItem per item instead would trigger a Def() for every
    // single item once the buffer is at SplitLen, i.e. O(SplitLen) work per deleted item.
    const uint64 OldSize = ItemVDel.GetMemUsed() + ItemV.GetMemUsed();
    for (int ItemN = 0; ItemN < DelV.Len(); ItemN++) {
        ItemVDel.Add(ItemV.Len());
        ItemV.Add(DelV[ItemN]);
    }
    const uint64 NewSize = ItemVDel.GetMemUsed() + ItemV.GetMemUsed();
    Gix->AddToNewCacheSizeInc(OldSize, NewSize);
    MergedP = false;
    DirtyP = true;
    TotalCnt += DelV.Len();
}

template <class TKey, class TItem>
uint64 TGixItemSet<TKey, TItem>::DelItemsBelow(const TItem& MinKeepItem) {
    // the sorted-prefix reasoning below only holds when the itemset is merged (children hold
    // disjoint ascending ranges, work buffer sorted and deduplicated) and no delete markers are
    // pending. Itemsets freshly loaded from disk are in this state; anything else falls back to
    // the regular read-and-filter delete path
    if (!MergedP || !ItemVDel.Empty()) { return 0; }
    const uint64 OldSize = GetMemUsed();
    uint64 Removed = 0;
    // children are disjoint and ascending, so the ones lying entirely below the threshold form
    // a prefix - drop them from the header alone, without ever loading their data. Deletes
    // never update the stored child stats, so a child's MaxItem can read stale-high (its true
    // items all below the threshold): cutting such a child empties it, and the child behind it
    // may be below the threshold as well - repeat the pass until the first child truly starts
    // at or above the threshold
    while (true) {
        int DropChildren = 0;
        while (DropChildren < ChildInfoV.Len() && ChildInfoV[DropChildren].MaxItem < MinKeepItem) {
            Removed += (uint64)(int)ChildInfoV[DropChildren].Len;
            Gix->DeleteChildVector(ChildInfoV[DropChildren].Pt);
            DropChildren++;
        }
        if (DropChildren > 0) {
            ChildInfoV.Del(0, DropChildren - 1);
            ChildV.Del(0, DropChildren - 1);
        }
        // the (now first) child may straddle the threshold - load it and cut its
        // below-threshold prefix. Only straddling (or stale-stat) children are ever read, and
        // a subsequent range query over the deleted ids would have loaded them anyway
        if (ChildInfoV.Len() == 0 || !(ChildInfoV[0].MinItem < MinKeepItem)) { break; }
        LoadChildVector(0);
        TVec<TItem>& Child = ChildV[0];
        int Cut = 0;
        while (Cut < Child.Len() && Child[Cut] < MinKeepItem) { Cut++; }
        // stats are outer bounds, so a stale-low MinItem can promise items that are not there
        if (Cut == 0) { break; }
        Child.Del(0, Cut - 1);
        ChildInfoV[0].Len = Child.Len();
        ChildInfoV[0].DirtyP = true;
        Removed += (uint64)Cut;
        if (!Child.Empty()) {
            ChildInfoV[0].MinItem = Child[0];
            break;	// the first child now starts at/above the threshold - prefix fully cut
        }
        // the stored MaxItem stat was stale-high and the whole child was below the
        // threshold - drop the now-empty child and re-examine the children behind it
        Gix->DeleteChildVector(ChildInfoV[0].Pt);
        ChildInfoV.Del(0);
        ChildV.Del(0);
    }
    // the work buffer is sorted when merged, so its sub-threshold items form a prefix as well
    int DropItems = 0;
    while (DropItems < ItemV.Len() && ItemV[DropItems] < MinKeepItem) { DropItems++; }
    if (DropItems > 0) {
        ItemV.Del(0, DropItems - 1);
        Removed += (uint64)DropItems;
    }
    if (Removed > 0) {
        TotalCnt -= (int)Removed;
        DirtyP = true;
        Gix->AddToNewCacheSizeInc(OldSize, GetMemUsed());
    }
    return Removed;
}

template <class TKey, class TItem>
void TGixItemSet<TKey, TItem>::Clr() {
    const int OldSize = GetMemUsed();
    if (ChildInfoV.Len() > 0) {
        for (int i = 0; i < ChildInfoV.Len(); i++) {
            Gix->DeleteChildVector(ChildInfoV[i].Pt);
        }
        ChildV.Clr();
        ChildInfoV.Clr();
    }
    ItemV.Clr();
    ItemVDel.Clr();
    MergedP = true;
    DirtyP = true;
    TotalCnt = 0;
    Gix->AddToNewCacheSizeInc(OldSize, GetMemUsed());
}

template <class TKey, class TItem>
void TGixItemSet<TKey, TItem>::Def() {
    // call merger to pack items, if not merged yet
    if (!MergedP) {
        ProcessDeletes(); // "execute" deletes, possibly leaving some child vectors too short
        Gix->GetItemHandler()->Merge(ItemV, true); // first do local merge of work-buffer
        DirtyP = true;
        InjectWorkBufferToChildren(); // inject data into child vectors

        int FirstChildToMerge = GetFirstChildToMerge();
        if (FirstChildToMerge >= 0 && ItemV.Empty() && !HasOversizedChild()) {
            // deletes-only flush: the work buffer is drained and children only shrank, so they
            // are still sorted and mutually disjoint - the itemset is already globally merged.
            // The global merge below would collect and rewrite the ENTIRE posting list from
            // FirstChildToMerge onward just because deletes left some children undersized; on a
            // huge key (per-key splitLen 100k, tens of millions of items) that is gigabytes of
            // memcpy on every work-buffer flush, which is what made batch deletes crawl even
            // after ProcessDeletes itself was made linear. Fix the fragmentation locally instead.
            CoalesceUndersizedChildren();
            FirstChildToMerge = -1;
        }
        // merge only when children actually need rebalancing. A non-empty work buffer is NOT a
        // reason: InjectWorkBufferToChildren just moved every item overlapping the children into
        // its child, so whatever remains in ItemV lies strictly beyond the last child's MaxItem
        // and the itemset is already globally merged (the same state DefLocal accepts). The old
        // additional `ItemV.Len() > 0` trigger re-merged the whole work buffer on every Def -
        // with per-item deletes on a mature key (work buffer sitting just under SplitLen) that
        // meant O(SplitLen) work for every deleted item, which the profile of a 600k-article
        // batch delete showed as 88% of all CPU (TQmGixItemPos construction under Def/AddV/Merge
        // on GixPos word keys).
        if (FirstChildToMerge >= 0) {
            // collect all data from subsequent child vectors and work-buffer
            TVec<TItem> MergedItems;
            for (int i = FirstChildToMerge; i < ChildInfoV.Len(); i++) {
                LoadChildVector(i);
                MergedItems.AddV(ChildV[i]);
            }
            MergedItems.AddV(ItemV);
            Gix->GetItemHandler()->Merge(MergedItems, false); // perform global merge

            PushMergedDataBackToChildren(FirstChildToMerge, MergedItems); // now save them back
        }
        // if the work buffer holds SplitLen or more (post-inject it lies entirely beyond the last
        // child), split it off into children - self-guarding, no-op for a smaller buffer
        PushWorkBufferToChildren();

        // in case deletes emptied the first children completely, remove them
        while (ChildInfoV.Len() > 0 && ChildInfoV[0].Len == 0) {
            // remove them first from BLOB storage
            Gix->DeleteChildVector(ChildInfoV[0].Pt);
            // remove it from memory
            ChildInfoV.Del(0);
            ChildV.Del(0);
        }

        RecalcTotalCnt();
        MergedP = true;
    }
}

template <class TKey, class TItem>
void TGixItemSet<TKey, TItem>::DefLocal() {
    // call merger to pack items in work buffer, if not merged yet
    if (!MergedP) {
        if (ItemVDel.Len() == 0) { // deletes are not treated as local - merger would get confused
            const int OldItemVLen = ItemV.Len();
            Gix->GetItemHandler()->Merge(ItemV, true); // perform local merge
            DirtyP = true;
            if (ChildInfoV.Len() > 0 && ItemV.Len() > 0) {
                if (Gix->GetItemHandler()->IsLt(ChildInfoV.Last().MaxItem, ItemV[0])) {
                    MergedP = true; // local merge achieved global merge
                }
            } else {
                MergedP = true;
            }
            // update the total count - since we have only been modifying the ItemV we can simply
            // update the total count by comparing previous and current number of items in ItemV
            TotalCnt = TotalCnt - OldItemVLen + ItemV.Len();
        }
    }
}

template <class TKey, class TItem>
double TGixItemSet<TKey, TItem>::GetLoadedPerc() const {
    int LoadedCount = 0;
    for (int ChildN = 0; ChildN < ChildInfoV.Len(); ChildN++) {
        if (ChildInfoV[ChildN].LoadedP) { LoadedCount++; }
    }
    return ChildInfoV.Empty() ? 1.0 : (double)LoadedCount / (double)ChildInfoV.Len();
}

#ifdef XTEST
template <class TKey, class TItem>
void TGixItemSet<TKey, TItem>::Print() const {
    LoadChildVectors();
    printf("TotalCnt=%d\n", TotalCnt);
    printf("len=%d\n", ItemV.Len());
    for (int i = 0; i < ItemV.Len(); i++) {
        printf("   %d=%d\n", i, ItemV[i]);
    }
    if (ItemVDel.Len()> 0) {
        printf("deleted=%d\n", ItemVDel.Len());
        for (int i = 0; i < ItemVDel.Len(); i++) {
            printf("   %d=%d\n", i, ItemVDel[i]);
        }
    }
    for (int j = 0; j < ChildInfoV.Len(); j++) {
        printf("   *** child %d\n", j);
        printf("   *** len %d\n", ChildInfoV[j].Len);
        printf("   *** mem-len %d\n", ChildV[j].Len());
        for (int i = 0; i < ChildV[j].Len(); i++) {
            printf("      %d=%d\n", i, ChildV[j][i]);
        }
    }
}
#endif

/////////////////////////////////////////////////
// General-Inverted-Index
template <class TKey, class TItem>
void TGix<TKey, TItem>::AssertReadOnly() const {
    EAssertR(((Access == faCreate) || (Access == faUpdate)), "Index opened in Read-Only mode!");
}

template <class TKey, class TItem>
TBlobPt TGix<TKey, TItem>::AddKeyId(const TKey& Key) {
    if (IsKey(Key)) { return KeyIdH.GetDat(Key); }
    // we don't have this key, create an empty item set and return pointer to it
    AssertReadOnly(); // check if we are allowed to write
    PGixItemSet ItemSet = TGixItemSet<TKey, TItem>::New(Key, &ItemHandler, this);
    TBlobPt KeyId = EnlistItemSet(ItemSet);
    KeyIdH.AddDat(Key, KeyId); // remember the new key and its Id
    KeyIdHDirtyP = true;
    return KeyId;
}

template <class TKey, class TItem>
TBlobPt TGix<TKey, TItem>::GetKeyId(const TKey& Key) const {
    if (IsKey(Key)) { return KeyIdH.GetDat(Key); }
    // we don't have this key, return empty pointer
    return TBlobPt();
}

template <class TKey, class TItem>
void TGix<TKey, TItem>::GetChildVector(const TBlobPt& KeyId, TVec<TItem>& Dest) const {
    if (KeyId.Empty()) { return; }
    PSIn ItemSetSIn = ItemSetBlobBs->GetBlob(KeyId);
    Dest.Load(*ItemSetSIn);
}

template <class TKey, class TItem>
TBlobPt TGix<TKey, TItem>::StoreChildVector(const TBlobPt& ExistingKeyId, const TVec<TItem>& Data) const {
    // check if we are allowed to write
    AssertReadOnly();
    // store the current version to the blob
    TMOut MOut;
    //Data.SaveMemCpy(MOut);
    Data.Save(MOut);
    int ReleasedSize;
    return ItemSetBlobBs->PutBlob(ExistingKeyId, MOut.GetSIn(), ReleasedSize);
}

template <class TKey, class TItem>
void TGix<TKey, TItem>::DeleteChildVector(const TBlobPt& KeyId) const {
    AssertReadOnly(); // check if we are allowed to write
    ItemSetBlobBs->DelBlob(KeyId);    // free space in BLOB
}

template <class TKey, class TItem>
TBlobPt TGix<TKey, TItem>::EnlistChildVector(const TVec<TItem>& Data) const {
    AssertReadOnly(); // check if we are allowed to write
    TMOut MOut;
    Data.Save(MOut);
    TBlobPt res = ItemSetBlobBs->PutBlob(MOut.GetSIn());
    return res;
}

template <class TKey, class TItem>
void TGix<TKey, TItem>::RefreshStats() const {
    Stats.CacheAll = 0;
    Stats.CacheDirty = 0;
    Stats.CacheAllLoadedPerc = 0;
    Stats.CacheDirtyLoadedPerc = 0;
    Stats.AvgLen = 0;

    Stats.MemUsed = this->GetMemUsed();
    TBlobPt BlobPt; PGixItemSet ItemSet;
    void* KeyDatP = ItemSetCache.FFirstKeyDat();
    while (ItemSetCache.FNextKeyDat(KeyDatP, BlobPt, ItemSet)) {
        Stats.CacheAll++;
        const double LoadedPerc = ItemSet->GetLoadedPerc();
        Stats.CacheAllLoadedPerc += LoadedPerc;
        Stats.AvgLen += ItemSet->GetItems();
        if (ItemSet->IsDirty()) {
            Stats.CacheDirty++;
            Stats.CacheDirtyLoadedPerc += LoadedPerc;
        }
    }
    if (Stats.CacheAll > 0) {
        Stats.CacheAllLoadedPerc /= Stats.CacheAll;
        Stats.AvgLen /= Stats.CacheAll;
        if (Stats.CacheDirty > 0) {
            Stats.CacheDirtyLoadedPerc /= Stats.CacheDirty;
        }
    }
}

template <class TKey, class TItem>
TGix<TKey, TItem>::TGix(const TStr& Nm, const TStr& FPath, const TFAccess& _Access,
    const TGixItemHandler<TKey, TItem>* _ItemHandler, const int64& CacheSize, const int _SplitLen,
    const bool _FirstChildBeUnfilledP, const int _SplitLenMin, const int _SplitLenMax,
    const TGixKeyDictType _KeyDictType) :
        Access(_Access), KeyIdH(_KeyDictType), KeyIdHDirtyP(false), ItemHandler(_ItemHandler),
        ItemSetCache(CacheSize, 1000000, GetVoidThis()),
        DiscardDirtyOnDropP(false),
        SplitLen(_SplitLen), SplitLenMin(_SplitLenMin), SplitLenMax(_SplitLenMax),
        FirstChildBeUnfilledP(_FirstChildBeUnfilledP), SplitLenProvider(NULL) {

    // prepare filenames of the GIX datastore
    GixFNm = TStr::GetNrFPath(FPath) + Nm.GetFBase() + ".Gix";
    GixBlobFNm = TStr::GetNrFPath(FPath) + Nm.GetFBase() + ".GixDat";
    // check in what mode should we open
    if (Access == faCreate) {
        // creating a new Gix; the key dictionary keeps the requested representation
        ItemSetBlobBs = TMBlobBs::New(GixBlobFNm, faCreate);
    } else {
        // loading an old Gix and getting it ready for search and update
        EAssert((Access == faUpdate) || (Access == faRdOnly) || (Access == faRestore));
        // load Gix from GixFNm; the file itself dictates the key dictionary
        // representation, the _KeyDictType parameter is only used for faCreate
        KeyIdH.LoadFile(GixFNm);
        // load ItemSets from GixBlobFNm
        ItemSetBlobBs = TMBlobBs::New(GixBlobFNm, Access);
    }
    // we do recounting after 10% change of the cache size
    CacheResetThreshold = int64(0.1 * double(CacheSize));
    NewCacheSizeInc = 0;
    CacheFullP = false;
}

template <class TKey, class TItem>
TGix<TKey, TItem>::~TGix() {
    if ((Access == faCreate) || (Access == faUpdate)) {
        // flush all the latest changes in cache to the disk
        // (storing a dirty itemset updates KeyIdH and sets KeyIdHDirtyP)
        ItemSetCache.Flush();
        // save the key dictionary to GixFNm; skipped in update mode when no key
        // was added/moved/removed - rewriting the unchanged (potentially huge)
        // dictionary dominated shutdown time on large read-mostly indexes
        if ((Access == faCreate) || KeyIdHDirtyP) {
            KeyIdH.SaveFile(GixFNm);
        }
    }
}

template <class TKey, class TItem>
TPt<TGixItemSet<TKey, TItem> > TGix<TKey, TItem>::GetItemSet(const TKey& Key) const {
    // reads grow the cache as well (itemsets and their child vectors get loaded),
    // so the size recomputation + purge has to be triggered from here too - with
    // write-only triggering a query-mostly process would grow far beyond the
    // configured cache size. It MUST run before the blob pointer is resolved:
    // the purge stores dirty itemsets, and a store can relocate an itemset's
    // blob (freeing the old one) and update KeyIdH - resolving first and purging
    // second read the itemset through a freed (possibly already reused) blob
    // pointer whenever the purge evicted the very key being fetched
    RefreshMemUsed();
    const TBlobPt KeyId = GetKeyId(Key);
    PGixItemSet ItemSet = GetItemSetNoRefresh(KeyId);
    // a stale pointer whose blob slot was reused delivers a well-formed itemset
    // of a DIFFERENT key - fail loudly instead of serving wrong postings
    EAssertR(KeyId.Empty() || ItemSet->GetKey() == Key,
        "TGix::GetItemSet: the itemset loaded from blob " + KeyId.GetAddrStr() +
        " belongs to a different key (stale blob pointer)");
    return ItemSet;
}

template <class TKey, class TItem>
TPt<TGixItemSet<TKey, TItem> > TGix<TKey, TItem>::GetItemSetNoRefresh(const TBlobPt& KeyId) const {
    if (KeyId.Empty()) {
        // return empty itemset
        return TGixItemSet<TKey, TItem>::New(TKey(), this);
    }
    PGixItemSet ItemSet;
    if (!ItemSetCache.Get(KeyId, ItemSet)) {
        // have to load it from the hard drive...
        PSIn ItemSetSIn = ItemSetBlobBs->GetBlob(KeyId);
        ItemSet = TGixItemSet<TKey, TItem>::Load(*ItemSetSIn, this);
        // account the freshly loaded itemset as cache growth (TCache::Put adds it
        // to its running total, but that total is only trusted between refreshes)
        AddToNewCacheSizeInc(ItemSet->GetMemUsed());
    }
    // bring the itemset to the top of the cache
    ItemSetCache.Put(KeyId, ItemSet);
    return ItemSet;
}

template <class TKey, class TItem>
void TGix<TKey, TItem>::GetItemV(const TKey& Key, TVec<TItem>& ItemV) const {
    PGixItemSet ItemSet = GetItemSet(Key);
    // first call Def() so that we can process some pending actions (like deletes) first
    ItemSet->Def();
    // get the items for the key
    return ItemSet->GetItemV(ItemV);
}

template <class TKey, class TItem>
void TGix<TKey, TItem>::GetItemVInRange(const TKey& Key, const TItem& MinItem, const TItem& MaxItem, TVec<TItem>& ItemV) const {
    PGixItemSet ItemSet = GetItemSet(Key);
    // first call Def() so that we can process some pending actions (like deletes) first
    ItemSet->Def();
    // get only the items that fall within the requested range
    ItemSet->GetItemVInRange(MinItem, MaxItem, ItemV);
}

template <class TKey, class TItem>
template <typename THandler>
void TGix<TKey, TItem>::GetItemV(const TKey& Key, THandler& Handler) const {
    PGixItemSet ItemSet = GetItemSet(Key);
    // first call Def() so that we can process some pending actions (like deletes) first
    ItemSet->Def();
    // get the items for the key
    return ItemSet->GetItemV(Handler);
}

template <class TKey, class TItem>
TBlobPt TGix<TKey, TItem>::StoreItemSet(const TBlobPt& KeyId) {
    AssertReadOnly(); // check if we are allowed to write
    // get the pointer to the item set
    PGixItemSet ItemSet;
    EAssert(ItemSetCache.Get(KeyId, ItemSet));
    ItemSet->Def();
    if (ItemSet->Empty()) {
        // itemset is empty after all deletes were processed => remove it
        ItemSetBlobBs->DelBlob(KeyId);
        KeyIdH.DelKey(ItemSet->GetKey());
        KeyIdHDirtyP = true;
        return TBlobPt(); // return NULL pointer
    } else {
        // store the current version to the blob
        TMOut MOut;
        ItemSet->Save(MOut);
        int ReleasedSize;
        TBlobPt NewKeyId = ItemSetBlobBs->PutBlob(KeyId, MOut.GetSIn(), ReleasedSize);
        // and update the KeyId in the key dictionary
        if (!(KeyIdH.GetDat(ItemSet->GetKey()) == NewKeyId)) {
            KeyIdH.AddDat(ItemSet->GetKey(), NewKeyId);
            KeyIdHDirtyP = true;
        }
        return NewKeyId;
    }
}

/// for deleting itemset from cache and blob
template <class TKey, class TItem>
void TGix<TKey, TItem>::DeleteItemSet(const TKey& Key) {
    AssertReadOnly(); // check if we are allowed to write
    if (IsKey(Key)) {
        TBlobPt Pt = KeyIdH.GetDat(Key);
        ItemSetCache.Del(Pt, false);
        ItemSetBlobBs->DelBlob(Pt);
        KeyIdH.DelKey(Key);
        KeyIdHDirtyP = true;
    }
}

template <class TKey, class TItem>
TBlobPt TGix<TKey, TItem>::EnlistItemSet(const PGixItemSet& ItemSet) const {
    AssertReadOnly(); // check if we are allowed to write
	// save the ItemsSet to MOut
    TMOut MOut;
    ItemSet->Save(MOut);
	// put the data into a blob
    TBlobPt Res = ItemSetBlobBs->PutBlob(MOut.GetSIn());
    return Res;
}

template <class TKey, class TItem>
void TGix<TKey, TItem>::AddItem(const TKey& Key, const TItem& Item) {
    AssertReadOnly(); // check if we are allowed to write
    if (IsKey(Key)) {
        // get the key handle
        TBlobPt KeyId = KeyIdH.GetDat(Key);
        // load the current item set
        PGixItemSet ItemSet = GetItemSet(Key);
        ItemSet->AddItem(Item);
    } else {
        // we don't have this key, create a new itemset and add new item immidiatelly
        PGixItemSet ItemSet = TGixItemSet<TKey, TItem>::New(Key, this);
        ItemSet->AddItem(Item, false);
        TBlobPt KeyId = EnlistItemSet(ItemSet); // now store this itemset to a blob
        KeyIdH.AddDat(Key, KeyId); // remember the new key and its Id
        KeyIdHDirtyP = true;
        ItemSetCache.Put(KeyId, ItemSet); // add it to cache
    }
    // check if we have to drop anything from the cache
    RefreshMemUsed();
}

template <class TKey, class TItem>
void TGix<TKey, TItem>::AddItemV(const TKey& Key, const TVec<TItem>& ItemV) {
    AssertReadOnly(); // check if we are allowed to write
    if (IsKey(Key)) {
        // get the key handle
        TBlobPt KeyId = KeyIdH.GetDat(Key);
        // load the current item set
        PGixItemSet ItemSet = GetItemSet(Key);
        ItemSet->AddItemV(ItemV);
    } else {
        // we don't have this key, create a new itemset and add new item immidiatelly
        PGixItemSet ItemSet = TGixItemSet<TKey, TItem>::New(Key, this);
        // report the base size of the new itemset to the cache growth counter -
        // AddItemV itself only reports the per-item deltas (same accounting as
        // AddItem's NotifyCacheOnlyDelta = false first add)
        AddToNewCacheSizeInc(ItemSet->GetMemUsed());
        ItemSet->AddItemV(ItemV);
        TBlobPt KeyId = EnlistItemSet(ItemSet); // now store this itemset to disk
        KeyIdH.AddDat(Key, KeyId); // remember the new key and its Id
        KeyIdHDirtyP = true;
        // keep it cached, as AddItem does - the very next AddItemV/GetItemSet for
        // this key would otherwise re-read the itemset from the blob
        ItemSetCache.Put(KeyId, ItemSet);
    }
    // check if we have to drop anything from the cache
    RefreshMemUsed();
}

template <class TKey, class TItem>
void TGix<TKey, TItem>::DelItem(const TKey& Key, const TItem& Item) {
    AssertReadOnly(); // check if we are allowed to write
    if (IsKey(Key)) { // check if this key exists
        // load the current item set
        PGixItemSet ItemSet = GetItemSet(Key);
        // clear the items from the ItemSet
        ItemSet->DelItem(Item);
        if (ItemSet->Empty()) {
            DeleteItemSet(Key);
        }
    }
}

template <class TKey, class TItem>
void TGix<TKey, TItem>::DelItemV(const TKey& Key, const TVec<TItem>& DelV) {
    AssertReadOnly(); // check if we are allowed to write
    if (IsKey(Key)) { // check if this key exists
        // load the current item set
        PGixItemSet ItemSet = GetItemSet(Key);
        // enqueue all deletes with a single flush
        ItemSet->DelItemV(DelV);
        if (ItemSet->Empty()) {
            DeleteItemSet(Key);
        }
    }
}

template <class TKey, class TItem>
uint64 TGix<TKey, TItem>::DelItemsBelow(const TKey& Key, const TItem& MinKeepItem) {
    AssertReadOnly(); // check if we are allowed to write
    if (!IsKey(Key)) { return 0; }
    PGixItemSet ItemSet = GetItemSet(Key);
    const uint64 Removed = ItemSet->DelItemsBelow(MinKeepItem);
    if (ItemSet->Empty()) {
        DeleteItemSet(Key);
    }
    return Removed;
}

template <class TKey, class TItem>
void TGix<TKey, TItem>::Clr(const TKey& Key) {
    AssertReadOnly(); // check if we are allowed to write
    if (IsKey(Key)) { // check if this key exists
        // load the current item set
        PGixItemSet ItemSet = GetItemSet(Key);
        // clear the items from the ItemSet
        ItemSet->Clr();
    }
}

template <class TKey, class TItem>
int TGix<TKey, TItem>::PartialFlush(int WndInMsec) {
    TBlobPt BlobPt;
    PGixItemSet ItemSet;
    THashSet<TBlobPt> BlobToDelH;
    int Changes = 0;
    void* KeyDatP;

    TTmStopWatch sw(true);

    KeyDatP = ItemSetCache.FLastKeyDat();
    while (ItemSetCache.FPrevKeyDat(KeyDatP, BlobPt, ItemSet)) {
        if (sw.GetMSecInt() > WndInMsec) break;
        if (ItemSet->IsDirty()) {
            TBlobPt NewBlobPt = StoreItemSet(BlobPt);
            if (NewBlobPt.Empty()) { // if itemset is empty, we get NULL pointer
                ItemSetCache.Del(BlobPt, false);
            }
            else {
                ItemSetCache.ChangeKey(BlobPt, NewBlobPt); // blob pointer might have changed, update cache
            }
            Changes++;
        }
    }
    return Changes;
}

template <class TKey, class TItem>
int64 TGix<TKey, TItem>::GetMemUsed() const {
    int64 res = sizeof(TCRef);
    res += sizeof(TFAccess);
    res += 2 * sizeof(int64);
    res += 3 * sizeof(int);
    res += sizeof(bool);
    res += sizeof(PBlobBs);
    res += ItemHandler->GetMemUsed();
    res += sizeof(TGixStats);
    res += GixFNm.GetMemUsed();
    res += GixBlobFNm.GetMemUsed();
    res += KeyIdH.GetMemUsed(true);
    res += ItemSetCache.GetMemUsed();
    return res;
}

template <class TKey, class TItem>
void TGix<TKey, TItem>::RefreshMemUsed() const {
    // check if we have to drop anything from the cache
    if (NewCacheSizeInc > CacheResetThreshold) {
        // start with \r and pad the line: the console may hold an in-place (\r) progress line -
        // e.g. the batch delete progress - which this overwrites cleanly (the pad erases any
        // longer leftover text). a bare leading \n would instead print a blank line whenever the
        // cursor already sits on an empty line
        printf("\r%-120s\n", TStr::Fmt("Gix cache clean-up start [accumulated growth: %s]",
            TUInt64::GetMegaStr(NewCacheSizeInc).CStr()).CStr());
        TExeTm ExeTm;
        // pack all the item sets
        TBlobPt BlobPt;
        PGixItemSet ItemSet;
        void* KeyDatP = ItemSetCache.FFirstKeyDat();
        while (ItemSetCache.FNextKeyDat(KeyDatP, BlobPt, ItemSet)) {
            ItemSet->DefLocal();
        }
        // clean-up cache
        CacheFullP = ItemSetCache.RefreshMemUsed();
        NewCacheSizeInc = 0;
        // GetCurMemUsed reuses the size just computed by RefreshMemUsed - calling
        // GetMemUsed here would re-walk the whole cache a second time
        printf("Gix cache clean-up done [new size: %s, took %s]\n",
            TUInt64::GetMegaStr(uint64(ItemSetCache.GetCurMemUsed())).CStr(), ExeTm.GetTmStr());
    }
}

template <class TKey, class TItem>
void TGix<TKey, TItem>::AddToNewCacheSizeInc(const uint64& OldSize, const uint64& NewSize) const {
    // no change
    if (NewSize == OldSize) {
        return;
    }
    // increased usage
    if (NewSize > OldSize) {
        NewCacheSizeInc += NewSize - OldSize;
    }
    // decreased usage
    else {
        if (NewCacheSizeInc >= OldSize - NewSize) {
            NewCacheSizeInc -= OldSize - NewSize;
        }
        // make sure we don't make an overflow
        else {
            NewCacheSizeInc = 0;
        }
    }
}

template <class TKey, class TItem>
void TGix<TKey, TItem>::DropFromCache(const TKey& Key) const {
    if (IsKey(Key)) {
        const TBlobPt KeyId = KeyIdH.GetDat(Key);
        PGixItemSet ItemSet;
        // only clean itemsets can be dropped without storing - discarding a dirty
        // itemset would lose all its changes that are not yet written to the blob.
        // In discard-dirty-on-drop mode dirty itemsets are dropped too: their only
        // changes are content-preserving merges over a flushed-current blob
        if (ItemSetCache.Get(KeyId, ItemSet) && (DiscardDirtyOnDropP || !ItemSet->IsDirty())) {
            ItemSetCache.Del(KeyId, false);
        }
    }
}

template <class TKey, class TItem>
void TGix<TKey, TItem>::StoreAndDropFromCache(const TKey& Key) {
    if (IsKey(Key)) {
        const TBlobPt KeyId = KeyIdH.GetDat(Key);
        // deleting with the event call routes a dirty itemset through
        // OnDelFromCache -> StoreItemSet, the same path the LRU purge uses;
        // a key that is not cached is a no-op
        ItemSetCache.Del(KeyId, true);
    }
}

template <class TKey, class TItem>
void TGix<TKey, TItem>::CopyTo(TGix<TKey, TItem>& DestGix, uint64* CopiedItemsOut, int* EmptyKeysOut,
        const TGixKeyFilter<TKey>* KeyFilter, TVec<TKey>* FailedKeyV) const {
    // collect and sort the keys, so that the data of all words belonging to the
    // same index key is also stored together in the destination
    TVec<TKey> KeyV;
    if (KeyFilter == NULL) {
        KeyIdH.GetKeyV(KeyV);
    } else {
        int KeyId = KeyIdH.FFirstKeyId();
        while (KeyIdH.FNextKeyId(KeyId)) {
            const TKey& Key = KeyIdH.GetKey(KeyId);
            if (KeyFilter->KeepKeyP(Key)) { KeyV.Add(Key); }
        }
    }
    KeyV.Sort();
    printf("Copying %s: %d keys\n", GixFNm.GetFMid().CStr(), KeyV.Len());
    uint64 TotalItems = 0;
    int EmptyKeys = 0;
    for (int KeyN = 0; KeyN < KeyV.Len(); KeyN++) {
        const TKey& Key = KeyV[KeyN];
        try {
            // load the itemset and stream its content (child vectors + work buffer) into the destination
            PGixItemSet ItemSet = GetItemSet(Key);
            ItemSet->Def();
            const int SrcItems = ItemSet->GetItems();
            if (SrcItems > 0) {
                TCopyToHandler Handler(DestGix, Key);
                ItemSet->GetItemV(Handler);
                TotalItems += (uint64) SrcItems;
                // validate that the destination received all the items
                const int DestItems = DestGix.GetItemSet(Key)->GetItems();
                EAssertR(DestItems == SrcItems, TStr::Fmt(
                    "TGix::CopyTo: item count mismatch for key %d of %d: %d in source, %d in destination",
                    KeyN, KeyV.Len(), SrcItems, DestItems));
                // the destination itemset is finished (keys are copied in sorted order
                // and never revisited) - flush it and evict it so the destination cache
                // holds only the key in flight instead of every key copied so far
                DestGix.StoreAndDropFromCache(Key);
            } else {
                // fully deleted posting list - the key is not created in the destination
                EmptyKeys++;
            }
        } catch (PExcept& Except) {
            // without a failure collector the first broken key aborts, as before
            if (FailedKeyV == NULL) { throw; }
            // record the key and keep scanning - the caller gets the complete list
            // of broken keys in one pass (it must not use the destination afterwards)
            FailedKeyV->Add(Key);
            printf("\nTGix::CopyTo: FAILED reading key %d of %d (blob %s): %s\n",
                KeyN, KeyV.Len(), GetKeyId(Key).GetAddrStr().CStr(), Except->GetMsgStr().CStr());
        }
        // release the source itemset so the full scan does not grow the cache without bound
        DropFromCache(Key);
        if (KeyN % 1000 == 0) {
            printf("%s / %s keys (%.1f%%), %s items copied\r", TStrUtil::GetStr(KeyN).CStr(), TStrUtil::GetStr(KeyV.Len()).CStr(),
                KeyV.Len() > 0 ? 100.0 * KeyN / KeyV.Len() : 100.0, TStrUtil::GetStr(TotalItems).CStr());
        }
    }
    printf("%s / %s keys (100.0%%), %s items copied, %s empty keys skipped\n",
        TStrUtil::GetStr(KeyV.Len()).CStr(), TStrUtil::GetStr(KeyV.Len()).CStr(), TStrUtil::GetStr(TotalItems).CStr(), TStrUtil::GetStr(EmptyKeys).CStr());
    if (FailedKeyV != NULL && !FailedKeyV->Empty()) {
        printf("TGix::CopyTo: %d key(s) FAILED - the destination is incomplete and must not be used\n", FailedKeyV->Len());
    }
    if (CopiedItemsOut != NULL) { *CopiedItemsOut = TotalItems; }
    if (EmptyKeysOut != NULL) { *EmptyKeysOut = EmptyKeys; }
}

template <class TKey, class TItem>
int TGix<TKey, TItem>::VerifyAllKeys(TStrV& FailedKeyStrV) const {
    // handler that only forces every child vector to be loaded from the blob
    struct TCountHandler {
        uint64 Items;
        TCountHandler(): Items(0) {}
        void operator()(const TVec<TItem>& ItemV) { Items += (uint64) ItemV.Len(); }
    };
    // collect (blob pointer, key) pairs and scan in blob-pointer order, so the
    // reads walk the blob files mostly sequentially instead of a random read
    // per key in dictionary order
    TVec<TPair<TBlobPt, TKey> > PtKeyV; PtKeyV.Gen(KeyIdH.Len(), 0);
    int KeyId = KeyIdH.FFirstKeyId();
    while (KeyIdH.FNextKeyId(KeyId)) {
        PtKeyV.Add(TPair<TBlobPt, TKey>(KeyIdH[KeyId], KeyIdH.GetKey(KeyId)));
    }
    PtKeyV.Sort();
    printf("Verifying %s: %d keys\n", GixFNm.GetFMid().CStr(), PtKeyV.Len());
    uint64 TotalItems = 0;
    for (int KeyN = 0; KeyN < PtKeyV.Len(); KeyN++) {
        const TKey& Key = PtKeyV[KeyN].Val2;
        try {
            // read the header blob and stream through every child vector; any
            // corrupt blob (of the itemset or a child) throws here
            PGixItemSet ItemSet = GetItemSet(Key);
            TCountHandler Handler;
            ItemSet->GetItemV(Handler);
            TotalItems += Handler.Items;
        } catch (PExcept& Except) {
            FailedKeyStrV.Add(TStr::Fmt("%s %s",
                PtKeyV[KeyN].Val1.GetAddrStr().CStr(), Except->GetMsgStr().CStr()));
            printf("\nTGix::VerifyAllKeys: FAILED key %d of %d (blob %s): %s\n",
                KeyN, PtKeyV.Len(), PtKeyV[KeyN].Val1.GetAddrStr().CStr(), Except->GetMsgStr().CStr());
        } catch (...) {
            FailedKeyStrV.Add(TStr::Fmt("%s unknown exception", PtKeyV[KeyN].Val1.GetAddrStr().CStr()));
            printf("\nTGix::VerifyAllKeys: FAILED key %d of %d (blob %s): unknown exception\n",
                KeyN, PtKeyV.Len(), PtKeyV[KeyN].Val1.GetAddrStr().CStr());
        }
        // release the itemset so the scan does not grow the cache without bound
        DropFromCache(Key);
        if (KeyN % 10000 == 0) {
            printf("%s / %s keys (%.1f%%), %s items read, %d failed\r",
                TStrUtil::GetStr(KeyN).CStr(), TStrUtil::GetStr(PtKeyV.Len()).CStr(),
                PtKeyV.Len() > 0 ? 100.0 * KeyN / PtKeyV.Len() : 100.0,
                TStrUtil::GetStr(TotalItems).CStr(), FailedKeyStrV.Len());
        }
    }
    printf("%s / %s keys (100.0%%), %s items read, %d failed             \n",
        TStrUtil::GetStr(PtKeyV.Len()).CStr(), TStrUtil::GetStr(PtKeyV.Len()).CStr(),
        TStrUtil::GetStr(TotalItems).CStr(), FailedKeyStrV.Len());
    return PtKeyV.Len();
}

template <class TKey, class TItem>
bool TGix<TKey, TItem>::IsKeyDataEqual(const TGix<TKey, TItem>& OtherGix, const TKey& Key, const int& MxItems) const {
    PGixItemSet ItemSet = GetItemSet(Key);
    PGixItemSet OtherItemSet = OtherGix.GetItemSet(Key);
    const int Items = ItemSet->GetItems();
    bool EqualP = (Items == OtherItemSet->GetItems());
    if (EqualP && Items > 0) {
        if (Items <= MxItems) {
            // compare complete item vectors
            TVec<TItem> ItemV; ItemSet->GetItemV(ItemV);
            TVec<TItem> OtherItemV; OtherItemSet->GetItemV(OtherItemV);
            EqualP = (ItemV == OtherItemV);
        } else {
            // too large to fully materialize twice - compare the boundary items
            EqualP = (ItemSet->GetItem(0) == OtherItemSet->GetItem(0)) &&
                (ItemSet->GetItem(Items - 1) == OtherItemSet->GetItem(Items - 1));
        }
    }
    // release both itemsets so verification does not grow the caches
    DropFromCache(Key);
    OtherGix.DropFromCache(Key);
    return EqualP;
}

template <class TKey, class TItem>
bool TGix<TKey, TItem>::VerifySample(const TGix<TKey, TItem>& OtherGix, const int& SampleKeys,
        const TGixKeyFilter<TKey>* KeyFilter) const {
    if (SampleKeys <= 0) { return true; }
    TVec<TKey> KeyV;
    if (KeyFilter == NULL) {
        KeyIdH.GetKeyV(KeyV);
    } else {
        int KeyId = KeyIdH.FFirstKeyId();
        while (KeyIdH.FNextKeyId(KeyId)) {
            const TKey& Key = KeyIdH.GetKey(KeyId);
            if (KeyFilter->KeepKeyP(Key)) { KeyV.Add(Key); }
        }
    }
    KeyV.Sort();
    const int Step = KeyV.Len() > SampleKeys ? KeyV.Len() / SampleKeys : 1;
    int Checked = 0, Failed = 0;
    for (int KeyN = 0; KeyN < KeyV.Len(); KeyN += Step) {
        if (!IsKeyDataEqual(OtherGix, KeyV[KeyN])) {
            printf("VerifySample: data mismatch for key %d of %d\n", KeyN, KeyV.Len());
            Failed++;
        }
        Checked++;
    }
    printf("VerifySample: %d keys checked, %d mismatches\n", Checked, Failed);
    return Failed == 0;
}

template <class TKey, class TItem>
void TGix<TKey, TItem>::SaveTxt(const TStr& FNm, const PGixKeyStr& KeyStr) const {
    TFOut FOut(FNm);
    // iterate over all the keys
    printf("Starting Gix SaveTxt\n");
    int KeyId = FFirstKeyId();
    int KeyN = 0; const int Keys = GetKeys();
    while (FNextKeyId(KeyId)) {
        if (KeyN % 1000 == 0) { printf("%d / %d\r", KeyN, Keys); } KeyN++;
        // get key and associated item set
        const TKey& Key = GetKey(KeyId);
        PGixItemSet ItemSet = GetItemSet(Key);
        // get statistics
        TStr KeyNm = KeyStr->GetKeyNm(Key);
        const int Items = ItemSet->GetItems();
        const uint64 MemUsed = ItemSet->GetMemUsed();
        // output statistics
        FOut.PutStrFmtLn("%s\t%d\t%d", KeyNm.CStr(), Items, MemUsed);
    }
    printf("Done: %d / %d\n", Keys, Keys);
}


/// print simple statistics for cache
template <class TKey, class TItem>
void TGix<TKey, TItem>::PrintStats() {
    RefreshStats();
    printf(".... gix cache stats - all=%d dirty=%d, loaded_perc=%f dirty_loaded_perc=%f, avg_len=%f, mem_used=%d \n",
        Stats.CacheAll, Stats.CacheDirty, Stats.CacheAllLoadedPerc, Stats.CacheDirtyLoadedPerc,
        Stats.AvgLen, Stats.MemUsed);
    const TBlobBsStats& blob_stats = ItemSetBlobBs->GetStats();
    printf(".... gix blob stats - puts=%u puts_new=%u gets=%u dels=%u size_chngs=%u avg_len_get=%f avg_len_put=%f avg_len_put_new=%f\n",
        blob_stats.Puts, blob_stats.PutsNew, blob_stats.Gets,
        blob_stats.Dels, blob_stats.SizeChngs, blob_stats.AvgGetLen, blob_stats.AvgPutLen, blob_stats.AvgPutNewLen);
    ItemSetBlobBs->ResetStats();
    printf(".... hash-table stats - memory=%s size=%d\n", TUInt64::GetKiloStr(KeyIdH.GetMemUsed()).CStr(), KeyIdH.Len());
    printf(".... gix - cnt=%s, memory=%s, hash=%s, cache=%s\n",
        TUInt64::GetMegaStr(KeyIdH.Len()).CStr(),
        TUInt64::GetMegaStr(GetMemUsed()).CStr(), TUInt64::GetMegaStr(KeyIdH.GetMemUsed()).CStr(), TUInt64::GetMegaStr(ItemSetCache.GetMemUsed()).CStr());
}

/////////////////////////////////////////////////
// General-Inverted-Index Expression-Item
template <class TKey, class TItem, class TResItem>
void TGixExpItem<TKey, TItem, TResItem>::PutAnd(const TPt<TGixExpItem<TKey, TItem, TResItem> >& _LeftExpItem,
    const TPt<TGixExpItem<TKey, TItem, TResItem> >& _RightExpItem) {

    ExpType = getAnd;
    LeftExpItem = _LeftExpItem;
    RightExpItem = _RightExpItem;
}

template <class TKey, class TItem, class TResItem>
void TGixExpItem<TKey, TItem, TResItem>::PutOr(const TPt<TGixExpItem<TKey, TItem, TResItem> >& _LeftExpItem,
    const TPt<TGixExpItem<TKey, TItem, TResItem> >& _RightExpItem) {

    ExpType = getOr;
    LeftExpItem = _LeftExpItem;
    RightExpItem = _RightExpItem;
}

template <class TKey, class TItem, class TResItem>
TPt<TGixExpItem<TKey, TItem, TResItem> > TGixExpItem<TKey, TItem, TResItem>::NewAndV(
    const TVec<TPt<TGixExpItem<TKey, TItem, TResItem> > >& ExpItemV) {

    // return empty item if no key is given
    if (ExpItemV.Empty()) { return TGixExpItem<TKey, TItem, TResItem>::NewEmpty(); }
    // otherwise we start with the first key
    TPt<TGixExpItem<TKey, TItem, TResItem> > TopExpItem = ExpItemV[0];
    // prepare a queue, which points to the next item (left) to be expanded to tree (and left right)
    TQQueue<TPt<TGixExpItem<TKey, TItem, TResItem> > > NextExpItemQ;
    // we start with the top
    NextExpItemQ.Push(TopExpItem);
    // add the rest of the items to the expresion tree
    for (int ExpItemN = 1; ExpItemN < ExpItemV.Len(); ExpItemN++) {
        const TPt<TGixExpItem<TKey, TItem, TResItem> >& RightExpItem = ExpItemV[ExpItemN];
        // which item should we expand
        TPt<TGixExpItem<TKey, TItem, TResItem> > ExpItem = NextExpItemQ.Top(); NextExpItemQ.Pop();
        // clone the item to be expanded
        TPt<TGixExpItem<TKey, TItem, TResItem> > LeftExpItem = ExpItem->Clone();
        // and make a new subtree
        ExpItem->PutAnd(LeftExpItem, RightExpItem);
        // update the queue
        NextExpItemQ.Push(ExpItem->LeftExpItem);
        NextExpItemQ.Push(ExpItem->RightExpItem);
    }
    return TopExpItem;
}

template <class TKey, class TItem, class TResItem>
TPt<TGixExpItem<TKey, TItem, TResItem> > TGixExpItem<TKey, TItem, TResItem>::NewOrV(
    const TVec<TPt<TGixExpItem<TKey, TItem, TResItem> > >& ExpItemV) {

    // return empty item if no key is given
    if (ExpItemV.Empty()) { return TGixExpItem<TKey, TItem, TResItem>::NewEmpty(); }
    // otherwise we start with the first key
    TPt<TGixExpItem<TKey, TItem, TResItem> > TopExpItem = ExpItemV[0];
    // prepare a queue, which points to the next item (left) to be expanded to tree (and left right)
    TQQueue<TPt<TGixExpItem<TKey, TItem, TResItem> > > NextExpItemQ;
    // we start with the top
    NextExpItemQ.Push(TopExpItem);
    // add the rest of the items to the expresion tree
    for (int ExpItemN = 1; ExpItemN < ExpItemV.Len(); ExpItemN++) {
        const TPt<TGixExpItem<TKey, TItem, TResItem> >& RightExpItem = ExpItemV[ExpItemN];
        // which item should we expand
        TPt<TGixExpItem<TKey, TItem, TResItem> > ExpItem = NextExpItemQ.Top(); NextExpItemQ.Pop();
        // clone the item to be expanded
        TPt<TGixExpItem<TKey, TItem, TResItem> > LeftExpItem = ExpItem->Clone();
        // and make a new subtree
        ExpItem->PutOr(LeftExpItem, RightExpItem);
        // update the queue
        NextExpItemQ.Push(ExpItem->LeftExpItem);
        NextExpItemQ.Push(ExpItem->RightExpItem);
    }
    return TopExpItem;
}

template <class TKey, class TItem, class TResItem>
TPt<TGixExpItem<TKey, TItem, TResItem> > TGixExpItem<TKey, TItem, TResItem>::NewAndV(const TVec<TKey>& KeyV) {
    TVec<TPt<TGixExpItem<TKey, TItem, TResItem> > > ExpItemV(KeyV.Len(), 0);
    for (int KeyN = 0; KeyN < KeyV.Len(); KeyN++) {
        ExpItemV.Add(TGixExpItem<TKey, TItem, TResItem>::NewItem(KeyV[KeyN]));
    }
    return NewAndV(ExpItemV);
}

template <class TKey, class TItem, class TResItem>
TPt<TGixExpItem<TKey, TItem, TResItem> > TGixExpItem<TKey, TItem, TResItem>::NewOrV(const TVec<TKey>& KeyV) {
    TVec<TPt<TGixExpItem<TKey, TItem, TResItem> > > ExpItemV(KeyV.Len(), 0);
    for (int KeyN = 0; KeyN < KeyV.Len(); KeyN++) {
        ExpItemV.Add(TGixExpItem<TKey, TItem, TResItem>::NewItem(KeyV[KeyN]));
    }
    return NewOrV(ExpItemV);
}

template <class TKey, class TItem, class TResItem>
bool TGixExpItem<TKey, TItem, TResItem>::Eval(const TPt<TGix<TKey, TItem> >& Gix,
    TVec<TResItem>& ResItemV, const TGixMerger<TKey, TItem, TResItem>* Merger) {

    // prepare place for result
    ResItemV.Clr();
    if (ExpType == getOr) {
        EAssert(!LeftExpItem.Empty() && !RightExpItem.Empty());
        TVec<TResItem> RightItemV;
        const bool NotLeft = LeftExpItem->Eval(Gix, ResItemV, Merger);
        const bool NotRight = RightExpItem->Eval(Gix, RightItemV, Merger);
        if (NotLeft && NotRight) {
            Merger->Intrs(ResItemV, RightItemV);
        } else if (!NotLeft && !NotRight) {
            Merger->Union(ResItemV, RightItemV);
        } else {
            TVec<TResItem> MinusItemV;
            if (NotLeft) {
                Merger->Minus(ResItemV, RightItemV, MinusItemV);
            } else {
                Merger->Minus(RightItemV, ResItemV, MinusItemV);
            }
            ResItemV = MinusItemV;
        }
        return (NotLeft || NotRight);
    } else if (ExpType == getAnd) {
        EAssert(!LeftExpItem.Empty() && !RightExpItem.Empty());
        TVec<TResItem> RightItemV;
        const bool NotLeft = LeftExpItem->Eval(Gix, ResItemV, Merger);
        const bool NotRight = RightExpItem->Eval(Gix, RightItemV, Merger);
        if (NotLeft && NotRight) {
            Merger->Union(ResItemV, RightItemV);
        } else if (!NotLeft && !NotRight) {
            Merger->Intrs(ResItemV, RightItemV);
        } else {
            TVec<TResItem> MinusItemV;
            if (NotLeft) {
                Merger->Minus(RightItemV, ResItemV, MinusItemV);
            } else {
                Merger->Minus(ResItemV, RightItemV, MinusItemV);
            }
            ResItemV = MinusItemV;
        }
        return (NotLeft && NotRight);
    } else if (ExpType == getKey) {
        PGixItemSet ItemSet = Gix->GetItemSet(Key);
        if (!ItemSet.Empty()) {
            ItemSet->Def();
            TVec<TItem> ItemV; ItemSet->GetItemV(ItemV);
            Merger->Def(ItemSet->GetKey(), ItemV, ResItemV);
        }
        return false;
    } else if (ExpType == getNot) {
        return !RightExpItem->Eval(Gix, ResItemV, Merger);
    } else if (ExpType == getEmpty) {
        return false; // return nothing
    }
    return true;
}
