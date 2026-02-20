#include <base.h>
#include <qminer.h>
#include <qminer_storage.h>

#if defined(GLib_MACOSX)

#include <mach/mach.h>

void ReportMemory(TStr Msg = "") {
    mach_task_basic_info info;
    mach_msg_type_number_t infoCount = MACH_TASK_BASIC_INFO_COUNT;

    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &infoCount) != KERN_SUCCESS) {
        printf("Error getting task info\n");
    } else {
		printf("Memory[%s]:  size=%10.2fMB\n", Msg.CStr(), (double)info.resident_size / (double)TInt::Mega);
	}
}

#elif defined(GLib_GLIBC)

void ReportMemory(TStr Msg = "") {
	TSysMemStat MemStat;
	printf("Memory[%s]:  size=%10.2fMB\n", Msg.CStr(), (double)MemStat.Resident.Val / (double)TInt::Mega);
}

#endif

void ConvertArticleToTestJson() {
	printf("------ Converting articles to test JSON ------\n");
	TStrV ArticleFNmV;
	ArticleFNmV.Add("./data/2023-06-08 Articles.json");
	ArticleFNmV.Add("./data/2023-06-09 Articles.json");
	ArticleFNmV.Add("./data/2023-06-10 Articles.json");
	ArticleFNmV.Add("./data/2023-06-11 Articles.json");
	const TStr OutFNm = "./data/TestBody.json";

	for (const TStr& ArticleFNm : ArticleFNmV) {
		printf("Converting %s ...\n", ArticleFNm.CStr());
		TFIn FIn(ArticleFNm);
		TFOut FOut(OutFNm, true);
		TStr Line;
		while (FIn.GetNextLn(Line)) {
			PJsonVal ArticleJson = TJsonVal::GetValFromStr(Line);
			TStr BodyStr = ArticleJson->GetObjStr("Body");
			PJsonVal TestJson = TJsonVal::NewObj();
			TestJson->AddToObj("Text", BodyStr);
			FOut.PutStrLn(TestJson->SaveStr());
		}
	}
	printf("Done.\n");
}

void CreateTestStore() {
	printf("------ Creating test store ------\n");
	TQm::TEnv::InitLogger(1, "std", true);

	// load schema
    const TStr StoreJsonFNm = "./test.def";
	if (!TFile::Exists(StoreJsonFNm)) {
        TQm::TEnv::Logger->OnStatusFmt("could not locate the test.def file");
        return;
    }
    PJsonVal StoreDefVal = TJsonVal::GetValFromStr(TStr::LoadTxt(StoreJsonFNm));

	// create base
	const uint64 StoreCacheSize = 100 * TInt::Mega;
	const uint64 IndexCacheSize = 100 * TInt::Mega;
	TQm::PBase Base = TQm::TStorage::NewBase("./test_base/", StoreDefVal, IndexCacheSize, StoreCacheSize,
		true, TStrUInt64H(), TStrUInt64H(), false, 128, true);

	// load articles
	const TStr JsonFNm = "./data/TestBody.json";
	TQm::PStore TestStore = Base->GetStoreByStoreNm("Test");
	TQm::TEnv::Logger->OnStatusFmt("Loading %s json ...", JsonFNm.CStr());
	TFIn FIn(JsonFNm); TStr Line;
	uint64 LastTm = TTm::GetCurUniMSecs();
	while (FIn.GetNextLn(Line)) {
		PJsonVal Json = TJsonVal::GetValFromStr(Line);
		// add record
		const uint64 RecId = TestStore->AddRec(Json);
		// print status
		if (RecId % 10000 == 0) {
			double Diff = (TTm::GetCurUniMSecs() - LastTm) / 1000.0;
			TQm::TEnv::Logger->OnStatusFmt("%llu (%.0fs)", RecId, Diff);
			LastTm = TTm::GetCurUniMSecs();
		}
	}
	// update last time
	double Diff = (TTm::GetCurUniMSecs() - LastTm) / 1000.0;
	TQm::TEnv::Logger->OnStatusFmt("%llu (%.0fs)", TestStore->GetRecs(), Diff);

	// save base
	TQm::TStorage::SaveBase(Base);
}

