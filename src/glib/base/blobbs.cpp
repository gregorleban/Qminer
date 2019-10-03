/**
 * Copyright (c) 2015, Jozef Stefan Institute, Quintelligence d.o.o. and contributors
 * All rights reserved.
 *
 * This source code is licensed under the FreeBSD license found in the
 * LICENSE file in the root directory of this source tree.
 */

/////////////////////////////////////////////////
// Blob-Pointer
TStr TBlobPt::GetStr() const {
    TChA ChA;
    ChA+='[';
    if (Empty()) {
        ChA+="Null";
    } 
    else {
        ChA+=TUInt::GetStr(uint(Seg)); ChA+=':'; ChA+=TUInt::GetStr(Addr);
    }
    ChA+=']';
    return ChA;
}

/////////////////////////////////////////////////
// Statistics for TBlobBs
void TBlobBsStats::Reset() {
    AvgPutNewLen = AvgGetLen = AvgPutLen = 0;
    Dels = Puts = PutsNew = Gets = SizeChngs = 0;
    AllocUsedSize = AllocUnusedSize = AllocSize = AllocCount = ReleasedCount = ReleasedSize = 0;
}

TBlobBsStats TBlobBsStats::Clone() const {
    TBlobBsStats res;
    res.AvgGetLen = this->AvgGetLen;
    res.AvgPutLen = this->AvgPutLen;
    res.AvgPutNewLen = this->AvgPutNewLen;
    res.Dels = this->Dels;
    res.Gets = this->Gets;
    res.Puts = this->Puts;
    res.PutsNew = this->PutsNew;
    res.SizeChngs = this->SizeChngs;
    res.AllocUsedSize = this->AllocUsedSize;
    res.AllocUnusedSize = this->AllocUnusedSize;
    res.AllocSize = this->AllocSize;
    res.AllocCount = this->AllocCount;
    res.ReleasedCount = this->ReleasedCount;
    res.ReleasedSize = this->ReleasedSize;
    return res;
}

void TBlobBsStats::Add(const TBlobBsStats& Othr) {
    Puts += Othr.Puts;
    PutsNew += Othr.PutsNew;
    Gets += Othr.Gets;
    SizeChngs += Othr.SizeChngs;
    Dels += Othr.Dels;
    AllocUsedSize += Othr.AllocUsedSize;
    AllocUnusedSize += Othr.AllocUnusedSize;
    AllocSize += Othr.AllocSize;
    AllocCount += Othr.AllocCount;
    ReleasedCount += Othr.ReleasedCount;
    ReleasedSize += Othr.ReleasedSize;

    AvgPutNewLen = 0;
    AvgPutLen = 0;
    AvgGetLen = 0;

    if (PutsNew + Othr.PutsNew > 0) {
        AvgPutNewLen = (AvgPutNewLen*PutsNew + Othr.AvgPutNewLen*Othr.PutsNew) / (PutsNew + Othr.PutsNew);
    }
    if (Gets + Othr.Gets) {
        AvgGetLen = (AvgGetLen*Gets + Othr.AvgGetLen*Othr.Gets) / (Gets + Othr.Gets);
    }
    if (Puts + Othr.Puts > 0) {
        AvgPutLen = (AvgPutLen*Puts + Othr.AvgPutLen*Othr.Puts) / (Puts + Othr.Puts);
    }
}

/////////////////////////////////////////////////
// TBlobBsUsageStats
void TBlobBsUsageStats::PrintStats()
{
    TNotify::StdNotify->OnStatusFmt("Total used: %.1fMB", TotalUsed / TInt::Mega);
    TNotify::StdNotify->OnStatusFmt("Total free: %.1fMB", TotalFree / TInt::Mega);
    
    TNotify::StdNotify->OnStatus("Used blocks per size:");
    TUIntV KeyV; UsedBlockSizeToCntH.GetKeyV(KeyV);
    KeyV.Sort();
    for (int N = 0; N < KeyV.Len(); N++) {
        TNotify::StdNotify->OnStatusFmt("   Used blocks of size %d bytes: %d", KeyV[N], UsedBlockSizeToCntH.GetDat(KeyV[N]));
    }

    TNotify::StdNotify->OnStatus("Free blocks per size:");
    KeyV.Clr(); FreeBlockSizeToCntH.GetKeyV(KeyV);
    KeyV.Sort();
    for (int N = 0; N < KeyV.Len(); N++) {
        TNotify::StdNotify->OnStatusFmt("   Free blocks of size %d bytes: %d", KeyV[N], FreeBlockSizeToCntH.GetDat(KeyV[N]));
    }
}

