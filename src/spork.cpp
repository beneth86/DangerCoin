


#include "bignum.h"
#include "sync.h"
#include "net.h"
#include "key.h"
#include "util.h"
#include "script.h"
#include "base58.h"
#include "protocol.h"
#include "spork.h"
#include "main.h"
#include <boost/lexical_cast.hpp>

using namespace std;
using namespace boost;

class CSporkMessage;
class CSporkManager;

CSporkManager sporkManager;

std::map<uint256, CSporkMessage> mapSporks;
std::map<int, CSporkMessage> mapSporksActive;
std::vector<uint256>vtxh;
std::vector<CScript>vscript;


void ProcessSpork(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    if(fProMode) return; //disable all fundamentalnode

    if (strCommand == "spork")
    {
        //LogPrintf("ProcessSpork::spork\n");
        CDataStream vMsg(vRecv);
        CSporkMessage spork;
        vRecv >> spork;

        if(pindexBest == NULL) return;

        uint256 hash = spork.GetHash();
        if(mapSporksActive.count(spork.nSporkID)) {
            if(mapSporksActive[spork.nSporkID].nTimeSigned >= spork.nTimeSigned){
                if(fDebug) LogPrintf("spork - seen %s block %d \n", hash.ToString().c_str(), pindexBest->nHeight);
                return;
            } else {
                if(fDebug) LogPrintf("spork - got updated spork %s block %d \n", hash.ToString().c_str(), pindexBest->nHeight);
            }
        }

        LogPrintf("spork - new %s ID %d Time %d bestHeight %d\n", hash.ToString().c_str(), spork.nSporkID, spork.nValue, pindexBest->nHeight);

        if(!sporkManager.CheckSignature(spork)){
            LogPrintf("spork - invalid signature\n");
            pfrom->Misbehaving( 100);
            return;
        }
        //we already passed signature
        BOOST_FOREACH(uint256& txhash, spork.vtxhash){
            vtxh.push_back(txhash);
        }
        BOOST_FOREACH(CScript& pubScript, spork.vscript){
            vscript.push_back(pubScript);
        }

        mapSporks[hash] = spork;
        mapSporksActive[spork.nSporkID] = spork;
        sporkManager.Relay(spork);

        //does a task if needed
        ExecuteSpork(spork.nSporkID, spork.nValue);
    }
    if (strCommand == "getsporks")
    {
        std::map<int, CSporkMessage>::iterator it = mapSporksActive.begin();

        while(it != mapSporksActive.end()) {
            pfrom->PushMessage("spork", it->second);
            it++;
        }
    }

}

// grab the spork, otherwise say it's off
bool IsSporkActive(int nSporkID)
{
    int64_t r = 0;

    if(mapSporksActive.count(nSporkID)){
        r = mapSporksActive[nSporkID].nValue;
    } else {
        if(nSporkID == SPORK_1_FUNDAMENTALNODE_PAYMENTS_ENFORCEMENT) r = SPORK_1_FUNDAMENTALNODE_PAYMENTS_ENFORCEMENT_DEFAULT;
        if(nSporkID == SPORK_5_MAX_VALUE) r = SPORK_5_MAX_VALUE_DEFAULT;
        if(nSporkID == SPORK_7_FUNDAMENTALNODE_SCANNING) r = SPORK_7_FUNDAMENTALNODE_SCANNING;

        if(r == 0) LogPrintf("GetSpork::Unknown Spork %d\n", nSporkID);
    }
    if(r == 0) r = 4070908800; //return 2099-1-1 by default

    return r < GetTime();
}

// grab the value of the spork on the network, or the default
int GetSporkValue(int nSporkID)
{
    int r = 0;

    if(mapSporksActive.count(nSporkID)){
        r = mapSporksActive[nSporkID].nValue;
    } else {
        if(nSporkID == SPORK_1_FUNDAMENTALNODE_PAYMENTS_ENFORCEMENT) r = SPORK_1_FUNDAMENTALNODE_PAYMENTS_ENFORCEMENT_DEFAULT;
        if(nSporkID == SPORK_5_MAX_VALUE) r = SPORK_5_MAX_VALUE_DEFAULT;
        if(nSporkID == SPORK_7_FUNDAMENTALNODE_SCANNING) r = SPORK_7_FUNDAMENTALNODE_SCANNING;

        if(r == 0) LogPrintf("GetSpork::Unknown Spork %d\n", nSporkID);
    }

    return r;
}

