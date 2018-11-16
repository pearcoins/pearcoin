// Copyright (c) 2012-2018 The Peercoin developers
// Distributed under conditional MIT/X11 software license,
// see the accompanying file COPYING
//
// Concepts
//
// In the network there can be a privileged node known as 'checkpoint master'.
// This node can send out checkpoint messages signed by the checkpoint master
// key. Each checkpoint is a block hash, representing a block on the blockchain
// that the network should reach consensus on.
//
// Besides verifying signatures of checkpoint messages, each node also verifies
// the consistency of the checkpoints. If a conflicting checkpoint is received,
// it means either the checkpoint master key is compromised, or there is an
// operator mistake. In this situation the node would discard the conflicting
// checkpoint message and display a warning message. This precaution controls
// the damage to network caused by operator mistake or compromised key.
//
// Operations
//
// Checkpoint master key can be established by using the 'makekeypair' command
// The public key in source code should then be updated and private key kept
// in a safe place.
//
// Any node can be turned into checkpoint master by setting the 'checkpointkey'
// configuration parameter with the private key of the checkpoint master key.
// Operator should exercise caution such that at any moment there is at most
// one node operating as checkpoint master. When switching master node, the
// recommended procedure is to shutdown the master node and restart as
// regular node, note down the current checkpoint by 'getcheckpoint', then
// compare to the checkpoint at the new node to be upgraded to master node.
// When the checkpoint on both nodes match then it is safe to switch the new
// node to checkpoint master.
//
// The configuration parameter 'checkpointdepth' specifies how many blocks
// should the checkpoints lag behind the latest block in auto checkpoint mode.
// A depth of 0 is the strongest auto checkpoint policy and offers the greatest
// protection against 51% attack. A negative depth means that the checkpoints
// should not be automatically generated by the checkpoint master, but instead
// be manually entered by operator via the 'sendcheckpoint' command. The manual
// mode is also the default mode (default value -1 for checkpointdepth).
//
// Command and configuration parameter 'enforcecheckpoint'
// is for the users to explicitly consent to enforce the checkpoints issued
// from checkpoint master. To enforce checkpoint, user needs to either issue
// command 'enforcecheckpoint true', or set configuration parameter
// enforcecheckpoint=1. The current enforcement setting can be queried via
// command 'getcheckpoint', where 'subscribemode' displays either 'enforce'
// or 'advisory'. The 'enforce' mode of subscribemode means checkpoints are
// enforced. The 'advisory' mode of subscribemode means checkpoints are not
// enforced but a warning message would be displayed if the node is on a 
// different blockchain fork from the checkpoint.
//

#include <checkpointsync.h>
#include <validation.h>
#include <txdb.h>
#include <consensus/validation.h>
#include <checkpoints.h>
#include <chainparams.h>
#include <base58.h>

using namespace std;


// peercoin: sync-checkpoint master key
const std::string CSyncCheckpoint::strMainPubKey = "04c0c707c28533fd5c9f79d2d3a2d80dff259ad8f915241cd14608fb9bc07c74830efe8438f2b272a866b4af5e0c2cc2a9909972aefbd976937e39f46bb38c277c";
const std::string CSyncCheckpoint::strTestPubKey = "0400c195be8d5194007b3f02249f785a51505776bd8f43cc6d49206163e08a63ad9009c814966921c361b14949c51e281edc9347e7ce0e8c57019df1313a6cac7b";
std::string CSyncCheckpoint::strMasterPrivKey = "";


// peercoin: synchronized checkpoint (centrally broadcasted)
uint256 hashSyncCheckpoint = uint256();
uint256 hashPendingCheckpoint = uint256();
CSyncCheckpoint checkpointMessage;
CSyncCheckpoint checkpointMessagePending;
uint256 hashInvalidCheckpoint = uint256();
CCriticalSection cs_hashSyncCheckpoint;
std::string strCheckpointWarning;

// peercoin: get last synchronized checkpoint
CBlockIndex* GetLastSyncCheckpoint()
{
    LOCK(cs_hashSyncCheckpoint);
    if (!mapBlockIndex.count(hashSyncCheckpoint))
        error("GetSyncCheckpoint: block index missing for current sync-checkpoint %s", hashSyncCheckpoint.ToString());
    else
        return mapBlockIndex[hashSyncCheckpoint];
    return NULL;
}