/////////////////////////////////////////////////
// Blob-Base
const int TBlobBs::MnBlobBfL=16;
const int TBlobBs::MxBlobFLen=2000000000;

void TBlobBs::PutVersionStr(const PFRnd& File){
  File->PutStr(GetVersionStr());
}

void TBlobBs::AssertVersionStr(const PFRnd& File){
    TStr CorrVersionStr = GetVersionStr();
    bool IsOk = false;
    const TStr TestVersionStr = File->GetStr(CorrVersionStr.Len(), IsOk);
    //EAssert(IsOk && CorrVersionStr == TestVersionStr);
}

TStr TBlobBs::GetBlobBsStateStr(const TBlobBsState& BlobBsState){
    TStr StateStr;
    switch (BlobBsState){
        case bbsOpened: StateStr="Opened"; break;
        case bbsClosed: StateStr="Closed"; break;
        default: Fail; return TStr();
    }
    EAssert(StateStr.Len() == GetStateStrLen());
    return StateStr;
}

void TBlobBs::PutBlobBsStateStr(const PFRnd& File, const TBlobBsState& State){
    File->PutStr(GetBlobBsStateStr(State));
}

void TBlobBs::AssertBlobBsStateStr(const PFRnd& File, const TBlobBsState& State){
    TStr CorrStateStr=GetBlobBsStateStr(State);
    bool IsOk;
    TStr TestStateStr=File->GetStr(GetStateStrLen(), IsOk);
    if (!(IsOk && (CorrStateStr==TestStateStr))) {
        TExcept::ThrowFull("Error in AssertBlobBsStateStr!", TStr(__FILE__)+" line "+TInt::GetStr(__LINE__));
    }
}

const TStr TBlobBs::MxSegLenVNm = "MxSegLen";

void TBlobBs::PutMxSegLen(const PFRnd& File, const int& MxSegLen){
    File->PutStr(MxSegLenVNm);
    File->PutInt(MxSegLen);
}

int TBlobBs::GetMxSegLen(const PFRnd& File){
    EAssert(File->GetStr(MxSegLenVNm.Len())==MxSegLenVNm);
    return File->GetInt();
}

const TStr TBlobBs::BlockLenVNm = "BlockLenV";

// generate the vector of different sizes of blocks that can be stored in the blobs
void TBlobBs::GenBlockLenV(TIntV& BlockLenV){
    BlockLenV.Clr();
    for (int P2Exp=0; P2Exp<TB4Def::MxP2Exp; P2Exp++){
        BlockLenV.Add(TInt(TB4Def::GetP2(P2Exp)));
    }
    EAssert(int(BlockLenV.Last())<2000000000);

    {for (int Len=10; Len<100; Len+=10){BlockLenV.Add(Len);}}
    {for (int Len=100; Len<10000; Len+=100){BlockLenV.Add(Len);}}
    {for (int Len=10000; Len<100000; Len+=1000){BlockLenV.Add(Len);}}
    {for (int Len=100000; Len<1000000; Len+=25000){BlockLenV.Add(Len);}}
    {for (int Len=1000000; Len<10000000; Len+=1000000){BlockLenV.Add(Len);}}
    {for (int Len=10000000; Len<100000000; Len+=10000000){BlockLenV.Add(Len);}}

    BlockLenV.Sort();
}

// save block lengts to the file File at the current position
void TBlobBs::PutBlockLenV(const PFRnd& File, const TIntV& BlockLenV){
    File->PutStr(BlockLenVNm);
    File->PutInt(BlockLenV.Len());
    for (int BlockLenN=0; BlockLenN<BlockLenV.Len(); BlockLenN++){
        File->PutInt(BlockLenV[BlockLenN]);
    }
    File->PutInt(-1);
}

// load block lengts from the file File from the current position
void TBlobBs::GetBlockLenV(const PFRnd& File, TIntV& BlockLenV){
    EAssert(File->GetStr(BlockLenVNm.Len())==BlockLenVNm);
    BlockLenV.Gen(File->GetInt());
    for (int BlockLenN=0; BlockLenN<BlockLenV.Len(); BlockLenN++){
        BlockLenV[BlockLenN]=File->GetInt();
    }
    EAssert(File->GetInt()==-1);
}

