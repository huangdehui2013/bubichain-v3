﻿/*
Copyright Bubi Technologies Co., Ltd. 2017 All Rights Reserved.
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <sstream>

#include <utils/utils.h>
#include <common/storage.h>
#include <common/pb2json.h>
#include <glue/glue_manager.h>
#include "ledger_manager.h"
#include "ledger_frm.h"
#include "ledgercontext_manager.h"
#include "contract_manager.h"

namespace bubi {
#define COUNT_PER_PARTITION 1000000
	LedgerFrm::LedgerFrm() {
		lpledger_context_ = NULL;
		enabled_ = false;
		apply_time_ = -1;
		total_fee_ = 0;
		total_real_fee_ = 0;
	}

	
	LedgerFrm::~LedgerFrm() {
	}

	bool LedgerFrm::LoadFromDb(int64_t ledger_seq) {

		bubi::KeyValueDb *db = bubi::Storage::Instance().ledger_db();
		std::string ledger_header;
		int32_t ret = db->Get(ComposePrefix(General::LEDGER_PREFIX, ledger_seq), ledger_header);
		if (ret > 0) {
			ledger_.mutable_header()->ParseFromString(ledger_header);
			return true;
		}
		else if (ret < 0) {
			LOG_ERROR("Get ledger failed, error desc(%s)", db->error_desc().c_str());
			return false;
		}

		return false;
	}


	bool LedgerFrm::AddToDb(WRITE_BATCH &batch) {
		KeyValueDb *db = Storage::Instance().ledger_db();

		batch.Put(bubi::General::KEY_LEDGER_SEQ, utils::String::ToString(ledger_.header().seq()));
		batch.Put(ComposePrefix(General::LEDGER_PREFIX, ledger_.header().seq()), ledger_.header().SerializeAsString());
		
		protocol::EntryList list;
		for (size_t i = 0; i < apply_tx_frms_.size(); i++) {
			const TransactionFrm::pointer ptr = apply_tx_frms_[i];

			protocol::TransactionEnvStore env_store;
			*env_store.mutable_transaction_env() = apply_tx_frms_[i]->GetTransactionEnv();
			env_store.set_ledger_seq(ledger_.header().seq());
			env_store.set_close_time(ledger_.header().close_time());
			env_store.set_error_code(ptr->GetResult().code());
			env_store.set_error_desc(ptr->GetResult().desc());

			batch.Put(ComposePrefix(General::TRANSACTION_PREFIX, ptr->GetContentHash()), env_store.SerializeAsString());
			list.add_entry(ptr->GetContentHash());

			//a transaction success so the transactions trigger by it can store
			if (ptr->GetResult().code() == protocol::ERRCODE_SUCCESS)
				for (size_t j = 0; j < ptr->instructions_.size(); j++){
					protocol::TransactionEnvStore env_sto = ptr->instructions_.at(j);
					env_sto.set_ledger_seq(ledger_.header().seq());
					env_sto.set_close_time(ledger_.header().close_time());
					std::string hash = HashWrapper::Crypto(env_sto.transaction_env().transaction().SerializeAsString());
					batch.Put(ComposePrefix(General::TRANSACTION_PREFIX, hash), env_sto.SerializeAsString());
					list.add_entry(hash);
				}
		}

		batch.Put(ComposePrefix(General::LEDGER_TRANSACTION_PREFIX, ledger_.header().seq()), list.SerializeAsString());

		//save the last tx hash, temporary
		if (list.entry_size() > 0) {
			protocol::EntryList new_last_hashs;
			if (list.entry_size() < General::LAST_TX_HASHS_LIMIT) {
				std::string str_last_hashs;
				int32_t ncount = db->Get(General::LAST_TX_HASHS, str_last_hashs);
				if (ncount < 0) {
					LOG_ERROR("Load last tx hash failed, error desc(%s)", db->error_desc().c_str());
				}

				protocol::EntryList exist_hashs;
				if (ncount > 0 && !exist_hashs.ParseFromString(str_last_hashs)) {
					LOG_ERROR("Parse from string failed");
				}

				for (int32_t i = list.entry_size() - 1; i >= 0; i--) {
					*new_last_hashs.add_entry() = list.entry(i);
				}

				for (int32_t i = 0; 
					i < exist_hashs.entry_size() && new_last_hashs.entry_size() < General::LAST_TX_HASHS_LIMIT;
					i++) { 
					*new_last_hashs.add_entry() = exist_hashs.entry(i);
				}
			} else{
				for (int32_t i = list.entry_size() - 1; i >= list.entry_size() - General::LAST_TX_HASHS_LIMIT; i--) {
					*new_last_hashs.add_entry() = list.entry(i);
				}
			}

			batch.Put(General::LAST_TX_HASHS, new_last_hashs.SerializeAsString());
		}

		if (!db->WriteBatch(batch)){
			BUBI_EXIT("Write ledger and transaction failed(%s)", db->error_desc().c_str());
		}
		return true;
	}

	bool LedgerFrm::Cancel() {
		enabled_ = false;
		return true;
	}

	bool LedgerFrm::Apply(const protocol::ConsensusValue& request,
		LedgerContext *ledger_context,
		int64_t tx_time_out,
		int32_t &tx_time_out_index) {

		int64_t start_time = utils::Timestamp::HighResolution();
		lpledger_context_ = ledger_context;
		enabled_ = true;
		value_ = std::make_shared<protocol::ConsensusValue>(request);
		uint32_t success_count = 0;
		total_fee_= 0;
		total_real_fee_ = 0;
		environment_ = std::make_shared<Environment>(nullptr);

		for (int i = 0; i < request.txset().txs_size() && enabled_; i++) {
			auto txproto = request.txset().txs(i);
			
			TransactionFrm::pointer tx_frm = std::make_shared<TransactionFrm>(txproto);

			if (!tx_frm->ValidForApply(environment_)){
				dropped_tx_frms_.push_back(tx_frm);
				continue;
			}

			//pay fee
			if (!tx_frm->PayFee(environment_, total_fee_)) {
				dropped_tx_frms_.push_back(tx_frm);
				continue;
			}

			ledger_context->transaction_stack_.push_back(tx_frm);
			tx_frm->NonceIncrease(this, environment_);

			int64_t time_start = utils::Timestamp::HighResolution();
			tx_frm->SetMaxEndTime(time_start + tx_time_out);

			bool ret = tx_frm->Apply(this, environment_);
			int64_t time_use = utils::Timestamp::HighResolution() - time_start;

			//caculate byte fee ,do not store when fee not enough 
			if (tx_time_out > 0 && time_use > tx_time_out ) { //special treatment, return false
				LOG_ERROR("transaction(%s) apply failed. %s, time out(" FMT_I64 "ms > " FMT_I64 "ms)",
					utils::String::BinToHexString(tx_frm->GetContentHash()).c_str(), tx_frm->GetResult().desc().c_str(),
					time_use / utils::MICRO_UNITS_PER_MILLI, tx_time_out / utils::MICRO_UNITS_PER_MILLI);
				tx_time_out_index = i;
				return false;
			} else {
				if (!ret ) {
					LOG_ERROR("transaction(%s) apply failed. %s",
						utils::String::BinToHexString(tx_frm->GetContentHash()).c_str(), tx_frm->GetResult().desc().c_str());
					tx_time_out_index = i;
				}
				else {
					tx_frm->environment_->Commit();
				}
			}

			total_real_fee_ += tx_frm->GetRealFee();
			apply_tx_frms_.push_back(tx_frm);			
			ledger_.add_transaction_envs()->CopyFrom(txproto);
			ledger_context->transaction_stack_.pop_back();
		}
		AllocateFee();
		apply_time_ = utils::Timestamp::HighResolution() - start_time;
		return true;
	}

	bool LedgerFrm::CheckValidation() {
		return true;
	}

	Json::Value LedgerFrm::ToJson() {
		return bubi::Proto2Json(ledger_);
	}

	protocol::Ledger &LedgerFrm::ProtoLedger() {
		return ledger_;
	}

	bool LedgerFrm::Commit(KVTrie* trie, int64_t& new_count, int64_t& change_count) {
		auto batch = trie->batch_;
		for (auto it = environment_->entries_.begin(); it != environment_->entries_.end(); it++){
			std::shared_ptr<AccountFrm> account = it->second;
			account->UpdateHash(batch);
			std::string ss = account->Serializer();
            std::string index = DecodeAddress(it->first);
			bool is_new = trie->Set(index, ss);
			if (is_new){
				new_count++;
			}
			else{
				change_count++;
			}
		}
		return true;
	}

	bool LedgerFrm::AllocateFee() {
		LOG_INFO("Ledger total_fee(" FMT_I64 ")", total_fee_);
		if (total_fee_==0){
			return true;
		}
		protocol::ValidatorSet set;
		int64_t seq = ledger_.header().seq() - 1;
		if (!LedgerManager::Instance().GetValidators(seq, set))	{
			LOG_ERROR("Get validator failed of ledger seq(" FMT_I64 ")", seq);
			return false;
		}
		if (set.validators_size() == 0) {
			LOG_ERROR("Get validator failed of ledger seq(" FMT_I64 "),validator number is 0", seq);
			return false;
		}
		int64_t tfee = total_fee_;
		std::shared_ptr<AccountFrm> random_account;
		int64_t random_index = seq % set.validators_size();
		int64_t fee = tfee / set.validators_size();
		for (size_t i = 0; i < set.validators_size(); i++) {
			std::shared_ptr<AccountFrm> account;
			if (!environment_->GetEntry(set.validators(i), account)) {
				account =CreatBookKeeperAccount(set.validators(i));
			}
			if (random_index == i)
				random_account = account;
			tfee -= fee;
			protocol::Account &proto_account = account->GetProtoAccount();
			proto_account.set_balance(proto_account.balance() + fee);
		}
		protocol::Account &proto_account = random_account->GetProtoAccount();
		proto_account.set_balance(proto_account.balance() + tfee);
		return true;
	}
	AccountFrm::pointer LedgerFrm::CreatBookKeeperAccount(const std::string& account_address) {
		protocol::Account acc;
		acc.set_address(account_address);
		acc.set_nonce(0);
		acc.set_balance(0); //100000000000000000
		AccountFrm::pointer acc_frm = std::make_shared<AccountFrm>(acc);
		acc_frm->SetProtoMasterWeight(1);
		acc_frm->SetProtoTxThreshold(1);
		environment_->AddEntry(acc_frm->GetAccountAddress(), acc_frm);
		LOG_INFO("Add bookeeper account(%)", account_address);
		return acc_frm;
	}
	
	bool LedgerFrm::GetVotedFee(protocol::FeeConfig& fee_config) {
		
		/*//for test format
		for (auto it = contracts_output_.begin(); it != contracts_output_.end();it++)
		{
			std::string key = it->first;
			Json::Value v= it->second;
			LOG_INFO("victory fee key(%s)--%s",key.c_str(),v.toStyledString().c_str());
		}*/
		/*victory fee format
		[
		null,
		{
			"count" : 3,
		    "enroll_id" : "xx1",
			"fee_type" : 1,
			"price" : 5
		}
		]
		*/
		std::string dest_address = Configure::Instance().ledger_configure_.fees_vote_account_;
		if (dest_address.empty()){
			LOG_ERROR("Ledger config fees_vote_account not set");
			return false;
		}
		auto iter =contracts_output_.find(dest_address);
		if (iter == contracts_output_.end()){
			return false;
		}
		bool change = false;
		Json::Value victor_fees =contracts_output_[dest_address];
		for (uint32_t i = 0; i < victor_fees.size();i++) {
			const Json::Value &item = victor_fees[i];
			int fee_type =item["fee_type"].asInt();
			switch ((protocol::FeeConfig_Type)fee_type)
			{
			case protocol::FeeConfig_Type_UNKNOWN:
				LOG_ERROR("FeeConfig type error");
				break;
			case protocol::FeeConfig_Type_BYTE_FEE:
				if (fee_config.byte_fee() != item["price"].asInt64()){
					fee_config.set_byte_fee(item["price"].asInt64());
					change = true;
				}
				break;
			case protocol::FeeConfig_Type_BASE_RESERVE_FEE:
				if (fee_config.base_reserve() != item["price"].asInt64()){
					fee_config.set_base_reserve(item["price"].asInt64());
					change = true;
				}
				break;
			case protocol::FeeConfig_Type_CREATE_ACCOUNT_FEE:
				if (fee_config.create_account_fee() != item["price"].asInt64()){
					fee_config.set_create_account_fee(item["price"].asInt64());
					change = true;
				}
				break;
			case protocol::FeeConfig_Type_ISSUE_ASSET_FEE:
				if (fee_config.issue_asset_fee() != item["price"].asInt64()){
					fee_config.set_issue_asset_fee(item["price"].asInt64());
					change = true;
				}
				break;
			case protocol::FeeConfig_Type_PAYMENT_FEE:
				if (fee_config.pay_fee() != item["price"].asInt64()){
					fee_config.set_pay_fee(item["price"].asInt64());
					change = true;
				}
				break;
			case protocol::FeeConfig_Type_SET_METADATA_FEE:
				if (fee_config.set_metadata_fee() != item["price"].asInt64()){
					fee_config.set_set_metadata_fee(item["price"].asInt64());
					change = true;
				}
				break;
			case protocol::FeeConfig_Type_SET_SIGNER_WEIGHT_FEE:
				if (fee_config.set_sigure_weight_fee() != item["price"].asInt64()){
					fee_config.set_set_sigure_weight_fee(item["price"].asInt64());
					change = true;
				}
				break;
			case protocol::FeeConfig_Type_SET_THRESHOLD_FEE:
				if (fee_config.set_threshold_fee() != item["price"].asInt64()){
					fee_config.set_set_threshold_fee(item["price"].asInt64());
					change = true;
				}
				break;
			case protocol::FeeConfig_Type_PAY_COIN_FEE:
				if (fee_config.pay_coin_fee() != item["price"].asInt64()){
					fee_config.set_pay_fee(item["price"].asInt64());
					change = true;
				}
				break;
			default:
				LOG_ERROR("FeeConfig type error");
				break;
			}
		}		
		return change;
	}
}