void TestIndexCache() {
	printf("------ Testing index cache ------\n");
	TQm::TEnv::InitLogger(1, "std", true);

	const uint64 StoreCacheSize = 100 * TInt::Mega;
	const uint64 IndexCacheSize = 100 * TInt::Mega;
	TWPt<TQm::TBase> Base = TQm::TStorage::LoadBase("./test_base/", faRdOnly, IndexCacheSize, StoreCacheSize);
	ReportMemory("on_load");

	// prepare list of possible words
	TQm::PStore TestStore = Base->GetStoreByStoreNm("Test");
	TQm::PIndexVoc IndexVoc = Base->GetIndexVoc();
	const int KeyId = IndexVoc->GetKeyId(TestStore->GetStoreId(), "Text");
	EAssert(IndexVoc->IsWordVoc(KeyId));
	EAssert(IndexVoc->GetKey(KeyId).IsTextPos());
	TStrV WordStrV; IndexVoc->GetAllWordStrV(KeyId, WordStrV);
	printf("Text index has %d words\n", WordStrV.Len());
	// map words to ids
	TIntV WordIdV;
	for (const TStr& WordStr : WordStrV) {
		const int WordId = IndexVoc->GetWordId(KeyId, WordStr);
		WordIdV.Add(WordId);
	}

	printf("Sleeping for 10 seconds to note down memory in task manager ...\n");
	sleep(10);
	printf("Waking up ...\n");

	TRnd Rnd(1); uint64 Searches = 0, TotalRecords = 0;
	while (Searches < 100*(uint64)TInt::Mega) {
		// prepare single word query
		// const int RndWordId = WordIdV[123];
		const int RndWordId = WordIdV[Rnd.GetUniDevInt(WordIdV.Len())];
		const TStr RndWordStr = IndexVoc->GetWordStr(KeyId, RndWordId);
		const TQm::TQueryItem Item(Base, "Test", "Text", TUInt64V::GetV(RndWordId), TIntV());
		// execute query
		TQm::PRecSet RecSet = Base->Search(Item);
		TotalRecords += RecSet->GetRecs();
		// print status every 1000 searches
		if (Searches % 100000 == 0) {
			ReportMemory(TStr::Fmt("searches=%llu, records=%llu", Searches, TotalRecords));
			const TGixStats GixStats = Base->GetIndex()->GetGixStats(true);
			printf("Catch hit vs miss: %llu vs %llu  (%.2f hit)\n", GixStats.CacheHits.Val, GixStats.CacheMisses.Val,
				(double)GixStats.CacheHits.Val / (double)(GixStats.CacheHits.Val + GixStats.CacheMisses.Val));
			printf("Cache size: %sB, Avg[ItemV.Len()]=%.2f\n", TUInt64::GetMegaStr(GixStats.CacheMemUsed).CStr(), GixStats.AvgLen.Val);
		}
		Searches++;
	}
}

///////////////////////////////////////////////