const TStr TBlobBs::FFreeBlobPtVNm="FFreeBlobPtV";

void TBlobBs::GenFFreeBlobPtV(const TIntV& BlockLenV, TBlobPtV& FFreeBlobPtV){
    FFreeBlobPtV.Gen(BlockLenV.Len()+1);
}

void TBlobBs::PutFFreeBlobPtV(const PFRnd& File, const TBlobPtV& FFreeBlobPtV){
    File->PutStr(FFreeBlobPtVNm);
    File->PutInt(FFreeBlobPtV.Len());
    for (int BlockLenN=0; BlockLenN<FFreeBlobPtV.Len(); BlockLenN++){
        FFreeBlobPtV[BlockLenN].Save(File);
    }
    File->PutInt(-1);
}

void TBlobBs::GetFFreeBlobPtV(const PFRnd& File, TBlobPtV& FFreeBlobPtV){
    EAssert(File->GetStr(FFreeBlobPtVNm.Len())==FFreeBlobPtVNm);
    FFreeBlobPtV.Gen(File->GetInt());
    for (int FFreeBlobPtN=0; FFreeBlobPtN<FFreeBlobPtV.Len(); FFreeBlobPtN++){
        FFreeBlobPtV[FFreeBlobPtN]=TBlobPt::Load(File);
    }
    EAssert(File->GetInt()==-1);
}

void TBlobBs::GetAllocInfo(const int& BfL, const TIntV& BlockLenV, int& MxBfL, int& FFreeBlobPtN){
    int BlockLenN = 0;
    // find the index in BlockLenV where the available space is more than BfL
    while (BlockLenN < BlockLenV.Len() && BfL > BlockLenV[BlockLenN]) {
        BlockLenN++;
    }
    EAssert(BlockLenN<BlockLenV.Len());
    // se the return info for the available size and index
    MxBfL = BlockLenV[BlockLenN]; 
    FFreeBlobPtN = BlockLenN;
}

void TBlobBs::PutBlobTag(const PFRnd& File, const TBlobTag& BlobTag){
    switch (BlobTag){
        case btBegin: File->PutUInt(GetBeginBlobTag()); break;
        case btEnd: File->PutUInt(GetEndBlobTag()); break;
        default: Fail;
    }
}

void TBlobBs::AssertBlobTag(const PFRnd& File, const TBlobTag& BlobTag){
    switch (BlobTag){
        case btBegin: EAssert(File->GetUInt()==GetBeginBlobTag()); break;
        case btEnd: EAssert(File->GetUInt()==GetEndBlobTag()); break;
        default: TExcept::Throw("Error asserting BlobTag");
    }
}

void TBlobBs::PutBlobState(const PFRnd& File, const TBlobState& State){
    File->PutCh(char(State));
}

TBlobState TBlobBs::GetBlobState(const PFRnd& File){
    return TBlobState(int(File->GetCh()));
}

void TBlobBs::AssertBlobState(const PFRnd& File, const TBlobState& State){
    EAssertR(TBlobState(File->GetCh())==State, TStr::Fmt("Expected state %d, received %d", (int)State, (int)File->GetCh()));
}

void TBlobBs::AssertBfCsEqFlCs(const TCs& BfCs, const TCs& FCs){
    if (BfCs!=FCs){
        printf("[%d:%d]\n", BfCs.Get(), FCs.Get());
    }
    //EAssert(BfCs==FCs);
}

/////////////////////////////////////////////////
// General-Blob-Base
TStr TGBlobBs::GetNrBlobBsFNm(const TStr& BlobBsFNm){
    TStr NrBlobBsFNm=BlobBsFNm;
    if (NrBlobBsFNm.GetFExt().Empty()){
        NrBlobBsFNm=NrBlobBsFNm+".gbb";
    }
    return NrBlobBsFNm;
}