// peercoin: only descendant of current sync-checkpoint is allowed
bool ValidateSyncCheckpoint(uint256 hashCheckpoint)
{
    if (!mapBlockIndex.count(hashSyncCheckpoint))
        return error("ValidateSyncCheckpoint: block index missing for current sync-checkpoint %s", hashSyncCheckpoint.ToString());
    if (!mapBlockIndex.count(hashCheckpoint))
        return error("ValidateSyncCheckpoint: block index missing for received sync-checkpoint %s", hashCheckpoint.ToString());

    CBlockIndex* pindexSyncCheckpoint = mapBlockIndex[hashSyncCheckpoint];
    CBlockIndex* pindexCheckpointRecv = mapBlockIndex[hashCheckpoint];

    if (pindexCheckpointRecv->nHeight <= pindexSyncCheckpoint->nHeight)
    {
        // Received an older checkpoint, trace back from current checkpoint
        // to the same height of the received checkpoint to verify
        // that current checkpoint should be a descendant block
        CBlockIndex* pindex = pindexSyncCheckpoint;
        while (pindex->nHeight > pindexCheckpointRecv->nHeight)
            if (!(pindex = pindex->pprev))
                return error("ValidateSyncCheckpoint: pprev1 null - block index structure failure");
        if (pindex->GetBlockHash() != hashCheckpoint)
        {
            hashInvalidCheckpoint = hashCheckpoint;
            return error("ValidateSyncCheckpoint: new sync-checkpoint %s is conflicting with current sync-checkpoint %s", hashCheckpoint.ToString(), hashSyncCheckpoint.ToString());
        }
        return false; // ignore older checkpoint
    }

    // Received checkpoint should be a descendant block of the current
    // checkpoint. Trace back to the same height of current checkpoint
    // to verify.
    CBlockIndex* pindex = pindexCheckpointRecv;
    while (pindex->nHeight > pindexSyncCheckpoint->nHeight)
        if (!(pindex = pindex->pprev))
            return error("ValidateSyncCheckpoint: pprev2 null - block index structure failure");
    if (pindex->GetBlockHash() != hashSyncCheckpoint)
    {
        hashInvalidCheckpoint = hashCheckpoint;
        return error("ValidateSyncCheckpoint: new sync-checkpoint %s is not a descendant of current sync-checkpoint %s", hashCheckpoint.ToString(), hashSyncCheckpoint.ToString());
    }
    return true;
}

bool WriteSyncCheckpoint(const uint256& hashCheckpoint)
{
    if (!pblocktree->WriteSyncCheckpoint(hashCheckpoint))
    {
        return error("WriteSyncCheckpoint(): failed to write to txdb sync checkpoint %s", hashCheckpoint.ToString());
    }
    if (!pblocktree->Sync())
        return error("WriteSyncCheckpoint(): failed to commit to txdb sync checkpoint %s", hashCheckpoint.ToString());

    hashSyncCheckpoint = hashCheckpoint;
    return true;
}

bool IsSyncCheckpointEnforced()
{
    return (gArgs.GetBoolArg("-enforcecheckpoint", true) || gArgs.IsArgSet("-checkpointkey")); // checkpoint master node is always enforced
}

void SetCheckpointEnforce(bool fEnforce)
{
    if (fEnforce)
        strCheckpointWarning = "";
    gArgs.ForceSetArg("-enforcecheckpoint", fEnforce ? "1" : "0");
}