void CreateArticleStore() {
	printf("------ Creating test store ------\n");
	TQm::TEnv::InitLogger(1, "std", true);

	// load schema
    const TStr StoreJsonFNm = "./news.def";
	if (!TFile::Exists(StoreJsonFNm)) {
        TQm::TEnv::Logger->OnStatusFmt("could not locate the news.def file");
        return;
    }
    PJsonVal StoreDefVal = TJsonVal::GetValFromStr(TStr::LoadTxt(StoreJsonFNm));

	// create base
	const uint64 StoreCacheSize = 100 * TInt::Mega;
	const uint64 IndexCacheSize = 100 * TInt::Mega;
	TQm::PBase Base = TQm::TStorage::NewBase("./test_base/", StoreDefVal, IndexCacheSize, StoreCacheSize,
		true, TStrUInt64H(), TStrUInt64H(), false, 128, true);

	// load articles
	TStrV ArticleFNmV; TStrSet UriSet;
	ArticleFNmV.Add("./data/2023-06-08 Articles.json");
	ArticleFNmV.Add("./data/2023-06-09 Articles.json");
	ArticleFNmV.Add("./data/2023-06-10 Articles.json");
	ArticleFNmV.Add("./data/2023-06-11 Articles.json");
	TQm::PStore ArticleStore = Base->GetStoreByStoreNm("Article");
	for (const TStr& ArticleFNm : ArticleFNmV) {
		TQm::TEnv::Logger->OnStatusFmt("Loading %s batch of articles ...", ArticleFNm.CStr());
		TFIn FIn(ArticleFNm);
		TStr Line;
		uint64 LastTm = TTm::GetCurUniMSecs();
		while (FIn.GetNextLn(Line)) {
			PJsonVal ArticleJson = TJsonVal::GetValFromStr(Line);
			// check if URI is in the set
			if (!UriSet.Empty()) {
				const TStr Uri = ArticleJson->GetObjStr("URI");
				if (!UriSet.IsKey(Uri)) { continue; }
			}
			// add record
			const uint64 ArticleId = ArticleStore->AddRec(ArticleJson);
			// print status
			if (ArticleId % 10000 == 0) {
				double Diff = (TTm::GetCurUniMSecs() - LastTm) / 1000.0;
				TQm::TEnv::Logger->OnStatusFmt("%llu (%.0fs)", ArticleId, Diff);
				LastTm = TTm::GetCurUniMSecs();
			}
		}
		// update last time
		double Diff = (TTm::GetCurUniMSecs() - LastTm) / 1000.0;
		TQm::TEnv::Logger->OnStatusFmt("%llu (%.0fs)", ArticleStore->GetRecs(), Diff);
	}

	// save base
	TQm::TStorage::SaveBase(Base);
}

void TestArticleStoreCache() {
	printf("------ Testing store cache ------\n");
	TQm::TEnv::InitLogger(1, "std", true);

	const uint64 StoreCacheSize = 10 * TInt::Mega;
	const uint64 IndexCacheSize = 10 * TInt::Mega;
	TQm::PBase Base = TQm::TStorage::LoadBase("./test_base/", faRdOnly, IndexCacheSize, StoreCacheSize);
	ReportMemory("on_load");

	TQm::PStore ArticleStore = Base->GetStoreByStoreNm("Article");
	TRnd Rnd(1); uint64 Recs = 0, DataSize = 0;
	while (Recs < 100*(uint64)TInt::Mega) {
		const uint64 RecId = Rnd.GetUniDevInt64(ArticleStore->GetRecs());
		// load cache fields
		const TStr Url = ArticleStore->GetFieldNmStr(RecId, "URI");
		const TStr Title = ArticleStore->GetFieldNmStr(RecId, "Title");
		const TStr Body = ArticleStore->GetFieldNmStr(RecId, "Body");
		const TStr Image = ArticleStore->GetFieldNmStr(RecId, "Image");
		const TStr Details = ArticleStore->GetFieldNmStr(RecId, "Details");
		const TStr ExtractedDates = ArticleStore->GetFieldNmStr(RecId, "ExtractedDates");
		const TStr Date = ArticleStore->GetFieldNmStr(RecId, "Date");
		const uchar Flags = ArticleStore->GetFieldNmByte(RecId, "Flags");
		Recs++;
		DataSize += Url.Len() + Title.Len() + Body.Len() + Image.Len() + Details.Len() + ExtractedDates.Len() + Date.Len() + sizeof(Flags);
		// print status every 1000 records
		if (Recs % 100000 == 0) {
			ReportMemory(TStr::Fmt("recs=%llu, data=%.2fMB", Recs, (double)DataSize / (double)TInt::Mega));
		}
	}
}