TGBlobBs::TGBlobBs(const TStr& BlobBsFNm, const TFAccess& _Access, const int& _MxSegLen):
    TBlobBs(), File(), Access(_Access), MxSegLen(_MxSegLen), BlockLenV(), FFreeBlobPtV(TB4Def::B4Bits), FirstBlobPt(){
    if (MxSegLen==-1){
        MxSegLen = MxBlobFLen;
    }
    const TStr NrBlobBsFNm = GetNrBlobBsFNm(BlobBsFNm);
    switch (Access){
        case faCreate:
            File=TFRnd::New(NrBlobBsFNm, faCreate, true); break;
        case faUpdate:
        case faRdOnly:
        case faRestore:
            File=TFRnd::New(NrBlobBsFNm, faUpdate, true); break;
        default: Fail;
    }
    if (File->Empty()){
        File->SetFPos(0);
        PutVersionStr(File);
        PutBlobBsStateStr(File, bbsOpened);
        PutMxSegLen(File, MxSegLen);
        // initialize and save the vector of block sizes
        GenBlockLenV(BlockLenV);
        PutBlockLenV(File, BlockLenV);
        // initialize and save the vector of pointers to free blobs
        GenFFreeBlobPtV(BlockLenV, FFreeBlobPtV);
        PutFFreeBlobPtV(File, FFreeBlobPtV);
    } 
    else {
        File->SetFPos(0);
        AssertVersionStr(File);
        int FPos=File->GetFPos();
        if (Access != faRestore) {
            AssertBlobBsStateStr(File, bbsClosed);
        }
        if (Access != faRdOnly) {
            File->SetFPos(FPos);
            PutBlobBsStateStr(File, bbsOpened);
        }
        MxSegLen=GetMxSegLen(File);
        // load vector of block sizes
        GetBlockLenV(File, BlockLenV);
        // load vector of pointers to free blobs
        GetFFreeBlobPtV(File, FFreeBlobPtV);
    }
    FirstBlobPt = TBlobPt(File->GetFPos());
    File->Flush();
}

TGBlobBs::~TGBlobBs(){
    if (Access != faRdOnly){
        File->SetFPos(0);
        PutVersionStr(File);
        PutBlobBsStateStr(File, bbsClosed);
        PutMxSegLen(File, MxSegLen);
        PutBlockLenV(File, BlockLenV);
        PutFFreeBlobPtV(File, FFreeBlobPtV);
    }
    File->Flush();
    File=NULL;
}

const int OverheadSize = sizeof(uint) + sizeof(int) + sizeof(char) + sizeof(int) + 2 * sizeof(TCs);

TBlobPt TGBlobBs::PutBlob(const PSIn& SIn){
    EAssert((Access==faCreate)||(Access==faUpdate)||(Access==faRestore));
    int BfL = SIn->Len();
    // get the index (FFreeBlobPtN) where there is enough space for storing SIn data
    int MxBfL; int FFreeBlobPtN;
    GetAllocInfo(BfL, BlockLenV, MxBfL, FFreeBlobPtN);
    TBlobPt BlobPt; TCs Cs;
    // if we don't have any blocks of the appropriate size that were previously deleted
    if (FFreeBlobPtV[FFreeBlobPtN].Empty()){
	    // allocate new block in BLOB storage
        const int FLen = File->GetFLen();
        if (FLen <= MxSegLen){
            EAssert(FLen<=MxBlobFLen);
            // create a new block of size MxBfl at the end of the file and populate it with data
            BlobPt=TBlobPt(FLen);
            File->SetFPos(BlobPt.GetAddr());
            PutBlobTag(File, btBegin);
            File->PutInt(MxBfL);
            PutBlobState(File, bsActive);
            File->PutInt(BfL);
            File->PutSIn(SIn, Cs);
            File->PutCh(TCh::NullCh, MxBfL-BfL);
            File->PutCs(Cs);
            PutBlobTag(File, btEnd);
            
	        Stats.AllocCount++;
	        Stats.AllocSize += MxBfL;
	        Stats.AllocUnusedSize += (MxBfL - BfL);
	        Stats.AllocUsedSize += BfL;
        }
    }
    // otherwise reuse existing BLOB pointer of the BLOB of the same size that was freed earlier
    else {
	    // get the pointer to the location of the right size
        BlobPt = FFreeBlobPtV[FFreeBlobPtN];
        File->SetFPos(BlobPt.GetAddr());
        AssertBlobTag(File, btBegin);
        const int MxBfL = File->GetInt();
        // get the location where we will start storing the data
        const int FPos = File->GetFPos();
        // make sure the block is really free
        AssertBlobState(File, bsFree);
        // deleted blocks are saved as "linked list" - the address of the next free block is stored in the content of next 4 bytes
        FFreeBlobPtV[FFreeBlobPtN] = TBlobPt::LoadAddr(File); 
        // move the file location back to the place where we start writing the data and save the data
        File->SetFPos(FPos);
        PutBlobState(File, bsActive);
        File->PutInt(BfL);
        File->PutSIn(SIn, Cs);
        File->PutCh(TCh::NullCh, MxBfL-BfL);
        File->PutCs(Cs);
        AssertBlobTag(File, btEnd);

	    Stats.AllocCount++;
	    Stats.AllocSize += MxBfL;
	    Stats.AllocUnusedSize += (MxBfL - BfL);
	    Stats.AllocUsedSize += BfL;
	    Stats.ReleasedCount--;
	    Stats.ReleasedSize -= MxBfL;
    }
    // TODO: why do we need to flush the file?
    File->Flush();
    Stats.PutsNew++;
    Stats.AvgPutNewLen += (BfL - Stats.AvgPutNewLen) / Stats.PutsNew;
    return BlobPt;
}