bool AcceptPendingSyncCheckpoint()
{
    LOCK(cs_hashSyncCheckpoint);
    if (hashPendingCheckpoint != uint256() && mapBlockIndex.count(hashPendingCheckpoint))
    {
        if (!ValidateSyncCheckpoint(hashPendingCheckpoint))
        {
            hashPendingCheckpoint = uint256();
            checkpointMessagePending.SetNull();
            return false;
        }

        CBlockIndex* pindexCheckpoint = mapBlockIndex[hashPendingCheckpoint];
        if (IsSyncCheckpointEnforced() && !chainActive.Contains(pindexCheckpoint))
        {
            //ppcTODO - redo this somehow
//            CValidationState state;
//            if (!SetBestChain(state, pindexCheckpoint))
//            {
//                hashInvalidCheckpoint = hashPendingCheckpoint;
//                return error("AcceptPendingSyncCheckpoint: SetBestChain failed for sync checkpoint %s", hashPendingCheckpoint.ToString());
//            }
        }

        if (!WriteSyncCheckpoint(hashPendingCheckpoint))
            return error("AcceptPendingSyncCheckpoint(): failed to write sync checkpoint %s", hashPendingCheckpoint.ToString());
        hashPendingCheckpoint = uint256();
        checkpointMessage = checkpointMessagePending;
        checkpointMessagePending.SetNull();
        LogPrintf("AcceptPendingSyncCheckpoint : sync-checkpoint at %s\n", hashSyncCheckpoint.ToString());
        // relay the checkpoint
        if (g_connman && !checkpointMessage.IsNull())
            g_connman->ForEachNode([](CNode* pnode) {
                checkpointMessage.RelayTo(pnode);
            });
        return true;
    }
    return false;
}

// Automatically select a suitable sync-checkpoint 
uint256 AutoSelectSyncCheckpoint()
{
    // Search backward for a block with specified depth policy
    const CBlockIndex *pindex = chainActive.Tip();
    while (pindex->pprev && pindex->nHeight + (int)gArgs.GetArg("-checkpointdepth", -1) > chainActive.Tip()->nHeight)
        pindex = pindex->pprev;
    return pindex->GetBlockHash();
}

// Check against synchronized checkpoint
bool CheckSyncCheckpoint(const uint256& hashBlock, const CBlockIndex* pindexPrev)
{
   // skip checks during reindex, except for genesis block
   if (pindexPrev->nHeight > 0 && chainActive.Height() == 0) return true;

    //ppcTODO - needs rewrite, because it is very slow during accepting headers
    return true;
//    int nHeight = pindexPrev->nHeight + 1;

//    LOCK(cs_hashSyncCheckpoint);
//    // sync-checkpoint should always be accepted block
//    assert(mapBlockIndex.count(hashSyncCheckpoint));
//    const CBlockIndex* pindexSync = mapBlockIndex[hashSyncCheckpoint];

//    if (nHeight > pindexSync->nHeight)
//    {
//        // trace back to same height as sync-checkpoint
//        const CBlockIndex* pindex = pindexPrev;
//        while (pindex->nHeight > pindexSync->nHeight)
//            if (!(pindex = pindex->pprev))
//                return error("CheckSyncCheckpoint: pprev null - block index structure failure");
//        if (pindex->nHeight < pindexSync->nHeight || pindex->GetBlockHash() != hashSyncCheckpoint)
//            return false; // only descendant of sync-checkpoint can pass check
//    }
//    if (nHeight == pindexSync->nHeight && hashBlock != hashSyncCheckpoint)
//        return false; // same height with sync-checkpoint
//    if (nHeight < pindexSync->nHeight && !mapBlockIndex.count(hashBlock))
//        return false; // lower height than sync-checkpoint
//    return true;
}

// peercoin: reset synchronized checkpoint to last hardened checkpoint
bool ResetSyncCheckpoint()
{
    LOCK(cs_hashSyncCheckpoint);
    const uint256& hash = Checkpoints::GetLatestHardenedCheckpoint();
    if (mapBlockIndex.count(hash))
    {
        const CBlockIndex* pindex = mapBlockIndex[hash];
        if (!chainActive.Contains(pindex))
        {
            //ppcTODO - redo this somehow
//            // checkpoint block accepted but not yet in main chain
//            LogPrintf("ResetSyncCheckpoint: SetBestChain to hardened checkpoint %s\n", hash.ToString());
//            CValidationState state;
//            if (!SetBestChain(state, mapBlockIndex[hash]))
//            {
//                return error("ResetSyncCheckpoint: SetBestChain failed for hardened checkpoint %s", hash.ToString());
//            }
        }
    }
    else
    {
        // checkpoint block not yet accepted
        hashPendingCheckpoint = hash;
        checkpointMessagePending.SetNull();
        LogPrintf("ResetSyncCheckpoint: pending for sync-checkpoint %s\n", hashPendingCheckpoint.ToString());
    }

    if (!WriteSyncCheckpoint(mapBlockIndex.count(hash) && chainActive.Contains(mapBlockIndex[hash]) ? hash : Params().GetConsensus().hashGenesisBlock))
        return error("ResetSyncCheckpoint: failed to write sync checkpoint %s", hash.ToString());
    LogPrintf("ResetSyncCheckpoint: sync-checkpoint reset to %s\n", hashSyncCheckpoint.ToString());
    return true;
}

