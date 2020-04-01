// Copyright (c) 2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "zerocoin_verify.h"

#include "chainparams.h"
#include "consensus/consensus.h"
#include "guiinterface.h"        // for ui_interface
#include "init.h"                // for ShutdownRequested()
#include "main.h"
#include "script/interpreter.h"
#include "spork.h"               // for sporkManager
#include "txdb.h"
#include "utilmoneystr.h"        // for FormatMoney

bool CheckZerocoinSpend(const CTransaction& tx, bool fVerifySignature, CValidationState& state)
{
    //max needed non-mint outputs should be 2 - one for redemption address and a possible 2nd for change
    if (tx.vout.size() > 2) {
        int outs = 0;
        for (const CTxOut& out : tx.vout) {
            if (out.IsZerocoinMint())
                continue;
            outs++;
        }
        if (outs > 2 && !tx.IsCoinStake())
            return state.DoS(100, error("CheckZerocoinSpend(): over two non-mint outputs in a zerocoinspend transaction"));
    }

    //compute the txout hash that is used for the zerocoinspend signatures
    CMutableTransaction txTemp;
    for (const CTxOut& out : tx.vout) {
        txTemp.vout.push_back(out);
    }
    uint256 hashTxOut = txTemp.GetHash();

    bool fValidated = false;
    const Consensus::Params& consensus = Params().GetConsensus();
    std::set<CBigNum> serials;
    CAmount nTotalRedeemed = 0;
    for (const CTxIn& txin : tx.vin) {

        //only check txin that is a zcspend
        bool isPublicSpend = txin.IsZerocoinPublicSpend();
        if (!txin.IsZerocoinSpend() && !isPublicSpend)
            continue;

        libzerocoin::CoinSpend newSpend;
        CTxOut prevOut;
        if (isPublicSpend) {
            if(!GetOutput(txin.prevout.hash, txin.prevout.n, state, prevOut)){
                return state.DoS(100, error("CheckZerocoinSpend(): public zerocoin spend prev output not found, prevTx %s, index %d", txin.prevout.hash.GetHex(), txin.prevout.n));
            }
            libzerocoin::ZerocoinParams* params = consensus.Zerocoin_Params(false);
            PublicCoinSpend publicSpend(params);
            if (!ZPIVModule::parseCoinSpend(txin, tx, prevOut, publicSpend)){
                return state.DoS(100, error("CheckZerocoinSpend(): public zerocoin spend parse failed"));
            }
            newSpend = publicSpend;
        } else {
            newSpend = TxInToZerocoinSpend(txin);
        }

        //check that the denomination is valid
        if (newSpend.getDenomination() == libzerocoin::ZQ_ERROR)
            return state.DoS(100, error("Zerocoinspend does not have the correct denomination"));

        //check that denomination is what it claims to be in nSequence
        if (newSpend.getDenomination() != txin.nSequence)
            return state.DoS(100, error("Zerocoinspend nSequence denomination does not match CoinSpend"));

        //make sure the txout has not changed
        if (newSpend.getTxOutHash() != hashTxOut)
            return state.DoS(100, error("Zerocoinspend does not use the same txout that was used in the SoK"));

        if (isPublicSpend) {
            libzerocoin::ZerocoinParams* params = consensus.Zerocoin_Params(false);
            PublicCoinSpend ret(params);
            if (!ZPIVModule::validateInput(txin, prevOut, tx, ret)){
                return state.DoS(100, error("CheckZerocoinSpend(): public zerocoin spend did not verify"));
            }
        }

        if (serials.count(newSpend.getCoinSerialNumber()))
            return state.DoS(100, error("Zerocoinspend serial is used twice in the same tx"));
        serials.insert(newSpend.getCoinSerialNumber());

        //make sure that there is no over redemption of coins
        nTotalRedeemed += libzerocoin::ZerocoinDenominationToAmount(newSpend.getDenomination());
        fValidated = true;
    }

    if (!tx.IsCoinStake() && nTotalRedeemed < tx.GetValueOut()) {
        LogPrintf("redeemed = %s , spend = %s \n", FormatMoney(nTotalRedeemed), FormatMoney(tx.GetValueOut()));
        return state.DoS(100, error("Transaction spend more than was redeemed in zerocoins"));
    }

    return fValidated;
}