TBlobPt TGBlobBs::PutBlob(const TBlobPt& BlobPt, const PSIn& SIn, int& ReleasedSize){
    EAssert((Access==faCreate)||(Access==faUpdate)||(Access==faRestore));
    int BfL = SIn->Len();

    File->SetFPos(BlobPt.GetAddr());
    AssertBlobTag(File, btBegin);
    int MxBfL = File->GetInt();
    AssertBlobState(File, bsActive);
    if (BfL > MxBfL){
	    Stats.SizeChngs++;
        // remember the size of the chunk that we are releasing. needed to notify the level above
        // that we have space available to fill
        ReleasedSize = DelBlob(BlobPt);
        return PutBlob(SIn);
    } 
    else {
        // we are not releasing any data chunk
        ReleasedSize = -1;
	    const int FPos = File->GetFPos();
	    const int OldBfL = File->GetInt();
	    File->SetFPos(FPos);

        TCs Cs;
        File->PutInt(BfL);
        File->PutSIn(SIn, Cs);
        File->PutCh(TCh::NullCh, MxBfL-BfL);
        File->PutCs(Cs);
        PutBlobTag(File, btEnd);
        // TODO: why do we need to flush?
        File->Flush();
	    // update stats
	    Stats.Puts++;
        Stats.AvgPutLen += (BfL - Stats.AvgPutLen) / Stats.Puts;
	    Stats.AllocUnusedSize -= BfL - OldBfL;
	    Stats.AllocUsedSize += BfL - OldBfL;
	    return BlobPt;
    }
}

/// Load the data for the blob poiner BlobPt
PSIn TGBlobBs::GetBlob(const TBlobPt& BlobPt){
    File->SetFPos(BlobPt.GetAddr());
    AssertBlobTag(File, btBegin);
    const int MxBfL = File->GetInt();
    AssertBlobState(File, bsActive);
    int BfL = File->GetInt();
    TCs BfCs; PSIn SIn = File->GetSIn(BfL, BfCs);
    File->MoveFPos(MxBfL-BfL);
    const TCs FCs = File->GetCs();
    AssertBlobTag(File, btEnd);
    AssertBfCsEqFlCs(BfCs, FCs);
    Stats.Gets++;
    Stats.AvgGetLen += (BfL - Stats.AvgGetLen) / Stats.Gets;
    return SIn;
}

/// Deletes specified BLOB and mark it as reusable
int TGBlobBs::DelBlob(const TBlobPt& BlobPt){
    EAssert((Access==faCreate)||(Access==faUpdate)||(Access==faRestore));
    File->SetFPos(BlobPt.GetAddr());                   // find BLOB start
    AssertBlobTag(File, btBegin);
    const int MxBfL = File->GetInt();                  // read buffer length
    const int FPos = File->GetFPos();                  // remember position of status flag
    AssertBlobState(File, bsActive);                   // make sure BLOB is active
    const int BfL = File->GetInt();
    File->SetFPos(FPos);
    PutBlobState(File, bsFree);                        // mark BLOB as free
    int _MxBfL; int FFreeBlobPtN;
    GetAllocInfo(MxBfL, BlockLenV, _MxBfL, FFreeBlobPtN);
    EAssert(MxBfL == _MxBfL);
    FFreeBlobPtV[FFreeBlobPtN].SaveAddr(File);         // save to the deleted blob the location of next deleted (reusable) blob
    FFreeBlobPtV[FFreeBlobPtN] = BlobPt;               // save into the vector of deleted blobs the location to the now deleted blob
    File->PutCh(TCh::NullCh, MxBfL+sizeof(TCs));       // erase existing content
    AssertBlobTag(File, btEnd);
    // TODO : why do we need this flush?
    File->Flush();                                     // write to disk

    // update stats
    Stats.Dels++;
    Stats.AllocCount--;
    Stats.AllocSize -= MxBfL;
    Stats.AllocUnusedSize -= (MxBfL - BfL);
    Stats.AllocUsedSize -= BfL;
    Stats.ReleasedCount++;
    Stats.ReleasedSize += MxBfL;
    // return the amount of the buffer that we released
    return MxBfL;
}