void AskForPendingSyncCheckpoint(CNode* pfrom)
{
    LOCK(cs_hashSyncCheckpoint);
    if (pfrom && hashPendingCheckpoint != uint256() && (!mapBlockIndex.count(hashPendingCheckpoint)))
        pfrom->AskFor(CInv(MSG_BLOCK, hashPendingCheckpoint));
}

// Verify sync checkpoint master pubkey and reset sync checkpoint if changed
bool CheckCheckpointPubKey()
{
    std::string strPubKey = "";
    std::string strMasterPubKey = Params().NetworkIDString() == CBaseChainParams::TESTNET ? CSyncCheckpoint::strTestPubKey : CSyncCheckpoint::strMainPubKey;
    if (!pblocktree->ReadCheckpointPubKey(strPubKey) || strPubKey != strMasterPubKey)
    {
        // write checkpoint master key to db
        if (!pblocktree->WriteCheckpointPubKey(strMasterPubKey))
            return error("CheckCheckpointPubKey() : failed to write new checkpoint master key to db");
        if (!pblocktree->Sync())
            return error("CheckCheckpointPubKey() : failed to commit new checkpoint master key to db");
        if (!ResetSyncCheckpoint())
            return error("CheckCheckpointPubKey() : failed to reset sync-checkpoint");
    }
    return true;
}

bool SetCheckpointPrivKey(std::string strPrivKey)
{
    // Test signing a sync-checkpoint with genesis block
    CSyncCheckpoint checkpoint;
    checkpoint.hashCheckpoint = Params().GetConsensus().hashGenesisBlock;
    CDataStream sMsg(SER_NETWORK, PROTOCOL_VERSION);
    sMsg << (CUnsignedSyncCheckpoint)checkpoint;
    checkpoint.vchMsg = std::vector<unsigned char>(sMsg.begin(), sMsg.end());

    CBitcoinSecret vchSecret;
    if (!vchSecret.SetString(strPrivKey))
        return error("SetCheckpointPrivKey(): Invalid private key encoding");
    CKey key = vchSecret.GetKey();
    if (!key.IsValid())
        return error("SetCheckpointPrivKey(): Private key outside allowed range");
    if (!key.Sign(Hash(checkpoint.vchMsg.begin(), checkpoint.vchMsg.end()), checkpoint.vchSig))
        return error("SetCheckpointPrivKey(): Unable to sign checkpoint, check private key?");

    // Test signing successful, proceed
    CSyncCheckpoint::strMasterPrivKey = strPrivKey;
    return true;
}

bool SendSyncCheckpoint(uint256 hashCheckpoint)
{
    CSyncCheckpoint checkpoint;
    checkpoint.hashCheckpoint = hashCheckpoint;
    CDataStream sMsg(SER_NETWORK, PROTOCOL_VERSION);
    sMsg << (CUnsignedSyncCheckpoint)checkpoint;
    checkpoint.vchMsg = std::vector<unsigned char>(sMsg.begin(), sMsg.end());

    if (CSyncCheckpoint::strMasterPrivKey.empty())
        return error("SendSyncCheckpoint: Checkpoint master key unavailable.");

    CBitcoinSecret vchSecret;
    if (!vchSecret.SetString(CSyncCheckpoint::strMasterPrivKey))
        return error("SendSyncCheckpoint(): Invalid private key encoding");
    CKey key = vchSecret.GetKey();
    if (!key.IsValid())
        return error("SendSyncCheckpoint(): Private key outside allowed range");
    if (!key.Sign(Hash(checkpoint.vchMsg.begin(), checkpoint.vchMsg.end()), checkpoint.vchSig))
        return error("SendSyncCheckpoint(): Unable to sign checkpoint, check private key?");

    if(!checkpoint.ProcessSyncCheckpoint(NULL))
    {
        LogPrintf("WARNING: SendSyncCheckpoint: Failed to process checkpoint.\n");
        return false;
    }

    // Relay checkpoint
    g_connman->ForEachNode([&checkpoint](CNode* pnode) {
        checkpoint.RelayTo(pnode);
    });
    return true;
}

