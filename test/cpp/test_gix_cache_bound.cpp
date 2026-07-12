/* Copyright (C) Event Registry d.o.o. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Written by Gregor Leban <gregor@eventregistry.org>, 2013-2017
 */

#include <qminer.h>
#include <qminer_storage.h>
#include "gtest/gtest.h"

using namespace TQm;
using namespace TQm::TStorage;

// regression tests: the gix itemset cache must respect its configured size limit
// also under a READ-only workload. Reads grow the cache too (itemsets and their
// child vectors get loaded from disk), but historically only the write path
// (AddItem/AddItemV) triggered the size recomputation + purge, so a query-mostly
// process (e.g. a read-only ServerArticles) grew far beyond the configured
// index cache size.

namespace {

typedef TIntUInt64Pr TCacheGixKey;
typedef TUInt TCacheGixItem;
typedef TPt<TGix<TCacheGixKey, TCacheGixItem> > PCacheGix;

} // namespace

TEST(GixCacheBoundTests, ReadOnlyQueriesRespectCacheSize)
{
	const TStr FPath = "./gix_cachebound_test/";
	if (TDir::Exists(FPath)) { TDir::DelNonEmptyDir(FPath); }
	TDir::GenDir(FPath);

	TGixDefItemHandler<TCacheGixKey, TCacheGixItem> ItemHandler;

	const int Keys = 200;
	const int ItemsPerKey = 5000;   // ~20 KB payload per key -> ~4 MB total on disk

	// build a gix with plenty of cache so everything fits while writing
	{
		PCacheGix Gix = TGix<TCacheGixKey, TCacheGixItem>::New(
			"GixCacheBound", FPath, faCreate, &ItemHandler, int64(64) * int64(TInt::Mega), 1000, true, 500, 2000);
		for (int KeyN = 0; KeyN < Keys; KeyN++) {
			TVec<TCacheGixItem> ItemV;
			for (int ItemN = 0; ItemN < ItemsPerKey; ItemN++) { ItemV.Add(TUInt(ItemN)); }
			Gix->AddItemV(TCacheGixKey(KeyN, 1), ItemV);
		}
	}

	// reopen READ-ONLY with a small cache (1 MB) and touch every key like a
	// query workload would. the data is ~4x the cache size, so without purging
	// on the read path the cache grows to the full data size
	const int64 CacheSize = int64(1) * int64(TInt::Mega);
	PCacheGix Gix = TGix<TCacheGixKey, TCacheGixItem>::New(
		"GixCacheBound", FPath, faRdOnly, &ItemHandler, CacheSize, 1000, true, 500, 2000);

	uint64 MxSeen = 0;
	for (int KeyN = 0; KeyN < Keys; KeyN++) {
		TVec<TCacheGixItem> ItemV;
		Gix->GetItemV(TCacheGixKey(KeyN, 1), ItemV);
		ASSERT_EQ(ItemsPerKey, ItemV.Len());
		MxSeen = MAX(MxSeen, (uint64)Gix->GetCacheSize());
	}

	// the recompute triggers when the accumulated growth exceeds 10% of the cache
	// size and TCache::Put purges incrementally, so allow generous slack: the
	// cache must stay in the vicinity of its limit, not at the size of the data.
	// (pre-fix this reached ~4 MB = all data; the limit is 1 MB)
	EXPECT_LT(MxSeen, (uint64)(2 * CacheSize))
		<< "read-driven cache growth is not being purged (saw " << MxSeen << " bytes)";

	// and the queries must still return correct data after purging
	TVec<TCacheGixItem> ItemV;
	Gix->GetItemV(TCacheGixKey(0, 1), ItemV);
	ASSERT_EQ(ItemsPerKey, ItemV.Len());
	for (int ItemN = 0; ItemN < 100; ItemN++) {
		EXPECT_EQ((uint)ItemN, (uint)ItemV[ItemN]);
	}

	Gix = nullptr;
	TDir::DelNonEmptyDir(FPath);
}