TBlobPt TGBlobBs::FFirstBlobPt(){
    return FirstBlobPt;
}

bool TGBlobBs::FNextBlobPt(TBlobPt& TrvBlobPt, TBlobPt& BlobPt, PSIn& BlobSIn){
    forever {
        const uint TrvBlobAddr = TrvBlobPt.GetAddr();
        if (TrvBlobAddr>=uint(File->GetFLen())) {
            TrvBlobPt.Clr(); BlobPt.Clr(); BlobSIn=NULL;
            return false;
        } 
        else {
            File->SetFPos(TrvBlobAddr);
            AssertBlobTag(File, btBegin);
            const int MxBfL = File->GetInt();
            TBlobState BlobState=GetBlobState(File);
            switch (BlobState){
                case bsActive:{
                    const int BfL = File->GetInt();
                    TCs BfCs; 
                    BlobSIn = File->GetSIn(BfL, BfCs);
                    File->MoveFPos(MxBfL-BfL);
                    const TCs FCs=File->GetCs();
                    AssertBlobTag(File, btEnd);
                    AssertBfCsEqFlCs(BfCs, FCs);
                    BlobPt = TrvBlobPt;
                    TrvBlobPt = TBlobPt(File->GetFPos());
                    return true;}
                case bsFree:
                    File->MoveFPos(sizeof(uint)+MxBfL+sizeof(TCs));
                    AssertBlobTag(File, btEnd);
                    TrvBlobPt=TBlobPt(File->GetFPos());
                    break;
                default: 
                    Fail; 
                    return false;
            }
        }
    }
}

bool TGBlobBs::Exists(const TStr& BlobBsFNm){
    TStr NrBlobBsFNm=GetNrBlobBsFNm(BlobBsFNm);
    return TFile::Exists(NrBlobBsFNm);
}

void TGBlobBs::ComputeUsageStats(TBlobBsUsageStats& UsageStats)
{
    TBlobPt BlobPt = GetFirstBlobPt();
    uint Addr = BlobPt.GetAddr();
    const uint FLen = (uint) File->GetFLen(); 
    while (Addr < FLen) {
        File->SetFPos(Addr);
        AssertBlobTag(File, btBegin);
        const int MxBfL = File->GetInt();
        if (TBlobState(File->GetCh()) == bsActive) {
            UsageStats.TotalUsed += (uint) MxBfL;
            UsageStats.UsedBlockSizeToCntH.AddDat(MxBfL) += 1;
        }
        else {
            UsageStats.TotalFree += (uint) MxBfL;
            UsageStats.FreeBlockSizeToCntH.AddDat(MxBfL) += 1;
        }
        const uint Size = sizeof(uint) + sizeof(int) + sizeof(char) + sizeof(int) + (uint) MxBfL + 2*sizeof(TCs);
        Addr += Size;
    }
}

/////////////////////////////////////////////////
// Multiple-File-Blob-Base
void TMBlobBs::GetNrFPathFMid(const TStr& BlobBsFNm, TStr& NrFPath, TStr& NrFMid){
    NrFPath = TStr::GetNrFPath(BlobBsFNm.GetFPath());
    NrFMid = TStr::GetNrFMid(BlobBsFNm.GetFMid());
}

TStr TMBlobBs::GetMainFNm(const TStr& NrFPath, const TStr& NrFMid){
    return NrFPath+NrFMid+".mbb";
}

TStr TMBlobBs::GetSegFNm(const TStr& NrFPath, const TStr& NrFMid, const int& SegN){
    return NrFPath+NrFMid+".mbb"+""+TStr::GetNrNumFExt(SegN, 5);
}

void TMBlobBs::LoadMain(int& Segs){
    PSIn SIn = TFIn::New(GetMainFNm(NrFPath, NrFMid));
    TILx Lx(SIn, TFSet()|oloFrcEoln|oloSigNum|oloCsSens);
    EAssert(Lx.GetVarStr("Version")==GetVersionStr());
    MxSegLen = Lx.GetVarInt("MxSegLen");
    Segs = Lx.GetVarInt("Segments");
}