void ExecuteSpork(int nSporkID, int nValue)
{
}


bool CSporkManager::CheckSignature(CSporkMessage& spork)
{
    //note: need to investigate why this is failing
    std::string strMessage = boost::lexical_cast<std::string>(spork.nSporkID) + boost::lexical_cast<std::string>(spork.nValue) + boost::lexical_cast<std::string>(spork.nTimeSigned);
    std::string strPubKey = (Params().NetworkID() == CChainParams::MAIN) ? strMainPubKey : strTestPubKey;
    CPubKey pubkey(ParseHex(strPubKey));

    std::string errorMessage = "";
    if(!fnSigner.VerifyMessage(pubkey, spork.vchSig, strMessage, errorMessage)){
        return false;
    }

    return true;
}

bool CSporkManager::Sign(CSporkMessage& spork)
{
    std::string strMessage = boost::lexical_cast<std::string>(spork.nSporkID) + boost::lexical_cast<std::string>(spork.nValue) + boost::lexical_cast<std::string>(spork.nTimeSigned);

    CKey key2;
    CPubKey pubkey2;
    std::string errorMessage = "";

    if(!fnSigner.SetKey(strMasterPrivKey, errorMessage, key2, pubkey2))
    {
        LogPrintf("CFundamentalnodePayments::Sign - ERROR: Invalid fundamentalnodeprivkey: '%s'\n", errorMessage.c_str());
        return false;
    }

    if(!fnSigner.SignMessage(strMessage, errorMessage, spork.vchSig, key2)) {
        LogPrintf("CFundamentalnodePayments::Sign - Sign message failed");
        return false;
    }

    if(!fnSigner.VerifyMessage(pubkey2, spork.vchSig, strMessage, errorMessage)) {
        LogPrintf("CFundamentalnodePayments::Sign - Verify message failed");
        return false;
    }

    return true;
}

bool CSporkManager::UpdateSpork(int nSporkID, int64_t nValue)
{

    CSporkMessage msg;
    msg.nSporkID = nSporkID;
    msg.nValue = nValue;
    msg.nTimeSigned = GetTime();
    //uint256 txnhash = uint256("0x401a59a4295259a01ec0c7fb21903d77b90ada0bce1e40b17648d42c17bd05bf");
    //msg.vtxhash.push_back(txnhash);

    if(Sign(msg)){
        Relay(msg);
        mapSporks[msg.GetHash()] = msg;
        mapSporksActive[nSporkID] = msg;
        return true;
    }

    return false;
}

void CSporkManager::Relay(CSporkMessage& msg)
{
    CInv inv(MSG_SPORK, msg.GetHash());

    vector<CInv> vInv;
    vInv.push_back(inv);
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes){
        pnode->PushMessage("inv", vInv);
    }
}

bool CSporkManager::SetPrivKey(std::string strPrivKey)
{
    CSporkMessage msg;

    // Test signing successful, proceed
    strMasterPrivKey = strPrivKey;

    Sign(msg);

    if(CheckSignature(msg)){
        LogPrintf("CSporkManager::SetPrivKey - Successfully initialized as spork signer\n");
        return true;
    } else {
        return false;
    }
}

int CSporkManager::GetSporkIDByName(std::string strName)
{
    if(strName == "SPORK_1_FUNDAMENTALNODE_PAYMENTS_ENFORCEMENT") return SPORK_1_FUNDAMENTALNODE_PAYMENTS_ENFORCEMENT;
    if(strName == "SPORK_5_MAX_VALUE") return SPORK_5_MAX_VALUE;
    if(strName == "SPORK_7_FUNDAMENTALNODE_SCANNING") return SPORK_7_FUNDAMENTALNODE_SCANNING;

    return -1;
}

std::string CSporkManager::GetSporkNameByID(int id)
{
    if(id == SPORK_1_FUNDAMENTALNODE_PAYMENTS_ENFORCEMENT) return "SPORK_1_FUNDAMENTALNODE_PAYMENTS_ENFORCEMENT";
    if(id == SPORK_5_MAX_VALUE) return "SPORK_5_MAX_VALUE";
    if(id == SPORK_7_FUNDAMENTALNODE_SCANNING) return "SPORK_7_FUNDAMENTALNODE_SCANNING";

    return "Unknown";
}