bool CheckPublicCoinSpendEnforced(int blockHeight, bool isPublicSpend)
{
    if (blockHeight >= Params().GetConsensus().height_start_ZC_PublicSpends) {
        // reject old coin spend
        if (!isPublicSpend) {
            return error("%s: failed to add block with older zc spend version", __func__);
        }

    } else {
        if (isPublicSpend) {
            return error("%s: failed to add block, public spend enforcement not activated", __func__);
        }
    }
    return true;
}

int CurrentPublicCoinSpendVersion() {
    return sporkManager.IsSporkActive(SPORK_18_ZEROCOIN_PUBLICSPEND_V4) ? 4 : 3;
}

bool CheckPublicCoinSpendVersion(int version) {
    return version == CurrentPublicCoinSpendVersion();
}

bool ContextualCheckZerocoinSpend(const CTransaction& tx, const libzerocoin::CoinSpend* spend, int nHeight, const uint256& hashBlock)
{
    if(!ContextualCheckZerocoinSpendNoSerialCheck(tx, spend, nHeight, hashBlock)){
        return false;
    }

    //Reject serial's that are already in the blockchain
    int nHeightTx = 0;
    if (IsSerialInBlockchain(spend->getCoinSerialNumber(), nHeightTx))
        return error("%s : zPIV spend with serial %s is already in block %d\n", __func__,
                     spend->getCoinSerialNumber().GetHex(), nHeightTx);

    return true;
}

bool ContextualCheckZerocoinSpendNoSerialCheck(const CTransaction& tx, const libzerocoin::CoinSpend* spend, int nHeight, const uint256& hashBlock)
{
    const Consensus::Params& consensus = Params().GetConsensus();
    //Check to see if the zPIV is properly signed
    if (nHeight >= consensus.height_start_ZC_SerialsV2) {
        try {
            if (!spend->HasValidSignature())
                return error("%s: V2 zPIV spend does not have a valid signature\n", __func__);
        } catch (const libzerocoin::InvalidSerialException& e) {
            return error("%s: Invalid serial detected, txid %s, in block %d\n", __func__, tx.GetHash().GetHex(), nHeight);
        }

        libzerocoin::SpendType expectedType = libzerocoin::SpendType::SPEND;
        if (tx.IsCoinStake())
            expectedType = libzerocoin::SpendType::STAKE;
        if (spend->getSpendType() != expectedType) {
            return error("%s: trying to spend zPIV without the correct spend type. txid=%s\n", __func__,
                         tx.GetHash().GetHex());
        }
    }
    return true;
}