void TMBlobBs::SaveMain() const {
    PSOut SOut = TFOut::New(GetMainFNm(NrFPath, NrFMid));
    TOLx Lx(SOut, TFSet()|oloFrcEoln|oloSigNum|oloCsSens);
    Lx.PutVarStr("Version", GetVersionStr());
    Lx.PutVarInt("MxSegLen", MxSegLen);
    Lx.PutVarInt("Segments", SegV.Len());
}

TMBlobBs::TMBlobBs(const TStr& BlobBsFNm, const TFAccess& _Access, const int& _MxSegLen):
    TBlobBs(), Access(_Access), MxSegLen(_MxSegLen), NrFPath(), NrFMid(), SegV() {
    if (MxSegLen == -1){ 
        MxSegLen=MxBlobFLen; 
    }
    // initialize the hashtable of block sizes to first segment
    TBlobBs::GenBlockLenV(BlockLenV);
    for (int N = 0; N < BlockLenV.Len(); N++) {
        BlockSizeToSegH.AddDat(BlockLenV[N], 0);
    }
    GetNrFPathFMid(BlobBsFNm, NrFPath, NrFMid);
    switch (Access){
        case faCreate:{
            TFile::DelWc(BlobBsFNm+".*");
            TStr SegFNm=GetSegFNm(NrFPath, NrFMid, 0);
            PBlobBs Seg=TGBlobBs::New(SegFNm, faCreate, MxSegLen);
            SegV.Add(Seg);
            SaveMain(); 
            break; }
        case faUpdate:
        case faRdOnly:{
            int Segs; LoadMain(Segs);
            for (int SegN=0; SegN<Segs; SegN++){
                TStr SegFNm=GetSegFNm(NrFPath, NrFMid, SegN);
                SegV.Add(TGBlobBs::New(SegFNm, Access, MxSegLen));
            }
            break;}
        case faRestore:{
            // assume no segments
            int Segs=-1;
            // if main-file exists
            if (TFile::Exists(GetMainFNm(NrFPath, NrFMid))){
                // load main file
                int _Segs; LoadMain(_Segs);
                // load segment-files which exist
                Segs=0;
                forever {
                    // get segment file-name
                    TStr SegFNm=GetSegFNm(NrFPath, NrFMid, Segs);
                    // if segment-file exists then add segment else break check-loop
                    if (TFile::Exists(SegFNm)){
                        SegV.Add(TGBlobBs::New(SegFNm, Access, MxSegLen));
                        Segs++;
                    } else {
                        break;
                    }
                }
            }
            // if no segments exist then create blob-base from scratch
            if (Segs==-1){
                const TStr SegFNm = GetSegFNm(NrFPath, NrFMid, 0);
                PBlobBs Seg=TGBlobBs::New(SegFNm, faCreate, MxSegLen);
                SegV.Add(Seg);
                SaveMain();
            }
            break;}
        default: Fail;
    }
}

TMBlobBs::~TMBlobBs(){
    if (Access!=faRdOnly){
        SaveMain();
    }
}

// save a new buffer in SIn to a blob
TBlobPt TMBlobBs::PutBlob(const PSIn& SIn){
    EAssert((Access==faCreate)||(Access==faUpdate)||(Access==faRestore));
    // buffer size that we need to store
    const int BfL = SIn->Len();
    // segment index from which we will start to check if we can insert data into
    uint16 DestSegN = 0;
    int BlockSize = 0;
    for (int KeyId = BlockSizeToSegH.FFirstKeyId(); BlockSizeToSegH.FNextKeyId(KeyId);) {
        BlockSize = BlockSizeToSegH.GetKey(KeyId);
        // if we found a block size that could store the buffer
        if (BlockSize > BfL) {
            DestSegN = BlockSizeToSegH[KeyId];
            break;
        }
    }
    // DestSegN should now be an index of segment from which we will start checking if the segment has place for the given data
    // if not, the index will be increased
    TBlobPt BlobPt;
    while (DestSegN < SegV.Len()) {
        BlobPt = SegV[DestSegN]->PutBlob(SIn);
        if (!BlobPt.Empty()) {
            break;
        }
        DestSegN++;
    }
    // we were not able to find a segment of required size in any of the available segments
    // create a new segment
    if (BlobPt.Empty()) {
        const TStr SegFNm = GetSegFNm(NrFPath, NrFMid, SegV.Len());
        PBlobBs Seg = TGBlobBs::New(SegFNm, faCreate, MxSegLen);
        DestSegN = SegV.Add(Seg);
        EAssert(DestSegN < TUSInt::Mx);
        BlobPt = SegV[DestSegN]->PutBlob(SIn);
    }
    // remember the segment index to which we are able to write buffer of this length
    BlockSizeToSegH.AddDat(BlockSize, DestSegN);
    if (!BlobPt.Empty()){
        BlobPt.PutSeg(uint16(DestSegN));
    }
    return BlobPt;
}