void TestArticleIndexCache() {
	printf("------ Testing index cache ------\n");
	TQm::TEnv::InitLogger(1, "std", true);

	const uint64 StoreCacheSize = 100 * TInt::Mega;
	const uint64 IndexCacheSize = 100 * TInt::Mega;
	TWPt<TQm::TBase> Base = TQm::TStorage::LoadBase("./test_base/", faRdOnly, IndexCacheSize, StoreCacheSize);
	ReportMemory("on_load");

	// prepare list of possible words
	TQm::PStore ArticleStore = Base->GetStoreByStoreNm("Article");
	TQm::PIndexVoc IndexVoc = Base->GetIndexVoc();
	const int BodyKeyId = IndexVoc->GetKeyId(ArticleStore->GetStoreId(), "Body");
	EAssert(IndexVoc->IsWordVoc(BodyKeyId));
	EAssert(IndexVoc->GetKey(BodyKeyId).IsTextPos());
	TStrV WordStrV; IndexVoc->GetAllWordStrV(BodyKeyId, WordStrV);
	printf("Body index has %d words\n", WordStrV.Len());
	// map words to ids
	TIntV WordIdV;
	for (const TStr& WordStr : WordStrV) {
		const int WordId = IndexVoc->GetWordId(BodyKeyId, WordStr);
		WordIdV.Add(WordId);
	}

	printf("Sleeping for 10 seconds to note down memory in task manager ...\n");
	sleep(10);
	printf("Waking up ...\n");

	TRnd Rnd(1); uint64 Searches = 0, TotalRecords = 0;
	while (Searches < 100*(uint64)TInt::Mega) {
		// prepare single word query
		// const int RndWordId = WordIdV[123];
		const int RndWordId = WordIdV[Rnd.GetUniDevInt(WordIdV.Len())];
		const TStr RndWordStr = IndexVoc->GetWordStr(BodyKeyId, RndWordId);
		const TQm::TQueryItem Item(Base, "Article", "Body", TUInt64V::GetV(RndWordId), TIntV());
		// execute query
		TQm::PRecSet RecSet = Base->Search(Item);
		TotalRecords += RecSet->GetRecs();
		// print status every 1000 searches
		if (Searches % 100000 == 0) {
			ReportMemory(TStr::Fmt("searches=%llu, records=%llu", Searches, TotalRecords));
		}
		Searches++;
	}
}

int main(int argc, char* argv[]) {
	TEnv Env(argc, argv, TNotify::StdNotify);
	TQm::TEnv::Init();
	TQm::TEnv::InitLogger(1, "std", true);

	// prepare unicode base
	if (!TUnicodeDef::IsDef()) {
		TUnicodeDef::Load("./UnicodeDef.Bin");
		if (!TUnicodeDef::IsDef()) {
			TNotify::StdNotify->OnNotify(ntInfo, "Unable to load ./UnicodeDef.Bin. Exiting...");
			return 1;
		}
	}

    try {
		if (Env.IsArgStr("--convert")) {
			printf("Converting articles to test JSON ...\n");
			ConvertArticleToTestJson();
		} else if (Env.IsArgStr("--create")) {
			printf("Creating test store ...\n");
			CreateTestStore();
			// CreateArticleStore();
		} else if (Env.IsArgStr("--store")) {
			printf("Testing store cache ...\n");
			// TestArticleStoreCache();
		} else if (Env.IsArgStr("--index")) {
			printf("Testing index cache ...\n");
			TestIndexCache();
			// TestArticleIndexCache();
		} else {
			printf("No arguments provided.\n");
		}
    } catch (PExcept E) {
        TNotify::StdNotify->OnStatus("Error: " + E->GetMsgStr());
        TNotify::StdNotify->OnStatus("Trace: " + E->GetLocStr());
    }
    catch (...) {
        printf("general exception");
    }

	printf("Done.\n");
}