bool RecalculatePIVSupply(int nHeightStart, bool fSkipZpiv)
{
    const Consensus::Params& consensus = Params().GetConsensus();
    const int chainHeight = chainActive.Height();
    if (nHeightStart > chainHeight)
        return false;

    CBlockIndex* pindex = chainActive[nHeightStart];
    CAmount nSupplyPrev = pindex->pprev->nMoneySupply;
    if (nHeightStart == consensus.height_start_ZC)
        nSupplyPrev = CAmount(5449796547496199);

    uiInterface.ShowProgress(_("Recalculating PIV supply..."), 0);
    while (true) {
        if (pindex->nHeight % 1000 == 0) {
            LogPrintf("%s : block %d...\n", __func__, pindex->nHeight);
            int percent = std::max(1, std::min(99, (int)((double)((pindex->nHeight - nHeightStart) * 100) / (chainHeight - nHeightStart))));
            uiInterface.ShowProgress(_("Recalculating PIV supply..."), percent);
        }

        CBlock block;
        assert(ReadBlockFromDisk(block, pindex));

        CAmount nValueIn = 0;
        CAmount nValueOut = 0;
        for (const CTransaction& tx : block.vtx) {
            for (unsigned int i = 0; i < tx.vin.size(); i++) {
                if (tx.IsCoinBase())
                    break;

                if (tx.vin[i].IsZerocoinSpend()) {
                    nValueIn += tx.vin[i].nSequence * COIN;
                    continue;
                }

                COutPoint prevout = tx.vin[i].prevout;
                CTransaction txPrev;
                uint256 hashBlock;
                assert(GetTransaction(prevout.hash, txPrev, hashBlock, true));
                nValueIn += txPrev.vout[prevout.n].nValue;
            }

            for (unsigned int i = 0; i < tx.vout.size(); i++) {
                if (i == 0 && tx.IsCoinStake())
                    continue;

                nValueOut += tx.vout[i].nValue;
            }
        }

        // Rewrite money supply
        pindex->nMoneySupply = nSupplyPrev + nValueOut - nValueIn;
        nSupplyPrev = pindex->nMoneySupply;

        // Rewrite zpiv supply too
        if (!fSkipZpiv && pindex->nHeight >= consensus.height_start_ZC) {
            UpdateZPIVSupply(block, pindex, true);
        }

        assert(pblocktree->WriteBlockIndex(CDiskBlockIndex(pindex)));

        // Stop if shutdown was requested
        if (ShutdownRequested()) return false;

        if (pindex->nHeight < chainHeight)
            pindex = chainActive.Next(pindex);
        else
            break;
    }
    uiInterface.ShowProgress("", 100);
    return true;
}

bool UpdateZPIVSupply(const CBlock& block, CBlockIndex* pindex, bool fJustCheck)
{
    const Consensus::Params& consensus = Params().GetConsensus();
    if (pindex->nHeight < consensus.height_start_ZC)
        return true;

    //Reset the supply to previous block
    pindex->mapZerocoinSupply = pindex->pprev->mapZerocoinSupply;

    //Add mints to zPIV supply (mints are forever disabled after last checkpoint)
    if (pindex->nHeight < consensus.height_last_ZC_AccumCheckpoint) {
        std::list<CZerocoinMint> listMints;
        std::set<uint256> setAddedToWallet;
        BlockToZerocoinMintList(block, listMints, true);
        for (const auto& m : listMints) {
            pindex->mapZerocoinSupply.at(m.GetDenomination())++;
            //Remove any of our own mints from the mintpool
            if (!fJustCheck && pwalletMain) {
                if (pwalletMain->IsMyMint(m.GetValue())) {
                    pwalletMain->UpdateMint(m.GetValue(), pindex->nHeight, m.GetTxHash(), m.GetDenomination());
                    // Add the transaction to the wallet
                    for (auto& tx : block.vtx) {
                        uint256 txid = tx.GetHash();
                        if (setAddedToWallet.count(txid))
                            continue;
                        if (txid == m.GetTxHash()) {
                            CWalletTx wtx(pwalletMain, tx);
                            wtx.nTimeReceived = block.GetBlockTime();
                            wtx.SetMerkleBranch(block);
                            pwalletMain->AddToWallet(wtx, false, nullptr);
                            setAddedToWallet.insert(txid);
                        }
                    }
                }
            }
        }
    }

    //Remove spends from zPIV supply
    std::list<libzerocoin::CoinDenomination> listDenomsSpent = ZerocoinSpendListFromBlock(block, true);
    for (const auto& denom : listDenomsSpent) {
        pindex->mapZerocoinSupply.at(denom)--;
        // zerocoin failsafe
        if (pindex->mapZerocoinSupply.at(denom) < 0)
            return error("Block contains zerocoins that spend more than are in the available supply to spend");
    }

    for (const auto& denom : libzerocoin::zerocoinDenomList)
        LogPrint("zero", "%s coins for denomination %d pubcoin %s\n", __func__, denom, pindex->mapZerocoinSupply.at(denom));

    return true;
}