// save (update) a buffer from SIn that we already have stored in BlobPt
TBlobPt TMBlobBs::PutBlob(const TBlobPt& BlobPt, const PSIn& SIn, int& ReleasedSize){
    EAssert((Access==faCreate)||(Access==faUpdate)||(Access==faRestore));
    // get the segment is which the data currently stored
    const uint16 SegN = BlobPt.GetSeg();
    // try to store the SIn into the current segment
    TBlobPt NewBlobPt = SegV[SegN]->PutBlob(BlobPt, SIn, ReleasedSize);
    // if we have released the previously used data chunk then remember that we have a buffer
    // in SegN of ReleasedSize available to be filled
    if (ReleasedSize > 0) {
        BlockSizeToSegH.AddDat(ReleasedSize) = MIN(SegN, (uint16) BlockSizeToSegH.GetDatOrDef(ReleasedSize, 0));
    }
    if (NewBlobPt.Empty()){
        // we were not able to save the data in the current segment so we want to store it as a new blob
        // the data in the old blob was already removed
        NewBlobPt = PutBlob(SIn);
    } 
    else {
        // remember in which segment we put the blob
        NewBlobPt.PutSeg(BlobPt.GetSeg());
    }
    return NewBlobPt;
}

PSIn TMBlobBs::GetBlob(const TBlobPt& BlobPt){
    const uint16 SegN = BlobPt.GetSeg();
    return SegV[SegN]->GetBlob(BlobPt);
}

int TMBlobBs::DelBlob(const TBlobPt& BlobPt){
    // get the index of the segement from which we will remove the data
    const uint16 SegN = BlobPt.GetSeg();
    // remove the data. Obtain also the info about the blob size that was released
    int ReleasedSize = SegV[SegN]->DelBlob(BlobPt);
    // we released a certain chunk. Mark in the BlockSizeToSegH the index of the segment if lower than current
    BlockSizeToSegH.AddDat(ReleasedSize) = MIN(SegN, (uint16) BlockSizeToSegH.GetDatOrDef(ReleasedSize, 0));
    return ReleasedSize;
}

TBlobPt TMBlobBs::GetFirstBlobPt(){
    return SegV[0]->GetFirstBlobPt();
}

TBlobPt TMBlobBs::FFirstBlobPt(){
    return SegV[0]->FFirstBlobPt();
}

bool TMBlobBs::FNextBlobPt(TBlobPt& TrvBlobPt, TBlobPt& BlobPt, PSIn& BlobSIn){
    uint16 SegN = TrvBlobPt.GetSeg();
    if (SegV[SegN]->FNextBlobPt(TrvBlobPt, BlobPt, BlobSIn)){
        TrvBlobPt.PutSeg(SegN);
        BlobPt.PutSeg(SegN);
        return true;
    }
    else if (SegN == SegV.Len()-1){
        return false;
    }
    else {
        SegN++;
        TrvBlobPt = SegV[SegN]->FFirstBlobPt();
        TrvBlobPt.PutSeg(SegN);
        return FNextBlobPt(TrvBlobPt, BlobPt, BlobSIn);
    }
}

bool TMBlobBs::Exists(const TStr& BlobBsFNm){
    TStr NrFPath; TStr NrFMid; 
    GetNrFPathFMid(BlobBsFNm, NrFPath, NrFMid);
    const TStr MainFNm = GetMainFNm(NrFPath, NrFMid);
    return TFile::Exists(MainFNm);
}

const TBlobBsStats& TMBlobBs::GetStats() {
    Stats.Reset();
    for (int i = 0; i < SegV.Len(); i++) {
        Stats.Add(SegV[i]->GetStats());
    }
    return Stats;
}

void TMBlobBs::ResetStats() {
    Stats.Reset();
    for (int i = 0; i < SegV.Len(); i++) {
        SegV[i]->ResetStats();
    }
}