// Is the sync-checkpoint too old?
bool IsSyncCheckpointTooOld(unsigned int nSeconds)
{
    LOCK(cs_hashSyncCheckpoint);
    // sync-checkpoint should always be accepted block
    assert(mapBlockIndex.count(hashSyncCheckpoint));
    const CBlockIndex* pindexSync = mapBlockIndex[hashSyncCheckpoint];
    return (pindexSync->GetBlockTime() + nSeconds < GetAdjustedTime());
}

// peercoin: verify signature of sync-checkpoint message
bool CSyncCheckpoint::CheckSignature()
{
    std::string strMasterPubKey = Params().NetworkIDString() == CBaseChainParams::TESTNET ? CSyncCheckpoint::strTestPubKey : CSyncCheckpoint::strMainPubKey;
    CPubKey key(ParseHex(strMasterPubKey));
    if (!key.Verify(Hash(vchMsg.begin(), vchMsg.end()), vchSig))
        return error("CSyncCheckpoint::CheckSignature() : verify signature failed");

    // Now unserialize the data
    CDataStream sMsg(vchMsg, SER_NETWORK, PROTOCOL_VERSION);
    sMsg >> *(CUnsignedSyncCheckpoint*)this;
    return true;
}

// peercoin: process synchronized checkpoint
bool CSyncCheckpoint::ProcessSyncCheckpoint(CNode* pfrom)
{
    if (!CheckSignature())
        return false;

    LOCK(cs_hashSyncCheckpoint);
    if (!mapBlockIndex.count(hashCheckpoint))
    {
        // We haven't received the checkpoint chain, keep the checkpoint as pending
        hashPendingCheckpoint = hashCheckpoint;
        checkpointMessagePending = *this;
        LogPrintf("ProcessSyncCheckpoint: pending for sync-checkpoint %s\n", hashCheckpoint.ToString());
        // Ask this guy to fill in what we're missing
        if (pfrom)
        {
            //ppcTODO - redo this somehow
//            pfrom->PushGetBlocks(pindexBest, hashCheckpoint);
//            // ask directly as well in case rejected earlier by duplicate
//            // proof-of-stake because getblocks may not get it this time
//            pfrom->AskFor(CInv(MSG_BLOCK, mapOrphanBlocks.count(hashCheckpoint)? WantedByOrphan(mapOrphanBlocks[hashCheckpoint]) : hashCheckpoint));
        }
        return false;
    }

    if (!ValidateSyncCheckpoint(hashCheckpoint))
        return false;

    CBlockIndex* pindexCheckpoint = mapBlockIndex[hashCheckpoint];
    if (IsSyncCheckpointEnforced() && !chainActive.Contains(pindexCheckpoint))
    {
        //ppcTODO - redo this somehow
//        // checkpoint chain received but not yet main chain
//        CValidationState state;
//        if (!SetBestChain(state, pindexCheckpoint))
//        {
//            hashInvalidCheckpoint = hashCheckpoint;
//            return error("ProcessSyncCheckpoint: SetBestChain failed for sync checkpoint %s", hashCheckpoint.ToString());
//        }
    }

    if (!WriteSyncCheckpoint(hashCheckpoint))
        return error("ProcessSyncCheckpoint(): failed to write sync checkpoint %s", hashCheckpoint.ToString());
    checkpointMessage = *this;
    hashPendingCheckpoint = uint256();
    checkpointMessagePending.SetNull();
    LogPrintf("ProcessSyncCheckpoint: sync-checkpoint at %s\n", hashCheckpoint.ToString());
    return true;
}
