#include <nano/lib/rep_weights.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/utility.hpp>
#include <nano/lib/work.hpp>
#include <nano/secure/blockstore.hpp>
#include <nano/secure/ledger.hpp>

namespace
{
/**
 * Roll back the visited block
 */
class rollback_visitor : public nano::block_visitor
{
public:
	rollback_visitor (nano::write_transaction const & transaction_a, nano::ledger & ledger_a, std::vector<std::shared_ptr<nano::block>> & list_a) :
	transaction (transaction_a),
	ledger (ledger_a),
	list (list_a)
	{
	}
	virtual ~rollback_visitor () = default;
	void send_block (nano::send_block const & block_a) override
	{
		auto hash (block_a.hash ());
		nano::pending_info pending;
		nano::pending_key key (block_a.hashables.destination, hash);
		while (!error && ledger.store.pending_get (transaction, key, pending))
		{
			error = ledger.rollback (transaction, ledger.latest (transaction, block_a.hashables.destination), list);
		}
		if (!error)
		{
			auto state (ledger.account_state (transaction, pending.source));
			assert (!state.head ().is_zero ());
			ledger.store.pending_del (transaction, key);
			ledger.rep_weights.representation_add (state.rep (), pending.amount.number ());
			nano::account_info new_info (block_a.hashables.previous, state.rep (), state.open ());
			ledger.change_latest (transaction, pending.source, state, new_info, nano::epoch::epoch_0);
			ledger.store.block_del (transaction, hash);
			ledger.store.frontier_del (transaction, hash);
			ledger.store.frontier_put (transaction, block_a.hashables.previous, pending.source);
			ledger.store.block_successor_clear (transaction, block_a.hashables.previous);
			ledger.stats.inc (nano::stat::type::rollback, nano::stat::detail::send);
		}
	}
	void receive_block (nano::receive_block const & block_a) override
	{
		auto hash (block_a.hash ());
		auto amount (ledger.amount (transaction, block_a.hashables.source));
		auto destination_account (ledger.account (transaction, hash));
		auto source_account (ledger.account (transaction, block_a.hashables.source));
		nano::account_info info;
		auto state (ledger.account_state (transaction, destination_account));
		assert (!state.head ().is_zero ());
		ledger.rep_weights.representation_add (state.rep (), 0 - amount);
		nano::account_info new_info (block_a.hashables.previous, state.rep (), info.open_block);
		ledger.change_latest (transaction, destination_account, state, new_info, nano::epoch::epoch_0);
		ledger.store.block_del (transaction, hash);
		ledger.store.pending_put (transaction, nano::pending_key (destination_account, block_a.hashables.source), { source_account, amount, nano::epoch::epoch_0 });
		ledger.store.frontier_del (transaction, hash);
		ledger.store.frontier_put (transaction, block_a.hashables.previous, destination_account);
		ledger.store.block_successor_clear (transaction, block_a.hashables.previous);
		ledger.stats.inc (nano::stat::type::rollback, nano::stat::detail::receive);
	}
	void open_block (nano::open_block const & block_a) override
	{
		auto hash (block_a.hash ());
		auto amount (ledger.amount (transaction, block_a.hashables.source));
		auto destination_account (ledger.account (transaction, hash));
		auto source_account (ledger.account (transaction, block_a.hashables.source));
		ledger.rep_weights.representation_add (block_a.representative (), 0 - amount);
		ledger.change_latest (transaction, destination_account, {}, {}, nano::epoch::epoch_0);
		ledger.store.block_del (transaction, hash);
		ledger.store.pending_put (transaction, nano::pending_key (destination_account, block_a.hashables.source), { source_account, amount, nano::epoch::epoch_0 });
		ledger.store.frontier_del (transaction, hash);
		ledger.stats.inc (nano::stat::type::rollback, nano::stat::detail::open);
	}
	void change_block (nano::change_block const & block_a) override
	{
		auto hash (block_a.hash ());
		auto rep_block (ledger.representative (transaction, block_a.hashables.previous));
		auto account (ledger.account (transaction, block_a.hashables.previous));
		auto state (ledger.account_state (transaction, account));
		assert (!state.head ().is_zero ());
		auto balance (ledger.balance (transaction, block_a.hashables.previous));
		auto block = ledger.store.block_get (transaction, rep_block);
		release_assert (block != nullptr);
		auto representative = block->representative ();
		ledger.rep_weights.representation_add (block_a.representative (), 0 - balance);
		ledger.rep_weights.representation_add (representative, balance);
		ledger.store.block_del (transaction, hash);
		nano::account_info new_info (block_a.hashables.previous, representative, state.open ());
		ledger.change_latest (transaction, account, state, new_info, nano::epoch::epoch_0);
		ledger.store.frontier_del (transaction, hash);
		ledger.store.frontier_put (transaction, block_a.hashables.previous, account);
		ledger.store.block_successor_clear (transaction, block_a.hashables.previous);
		ledger.stats.inc (nano::stat::type::rollback, nano::stat::detail::change);
	}
	void state_block (nano::state_block const & block_a) override
	{
		auto hash (block_a.hash ());
		nano::block_hash rep_block_hash (0);
		if (!block_a.hashables.previous.is_zero ())
		{
			rep_block_hash = ledger.representative (transaction, block_a.hashables.previous);
		}
		auto balance (ledger.balance (transaction, block_a.hashables.previous));
		auto is_send (block_a.hashables.balance < balance);
		// Add in amount delta
		ledger.rep_weights.representation_add (block_a.representative (), 0 - block_a.hashables.balance.number ());
		nano::account representative{ 0 };
		if (!rep_block_hash.is_zero ())
		{
			// Move existing representation
			auto block (ledger.store.block_get (transaction, rep_block_hash));
			assert (block != nullptr);
			representative = block->representative ();
			ledger.rep_weights.representation_add (representative, balance);
		}

		auto state (ledger.account_state (transaction, block_a.hashables.account));
		assert (!state.head ().is_zero ());

		if (is_send)
		{
			nano::pending_key key (block_a.hashables.link, hash);
			while (!error && !ledger.store.pending_exists (transaction, key))
			{
				error = ledger.rollback (transaction, ledger.latest (transaction, block_a.hashables.link), list);
			}
			ledger.store.pending_del (transaction, key);
			ledger.stats.inc (nano::stat::type::rollback, nano::stat::detail::send);
		}
		else if (!block_a.hashables.link.is_zero () && !ledger.is_epoch_link (block_a.hashables.link))
		{
			auto source_version (ledger.store.block_version (transaction, block_a.hashables.link));
			nano::pending_info pending_info (ledger.account (transaction, block_a.hashables.link), block_a.hashables.balance.number () - balance, source_version);
			ledger.store.pending_put (transaction, nano::pending_key (block_a.hashables.account, block_a.hashables.link), pending_info);
			ledger.stats.inc (nano::stat::type::rollback, nano::stat::detail::receive);
		}

		auto previous_version (ledger.store.block_version (transaction, block_a.hashables.previous));
		nano::account_info new_info (block_a.hashables.previous, representative, state.open ());
		ledger.change_latest (transaction, block_a.hashables.account, state, new_info, previous_version);

		auto previous (ledger.store.block_get (transaction, block_a.hashables.previous));
		if (previous != nullptr)
		{
			ledger.store.block_successor_clear (transaction, block_a.hashables.previous);
			if (previous->type () < nano::block_type::state)
			{
				ledger.store.frontier_put (transaction, block_a.hashables.previous, block_a.hashables.account);
			}
		}
		else
		{
			ledger.stats.inc (nano::stat::type::rollback, nano::stat::detail::open);
		}
		ledger.store.block_del (transaction, hash);
	}
	nano::write_transaction const & transaction;
	nano::ledger & ledger;
	std::vector<std::shared_ptr<nano::block>> & list;
	bool error{ false };
};

class ledger_processor : public nano::block_visitor
{
public:
	ledger_processor (nano::ledger &, nano::write_transaction const &, nano::signature_verification = nano::signature_verification::unknown);
	virtual ~ledger_processor () = default;
	void send_block (nano::send_block const &) override;
	void receive_block (nano::receive_block const &) override;
	void open_block (nano::open_block const &) override;
	void change_block (nano::change_block const &) override;
	void state_block (nano::state_block const &) override;
	void state_block_impl (nano::state_block const &);
	void epoch_block_impl (nano::state_block const &);
	nano::ledger & ledger;
	nano::write_transaction const & transaction;
	nano::signature_verification verification;
	nano::process_return result;
};

void ledger_processor::state_block (nano::state_block const & block_a)
{
	result.code = nano::process_result::progress;
	auto is_epoch_block (false);
	// Check if this is an epoch block
	if (ledger.is_epoch_link (block_a.hashables.link))
	{
		nano::amount prev_balance (0);
		if (!block_a.hashables.previous.is_zero ())
		{
			result.code = ledger.store.block_exists (transaction, block_a.hashables.previous) ? nano::process_result::progress : nano::process_result::gap_previous;
			if (result.code == nano::process_result::progress)
			{
				prev_balance = ledger.balance (transaction, block_a.hashables.previous);
			}
			else if (result.verified == nano::signature_verification::unknown)
			{
				// Check for possible regular state blocks with epoch link (send subtype)
				if (validate_message (block_a.hashables.account, block_a.hash (), block_a.signature))
				{
					// Is epoch block signed correctly
					if (validate_message (ledger.signer (block_a.link ()), block_a.hash (), block_a.signature))
					{
						result.verified = nano::signature_verification::invalid;
						result.code = nano::process_result::bad_signature;
					}
					else
					{
						result.verified = nano::signature_verification::valid_epoch;
					}
				}
				else
				{
					result.verified = nano::signature_verification::valid;
				}
			}
		}
		if (block_a.hashables.balance == prev_balance)
		{
			is_epoch_block = true;
		}
	}
	if (result.code == nano::process_result::progress)
	{
		if (is_epoch_block)
		{
			epoch_block_impl (block_a);
		}
		else
		{
			state_block_impl (block_a);
		}
	}
}

void ledger_processor::state_block_impl (nano::state_block const & block_a)
{
	auto hash (block_a.hash ());
	auto existing (ledger.store.block_exists (transaction, block_a.type (), hash));
	result.code = existing ? nano::process_result::old : nano::process_result::progress; // Have we seen this block before? (Unambiguous)
	if (result.code == nano::process_result::progress)
	{
		// Validate block if not verified outside of ledger
		if (result.verified != nano::signature_verification::valid)
		{
			result.code = validate_message (block_a.hashables.account, hash, block_a.signature) ? nano::process_result::bad_signature : nano::process_result::progress; // Is this block signed correctly (Unambiguous)
		}
		if (result.code == nano::process_result::progress)
		{
			assert (!validate_message (block_a.hashables.account, hash, block_a.signature));
			result.verified = nano::signature_verification::valid;
			result.code = block_a.hashables.account.is_zero () ? nano::process_result::opened_burn_account : nano::process_result::progress; // Is this for the burn account? (Unambiguous)
			if (result.code == nano::process_result::progress)
			{
				nano::epoch epoch (nano::epoch::epoch_0);
				auto state (ledger.account_state (transaction, block_a.hashables.account));
				result.amount = block_a.hashables.balance;
				auto is_send (false);
				if (!state.head ().is_zero ())
				{
					epoch = state.epoch ();
					// Account already exists
					result.code = block_a.hashables.previous.is_zero () ? nano::process_result::fork : nano::process_result::progress; // Has this account already been opened? (Ambigious)
					if (result.code == nano::process_result::progress)
					{
						result.code = ledger.store.block_exists (transaction, block_a.hashables.previous) ? nano::process_result::progress : nano::process_result::gap_previous; // Does the previous block exist in the ledger? (Unambigious)
						if (result.code == nano::process_result::progress)
						{
							is_send = block_a.hashables.balance < state.balance ();
							result.amount = is_send ? (state.balance ().number () - result.amount.number ()) : (result.amount.number () - state.balance ().number ());
							result.code = block_a.hashables.previous == state.head () ? nano::process_result::progress : nano::process_result::fork; // Is the previous block the account's head block? (Ambigious)
						}
					}
				}
				else
				{
					// Account does not yet exists
					result.code = block_a.previous ().is_zero () ? nano::process_result::progress : nano::process_result::gap_previous; // Does the first block in an account yield 0 for previous() ? (Unambigious)
					if (result.code == nano::process_result::progress)
					{
						result.code = !block_a.hashables.link.is_zero () ? nano::process_result::progress : nano::process_result::gap_source; // Is the first block receiving from a send ? (Unambigious)
					}
				}
				if (result.code == nano::process_result::progress)
				{
					if (!is_send)
					{
						if (!block_a.hashables.link.is_zero ())
						{
							result.code = ledger.store.source_exists (transaction, block_a.hashables.link) ? nano::process_result::progress : nano::process_result::gap_source; // Have we seen the source block already? (Harmless)
							if (result.code == nano::process_result::progress)
							{
								nano::pending_key key (block_a.hashables.account, block_a.hashables.link);
								nano::pending_info pending;
								result.code = ledger.store.pending_get (transaction, key, pending) ? nano::process_result::unreceivable : nano::process_result::progress; // Has this source already been received (Malformed)
								if (result.code == nano::process_result::progress)
								{
									result.code = result.amount == pending.amount ? nano::process_result::progress : nano::process_result::balance_mismatch;
									epoch = std::max (epoch, pending.epoch);
								}
							}
						}
						else
						{
							// If there's no link, the balance must remain the same, only the representative can change
							result.code = result.amount.is_zero () ? nano::process_result::progress : nano::process_result::balance_mismatch;
						}
					}
				}
				if (result.code == nano::process_result::progress)
				{
					ledger.stats.inc (nano::stat::type::ledger, nano::stat::detail::state_block);
					result.state_is_send = is_send;
					nano::block_sideband sideband (nano::block_type::state, block_a.hashables.account /* unused */, 0, 0 /* unused */, state.block_count () + 1, nano::seconds_since_epoch ());
					ledger.store.block_put (transaction, hash, block_a, sideband, epoch);

					if (!state.rep ().is_zero ())
					{
						// Move existing representation
						ledger.rep_weights.representation_add (state.rep (), 0 - state.balance ().number ());
					}
					// Add in amount delta
					auto block (ledger.store.block_get (transaction, hash));
					assert (block != nullptr);
					auto representative = block->representative ();
					ledger.rep_weights.representation_add (representative, block_a.hashables.balance.number ());

					if (is_send)
					{
						nano::pending_key key (block_a.hashables.link, hash);
						nano::pending_info info (block_a.hashables.account, result.amount.number (), epoch);
						ledger.store.pending_put (transaction, key, info);
					}
					else if (!block_a.hashables.link.is_zero ())
					{
						ledger.store.pending_del (transaction, nano::pending_key (block_a.hashables.account, block_a.hashables.link));
					}

					nano::account_info new_info (hash, representative, state.open ().is_zero () ? hash : state.open ());
					ledger.change_latest (transaction, block_a.hashables.account, state, new_info, epoch);
					if (!ledger.store.frontier_get (transaction, state.head ()).is_zero ())
					{
						ledger.store.frontier_del (transaction, state.head ());
					}
					// Frontier table is unnecessary for state blocks and this also prevents old blocks from being inserted on top of state blocks
					result.account = block_a.hashables.account;
				}
			}
		}
	}
}

void ledger_processor::epoch_block_impl (nano::state_block const & block_a)
{
	auto hash (block_a.hash ());
	auto existing (ledger.store.block_exists (transaction, block_a.type (), hash));
	result.code = existing ? nano::process_result::old : nano::process_result::progress; // Have we seen this block before? (Unambiguous)
	if (result.code == nano::process_result::progress)
	{
		// Validate block if not verified outside of ledger
		if (result.verified != nano::signature_verification::valid_epoch)
		{
			result.code = validate_message (ledger.signer (block_a.link ()), hash, block_a.signature) ? nano::process_result::bad_signature : nano::process_result::progress; // Is this block signed correctly (Unambiguous)
		}
		if (result.code == nano::process_result::progress)
		{
			assert (!validate_message (ledger.signer (block_a.link ()), hash, block_a.signature));
			result.verified = nano::signature_verification::valid_epoch;
			result.code = block_a.hashables.account.is_zero () ? nano::process_result::opened_burn_account : nano::process_result::progress; // Is this for the burn account? (Unambiguous)
			if (result.code == nano::process_result::progress)
			{
				auto state (ledger.account_state (transaction, block_a.hashables.account));
				if (!state.head ().is_zero ())
				{
					// Account already exists
					result.code = block_a.hashables.previous.is_zero () ? nano::process_result::fork : nano::process_result::progress; // Has this account already been opened? (Ambigious)
					if (result.code == nano::process_result::progress)
					{
						result.code = block_a.hashables.previous == state.head () ? nano::process_result::progress : nano::process_result::fork; // Is the previous block the account's head block? (Ambigious)
						if (result.code == nano::process_result::progress)
						{
							result.code = block_a.hashables.representative == state.rep () ? nano::process_result::progress : nano::process_result::representative_mismatch;
						}
					}
				}
				else
				{
					result.code = block_a.hashables.representative.is_zero () ? nano::process_result::progress : nano::process_result::representative_mismatch;
				}
				if (result.code == nano::process_result::progress)
				{
					result.code = (state.head ().is_zero () ? nano::epoch::epoch_0 : state.epoch ()) < ledger.network_params.ledger.epochs.epoch (block_a.link ()) ? nano::process_result::progress : nano::process_result::block_position;
					if (result.code == nano::process_result::progress)
					{
						result.code = block_a.hashables.balance == state.balance () ? nano::process_result::progress : nano::process_result::balance_mismatch;
						if (result.code == nano::process_result::progress)
						{
							ledger.stats.inc (nano::stat::type::ledger, nano::stat::detail::epoch_block);
							result.account = block_a.hashables.account;
							result.amount = 0;
							nano::block_sideband sideband (nano::block_type::state, block_a.hashables.account /* unused */, 0, 0 /* unused */, state.block_count () + 1, nano::seconds_since_epoch ());
							ledger.store.block_put (transaction, hash, block_a, sideband, nano::epoch::epoch_1);
							nano::account_info new_info (hash, block_a.representative (), state.open ().is_zero () ? hash : state.open ());
							ledger.change_latest (transaction, block_a.hashables.account, state, new_info, nano::epoch::epoch_1);
							if (!ledger.store.frontier_get (transaction, state.head ()).is_zero ())
							{
								ledger.store.frontier_del (transaction, state.head ());
							}
						}
					}
				}
			}
		}
	}
}

void ledger_processor::change_block (nano::change_block const & block_a)
{
	auto hash (block_a.hash ());
	auto existing (ledger.store.block_exists (transaction, block_a.type (), hash));
	result.code = existing ? nano::process_result::old : nano::process_result::progress; // Have we seen this block before? (Harmless)
	if (result.code == nano::process_result::progress)
	{
		auto previous (ledger.store.block_get (transaction, block_a.hashables.previous));
		result.code = previous != nullptr ? nano::process_result::progress : nano::process_result::gap_previous; // Have we seen the previous block already? (Harmless)
		if (result.code == nano::process_result::progress)
		{
			result.code = block_a.valid_predecessor (*previous) ? nano::process_result::progress : nano::process_result::block_position;
			if (result.code == nano::process_result::progress)
			{
				auto account (ledger.store.frontier_get (transaction, block_a.hashables.previous));
				result.code = account.is_zero () ? nano::process_result::fork : nano::process_result::progress;
				if (result.code == nano::process_result::progress)
				{
					auto state (ledger.account_state (transaction, account));
					assert (!state.head ().is_zero ());
					assert (state.head () == block_a.hashables.previous);
					// Validate block if not verified outside of ledger
					if (result.verified != nano::signature_verification::valid)
					{
						result.code = validate_message (account, hash, block_a.signature) ? nano::process_result::bad_signature : nano::process_result::progress; // Is this block signed correctly (Malformed)
					}
					if (result.code == nano::process_result::progress)
					{
						assert (!validate_message (account, hash, block_a.signature));
						result.verified = nano::signature_verification::valid;
						nano::block_sideband sideband (nano::block_type::change, account, 0, state.balance (), state.block_count () + 1, nano::seconds_since_epoch ());
						ledger.store.block_put (transaction, hash, block_a, sideband);
						auto balance (ledger.balance (transaction, block_a.hashables.previous));
						ledger.rep_weights.representation_add (block_a.representative (), balance);
						ledger.rep_weights.representation_add (state.rep (), 0 - balance);
						nano::account_info new_info (hash, block_a.representative (), state.open ());
						ledger.change_latest (transaction, account, state, new_info, nano::epoch::epoch_0);
						ledger.store.frontier_del (transaction, block_a.hashables.previous);
						ledger.store.frontier_put (transaction, hash, account);
						result.account = account;
						result.amount = 0;
						ledger.stats.inc (nano::stat::type::ledger, nano::stat::detail::change);
					}
				}
			}
		}
	}
}

void ledger_processor::send_block (nano::send_block const & block_a)
{
	auto hash (block_a.hash ());
	auto existing (ledger.store.block_exists (transaction, block_a.type (), hash));
	result.code = existing ? nano::process_result::old : nano::process_result::progress; // Have we seen this block before? (Harmless)
	if (result.code == nano::process_result::progress)
	{
		auto previous (ledger.store.block_get (transaction, block_a.hashables.previous));
		result.code = previous != nullptr ? nano::process_result::progress : nano::process_result::gap_previous; // Have we seen the previous block already? (Harmless)
		if (result.code == nano::process_result::progress)
		{
			result.code = block_a.valid_predecessor (*previous) ? nano::process_result::progress : nano::process_result::block_position;
			if (result.code == nano::process_result::progress)
			{
				auto account (ledger.store.frontier_get (transaction, block_a.hashables.previous));
				result.code = account.is_zero () ? nano::process_result::fork : nano::process_result::progress;
				if (result.code == nano::process_result::progress)
				{
					// Validate block if not verified outside of ledger
					if (result.verified != nano::signature_verification::valid)
					{
						result.code = validate_message (account, hash, block_a.signature) ? nano::process_result::bad_signature : nano::process_result::progress; // Is this block signed correctly (Malformed)
					}
					if (result.code == nano::process_result::progress)
					{
						assert (!validate_message (account, hash, block_a.signature));
						result.verified = nano::signature_verification::valid;
						auto state (ledger.account_state (transaction, account));
						assert (!state.head ().is_zero ());
						assert (state.head () == block_a.hashables.previous);
						result.code = state.balance ().number () >= block_a.hashables.balance.number () ? nano::process_result::progress : nano::process_result::negative_spend; // Is this trying to spend a negative amount (Malicious)
						if (result.code == nano::process_result::progress)
						{
							auto amount (state.balance ().number () - block_a.hashables.balance.number ());
							ledger.rep_weights.representation_add (state.rep (), 0 - amount);
							nano::block_sideband sideband (nano::block_type::send, account, 0, block_a.hashables.balance /* unused */, state.block_count () + 1, nano::seconds_since_epoch ());
							ledger.store.block_put (transaction, hash, block_a, sideband);
							nano::account_info new_info (hash, state.rep (), state.open ());
							ledger.change_latest (transaction, account, state, new_info, nano::epoch::epoch_0);
							ledger.store.pending_put (transaction, nano::pending_key (block_a.hashables.destination, hash), { account, amount, nano::epoch::epoch_0 });
							ledger.store.frontier_del (transaction, block_a.hashables.previous);
							ledger.store.frontier_put (transaction, hash, account);
							result.account = account;
							result.amount = amount;
							result.pending_account = block_a.hashables.destination;
							ledger.stats.inc (nano::stat::type::ledger, nano::stat::detail::send);
						}
					}
				}
			}
		}
	}
}

void ledger_processor::receive_block (nano::receive_block const & block_a)
{
	auto hash (block_a.hash ());
	auto existing (ledger.store.block_exists (transaction, block_a.type (), hash));
	result.code = existing ? nano::process_result::old : nano::process_result::progress; // Have we seen this block already?  (Harmless)
	if (result.code == nano::process_result::progress)
	{
		auto previous (ledger.store.block_get (transaction, block_a.hashables.previous));
		result.code = previous != nullptr ? nano::process_result::progress : nano::process_result::gap_previous;
		if (result.code == nano::process_result::progress)
		{
			result.code = block_a.valid_predecessor (*previous) ? nano::process_result::progress : nano::process_result::block_position;
			if (result.code == nano::process_result::progress)
			{
				auto account (ledger.store.frontier_get (transaction, block_a.hashables.previous));
				result.code = account.is_zero () ? nano::process_result::gap_previous : nano::process_result::progress; //Have we seen the previous block? No entries for account at all (Harmless)
				if (result.code == nano::process_result::progress)
				{
					// Validate block if not verified outside of ledger
					if (result.verified != nano::signature_verification::valid)
					{
						result.code = validate_message (account, hash, block_a.signature) ? nano::process_result::bad_signature : nano::process_result::progress; // Is the signature valid (Malformed)
					}
					if (result.code == nano::process_result::progress)
					{
						assert (!validate_message (account, hash, block_a.signature));
						result.verified = nano::signature_verification::valid;
						result.code = ledger.store.source_exists (transaction, block_a.hashables.source) ? nano::process_result::progress : nano::process_result::gap_source; // Have we seen the source block already? (Harmless)
						if (result.code == nano::process_result::progress)
						{
							auto state (ledger.account_state (transaction, account));
							assert (!state.head ().is_zero ());
							result.code = state.head () == block_a.hashables.previous ? nano::process_result::progress : nano::process_result::gap_previous; // Block doesn't immediately follow latest block (Harmless)
							if (result.code == nano::process_result::progress)
							{
								nano::pending_key key (account, block_a.hashables.source);
								nano::pending_info pending;
								result.code = ledger.store.pending_get (transaction, key, pending) ? nano::process_result::unreceivable : nano::process_result::progress; // Has this source already been received (Malformed)
								if (result.code == nano::process_result::progress)
								{
									result.code = pending.epoch == nano::epoch::epoch_0 ? nano::process_result::progress : nano::process_result::unreceivable; // Are we receiving a state-only send? (Malformed)
									if (result.code == nano::process_result::progress)
									{
										auto new_balance (state.balance ().number () + pending.amount.number ());
										nano::account_info source_info;
										auto error (ledger.store.account_get (transaction, pending.source, source_info));
										(void)error;
										assert (!error);
										ledger.store.pending_del (transaction, key);
										nano::block_sideband sideband (nano::block_type::receive, account, 0, new_balance, state.block_count () + 1, nano::seconds_since_epoch ());
										ledger.store.block_put (transaction, hash, block_a, sideband);
										nano::account_info new_info (hash, state.rep (), state.open ());
										ledger.change_latest (transaction, account, state, new_info, nano::epoch::epoch_0);
										ledger.rep_weights.representation_add (state.rep (), pending.amount.number ());
										ledger.store.frontier_del (transaction, block_a.hashables.previous);
										ledger.store.frontier_put (transaction, hash, account);
										result.account = account;
										result.amount = pending.amount;
										ledger.stats.inc (nano::stat::type::ledger, nano::stat::detail::receive);
									}
								}
							}
						}
					}
				}
				else
				{
					result.code = ledger.store.block_exists (transaction, block_a.hashables.previous) ? nano::process_result::fork : nano::process_result::gap_previous; // If we have the block but it's not the latest we have a signed fork (Malicious)
				}
			}
		}
	}
}

void ledger_processor::open_block (nano::open_block const & block_a)
{
	auto hash (block_a.hash ());
	auto existing (ledger.store.block_exists (transaction, block_a.type (), hash));
	result.code = existing ? nano::process_result::old : nano::process_result::progress; // Have we seen this block already? (Harmless)
	if (result.code == nano::process_result::progress)
	{
		// Validate block if not verified outside of ledger
		if (result.verified != nano::signature_verification::valid)
		{
			result.code = validate_message (block_a.hashables.account, hash, block_a.signature) ? nano::process_result::bad_signature : nano::process_result::progress; // Is the signature valid (Malformed)
		}
		if (result.code == nano::process_result::progress)
		{
			assert (!validate_message (block_a.hashables.account, hash, block_a.signature));
			result.verified = nano::signature_verification::valid;
			result.code = ledger.store.source_exists (transaction, block_a.hashables.source) ? nano::process_result::progress : nano::process_result::gap_source; // Have we seen the source block? (Harmless)
			if (result.code == nano::process_result::progress)
			{
				auto state (ledger.account_state (transaction, block_a.hashables.account));
				result.code = state.head ().is_zero () ? nano::process_result::progress : nano::process_result::fork; // Has this account already been opened? (Malicious)
				if (result.code == nano::process_result::progress)
				{
					nano::pending_key key (block_a.hashables.account, block_a.hashables.source);
					nano::pending_info pending;
					result.code = ledger.store.pending_get (transaction, key, pending) ? nano::process_result::unreceivable : nano::process_result::progress; // Has this source already been received (Malformed)
					if (result.code == nano::process_result::progress)
					{
						result.code = block_a.hashables.account == ledger.network_params.ledger.burn_account ? nano::process_result::opened_burn_account : nano::process_result::progress; // Is it burning 0 account? (Malicious)
						if (result.code == nano::process_result::progress)
						{
							result.code = pending.epoch == nano::epoch::epoch_0 ? nano::process_result::progress : nano::process_result::unreceivable; // Are we receiving a state-only send? (Malformed)
							if (result.code == nano::process_result::progress)
							{
								ledger.store.pending_del (transaction, key);
								nano::block_sideband sideband (nano::block_type::open, block_a.hashables.account, 0, pending.amount, 1, nano::seconds_since_epoch ());
								ledger.store.block_put (transaction, hash, block_a, sideband);
								ledger.change_latest (transaction, block_a.hashables.account, state, { hash, block_a.representative (), hash }, nano::epoch::epoch_0);
								ledger.rep_weights.representation_add (block_a.representative (), pending.amount.number ());
								ledger.store.frontier_put (transaction, hash, block_a.hashables.account);
								result.account = block_a.hashables.account;
								result.amount = pending.amount;
								ledger.stats.inc (nano::stat::type::ledger, nano::stat::detail::open);
							}
						}
					}
				}
			}
		}
	}
}

ledger_processor::ledger_processor (nano::ledger & ledger_a, nano::write_transaction const & transaction_a, nano::signature_verification verification_a) :
ledger (ledger_a),
transaction (transaction_a),
verification (verification_a)
{
	result.verified = verification;
}
} // namespace

size_t nano::shared_ptr_block_hash::operator() (std::shared_ptr<nano::block> const & block_a) const
{
	auto hash (block_a->hash ());
	auto result (static_cast<size_t> (hash.qwords[0]));
	return result;
}

bool nano::shared_ptr_block_hash::operator() (std::shared_ptr<nano::block> const & lhs, std::shared_ptr<nano::block> const & rhs) const
{
	return lhs->hash () == rhs->hash ();
}

nano::account_state::account_state (nano::account_info const & info_a, std::shared_ptr<nano::block> block_a, nano::block_sideband const & sideband_a) :
info_m (info_a),
block_m (block_a),
sideband (sideband_a)
{
	assert (block_a->hash () == info_a.head);
}

nano::account_state::account_state (nano::transaction const & transaction_a, nano::block_store & store_a, nano::account const & account_a)
{
	auto error (store_a.account_get (transaction_a, account_a, info_m));
	if (!error)
	{
		block_m = store_a.block_get (transaction_a, info_m.head, &sideband);
	}
}

nano::account_state::account_state (nano::transaction const & transaction_a, nano::block_store & store_a, nano::account_info const & info_a) :
info_m (info_a)
{
	assert (!info_a.head.is_zero ());
	block_m = store_a.block_get (transaction_a, info_m.head, &sideband);
}

nano::uint256_union nano::account_state::head () const
{
	return info_m.head;
}

nano::uint256_union nano::account_state::rep () const
{
	return info_m.representative;
}

nano::uint256_union nano::account_state::open () const
{
	return info_m.open_block;
}

nano::amount nano::account_state::balance () const
{
	if (block_m == nullptr)
	{
		return 0;
	}
	class balance_visitor : public nano::block_visitor
	{
	public:
		balance_visitor (nano::account_state const & state_a) :
		state (state_a)
		{
		}
		void send_block (nano::send_block const & block_a) override
		{
			result = block_a.hashables.balance;
		}
		void receive_block (nano::receive_block const &) override
		{
			result = state.sideband.balance;
		}
		void open_block (nano::open_block const &) override
		{
			result = state.sideband.balance;
		}
		void change_block (nano::change_block const &) override
		{
			result = state.sideband.balance;
		}
		void state_block (nano::state_block const & block_a) override
		{
			result = block_a.hashables.balance;
		}
		nano::account_state const & state;
		nano::amount result;
	};
	balance_visitor visitor (*this);
	block_m->visit (visitor);
	return visitor.result;
}

uint64_t nano::account_state::block_count () const
{
	return sideband.height;
}

std::shared_ptr<nano::block> nano::account_state::block () const
{
	return block_m;
}

uint64_t nano::account_state::modified () const
{
	return sideband.timestamp;
}

nano::epoch nano::account_state::epoch () const
{
	return block_m->epoch ();
}

nano::ledger::ledger (nano::block_store & store_a, nano::stat & stat_a, bool cache_reps_a, bool cache_cemented_count_a) :
store (store_a),
stats (stat_a),
check_bootstrap_weights (true)
{
	if (!store.init_error ())
	{
		auto transaction = store.tx_begin_read ();
		if (cache_reps_a)
		{
			for (auto i (store.latest_begin (transaction)), n (store.latest_end ()); i != n; ++i)
			{
				auto state (account_state (transaction, nano::account_info (i->second)));
				rep_weights.representation_add (state.rep (), state.balance ().number ());
			}
		}

		if (cache_cemented_count_a)
		{
			for (auto i (store.confirmation_height_begin (transaction)), n (store.confirmation_height_end ()); i != n; ++i)
			{
				cemented_count += i->second;
			}
		}
	}
}

// Balance for account containing hash
nano::uint128_t nano::ledger::balance (nano::transaction const & transaction_a, nano::block_hash const & hash_a) const
{
	return hash_a.is_zero () ? 0 : store.block_balance (transaction_a, hash_a);
}

// Balance for an account by account number
nano::uint128_t nano::ledger::account_balance (nano::transaction const & transaction_a, nano::account const & account_a)
{
	nano::uint128_t result (0);
	auto state (account_state (transaction_a, account_a));
	if (!state.head ().is_zero ())
	{
		result = state.balance ().number ();
	}
	return result;
}

nano::uint128_t nano::ledger::account_pending (nano::transaction const & transaction_a, nano::account const & account_a)
{
	nano::uint128_t result (0);
	nano::account end (account_a.number () + 1);
	for (auto i (store.pending_v0_begin (transaction_a, nano::pending_key (account_a, 0))), n (store.pending_v0_begin (transaction_a, nano::pending_key (end, 0))); i != n; ++i)
	{
		nano::pending_info const & info (i->second);
		result += info.amount.number ();
	}
	for (auto i (store.pending_v1_begin (transaction_a, nano::pending_key (account_a, 0))), n (store.pending_v1_begin (transaction_a, nano::pending_key (end, 0))); i != n; ++i)
	{
		nano::pending_info const & info (i->second);
		result += info.amount.number ();
	}
	return result;
}

nano::process_return nano::ledger::process (nano::write_transaction const & transaction_a, nano::block const & block_a, nano::signature_verification verification)
{
	assert (!nano::work_validate (block_a));
	ledger_processor processor (*this, transaction_a, verification);
	block_a.visit (processor);
	return processor.result;
}

nano::block_hash nano::ledger::representative (nano::transaction const & transaction_a, nano::block_hash const & hash_a)
{
	auto result (representative_calculated (transaction_a, hash_a));
	assert (result.is_zero () || store.block_exists (transaction_a, result));
	return result;
}

nano::block_hash nano::ledger::representative_calculated (nano::transaction const & transaction_a, nano::block_hash const & hash_a)
{
	representative_visitor visitor (transaction_a, store);
	visitor.compute (hash_a);
	return visitor.result;
}

bool nano::ledger::block_exists (nano::block_hash const & hash_a)
{
	auto transaction (store.tx_begin_read ());
	auto result (store.block_exists (transaction, hash_a));
	return result;
}

bool nano::ledger::block_exists (nano::block_type type, nano::block_hash const & hash_a)
{
	auto transaction (store.tx_begin_read ());
	auto result (store.block_exists (transaction, type, hash_a));
	return result;
}

std::string nano::ledger::block_text (char const * hash_a)
{
	return block_text (nano::block_hash (hash_a));
}

std::string nano::ledger::block_text (nano::block_hash const & hash_a)
{
	std::string result;
	auto transaction (store.tx_begin_read ());
	auto block (store.block_get (transaction, hash_a));
	if (block != nullptr)
	{
		block->serialize_json (result);
	}
	return result;
}

bool nano::ledger::is_send (nano::transaction const & transaction_a, nano::state_block const & block_a) const
{
	bool result (false);
	nano::block_hash previous (block_a.hashables.previous);
	if (!previous.is_zero ())
	{
		if (block_a.hashables.balance < balance (transaction_a, previous))
		{
			result = true;
		}
	}
	return result;
}

nano::block_hash nano::ledger::block_destination (nano::transaction const & transaction_a, nano::block const & block_a)
{
	nano::block_hash result (0);
	nano::send_block const * send_block (dynamic_cast<nano::send_block const *> (&block_a));
	nano::state_block const * state_block (dynamic_cast<nano::state_block const *> (&block_a));
	if (send_block != nullptr)
	{
		result = send_block->hashables.destination;
	}
	else if (state_block != nullptr && is_send (transaction_a, *state_block))
	{
		result = state_block->hashables.link;
	}
	return result;
}

nano::block_hash nano::ledger::block_source (nano::transaction const & transaction_a, nano::block const & block_a)
{
	/*
	 * block_source() requires that the previous block of the block
	 * passed in exist in the database.  This is because it will try
	 * to check account balances to determine if it is a send block.
	 */
	assert (block_a.previous ().is_zero () || store.block_exists (transaction_a, block_a.previous ()));

	// If block_a.source () is nonzero, then we have our source.
	// However, universal blocks will always return zero.
	nano::block_hash result (block_a.source ());
	nano::state_block const * state_block (dynamic_cast<nano::state_block const *> (&block_a));
	if (state_block != nullptr && !is_send (transaction_a, *state_block))
	{
		result = state_block->hashables.link;
	}
	return result;
}

// Vote weight of an account
nano::uint128_t nano::ledger::weight (nano::transaction const & transaction_a, nano::account const & account_a)
{
	if (check_bootstrap_weights.load ())
	{
		auto blocks = store.block_count (transaction_a);
		if (blocks.sum () < bootstrap_weight_max_blocks)
		{
			auto weight = bootstrap_weights.find (account_a);
			if (weight != bootstrap_weights.end ())
			{
				return weight->second;
			}
		}
		else
		{
			check_bootstrap_weights = false;
		}
	}
	return rep_weights.representation_get (account_a);
}

// Rollback blocks until `block_a' doesn't exist or it tries to penetrate the confirmation height
bool nano::ledger::rollback (nano::write_transaction const & transaction_a, nano::block_hash const & block_a, std::vector<std::shared_ptr<nano::block>> & list_a)
{
	assert (store.block_exists (transaction_a, block_a));
	auto account_l (account (transaction_a, block_a));
	auto block_account_height (store.block_account_height (transaction_a, block_a));
	rollback_visitor rollback (transaction_a, *this, list_a);
	nano::account_info account_info;
	auto error (false);
	while (!error && store.block_exists (transaction_a, block_a))
	{
		uint64_t confirmation_height;
		auto latest_error = store.confirmation_height_get (transaction_a, account_l, confirmation_height);
		assert (!latest_error);
		(void)latest_error;
		if (block_account_height > confirmation_height)
		{
			latest_error = store.account_get (transaction_a, account_l, account_info);
			assert (!latest_error);
			auto block (store.block_get (transaction_a, account_info.head));
			list_a.push_back (block);
			block->visit (rollback);
			error = rollback.error;
		}
		else
		{
			error = true;
		}
	}
	return error;
}

bool nano::ledger::rollback (nano::write_transaction const & transaction_a, nano::block_hash const & block_a)
{
	std::vector<std::shared_ptr<nano::block>> rollback_list;
	return rollback (transaction_a, block_a, rollback_list);
}

// Return account containing hash
nano::account nano::ledger::account (nano::transaction const & transaction_a, nano::block_hash const & hash_a) const
{
	return store.block_account (transaction_a, hash_a);
}

// Return amount decrease or increase for block
nano::uint128_t nano::ledger::amount (nano::transaction const & transaction_a, nano::block_hash const & hash_a)
{
	nano::uint128_t result;
	if (hash_a != network_params.ledger.genesis_account)
	{
		auto block (store.block_get (transaction_a, hash_a));
		auto block_balance (balance (transaction_a, hash_a));
		auto previous_balance (balance (transaction_a, block->previous ()));
		result = block_balance > previous_balance ? block_balance - previous_balance : previous_balance - block_balance;
	}
	else
	{
		result = network_params.ledger.genesis_amount;
	}
	return result;
}

// Return latest block for account
nano::block_hash nano::ledger::latest (nano::transaction const & transaction_a, nano::account const & account_a)
{
	nano::account_info info;
	auto latest_error (store.account_get (transaction_a, account_a, info));
	return latest_error ? 0 : info.head;
}

// Return latest root for account, account number of there are no blocks for this account.
nano::block_hash nano::ledger::latest_root (nano::transaction const & transaction_a, nano::account const & account_a)
{
	nano::account_info info;
	auto latest_error (store.account_get (transaction_a, account_a, info));
	nano::block_hash result;
	if (latest_error)
	{
		result = account_a;
	}
	else
	{
		result = info.head;
	}
	return result;
}

void nano::ledger::dump_account_chain (nano::account const & account_a)
{
	auto transaction (store.tx_begin_read ());
	auto hash (latest (transaction, account_a));
	while (!hash.is_zero ())
	{
		auto block (store.block_get (transaction, hash));
		assert (block != nullptr);
		std::cerr << hash.to_string () << std::endl;
		hash = block->previous ();
	}
}

class block_fit_visitor : public nano::block_visitor
{
public:
	block_fit_visitor (nano::ledger & ledger_a, nano::transaction const & transaction_a) :
	ledger (ledger_a),
	transaction (transaction_a),
	result (false)
	{
	}
	void send_block (nano::send_block const & block_a) override
	{
		result = ledger.store.block_exists (transaction, block_a.previous ());
	}
	void receive_block (nano::receive_block const & block_a) override
	{
		result = ledger.store.block_exists (transaction, block_a.previous ());
		result &= ledger.store.block_exists (transaction, block_a.source ());
	}
	void open_block (nano::open_block const & block_a) override
	{
		result = ledger.store.block_exists (transaction, block_a.source ());
	}
	void change_block (nano::change_block const & block_a) override
	{
		result = ledger.store.block_exists (transaction, block_a.previous ());
	}
	void state_block (nano::state_block const & block_a) override
	{
		result = block_a.previous ().is_zero () || ledger.store.block_exists (transaction, block_a.previous ());
		if (result && !ledger.is_send (transaction, block_a))
		{
			result &= ledger.store.block_exists (transaction, block_a.hashables.link) || block_a.hashables.link.is_zero () || ledger.is_epoch_link (block_a.hashables.link);
		}
	}
	nano::ledger & ledger;
	nano::transaction const & transaction;
	bool result;
};

bool nano::ledger::could_fit (nano::transaction const & transaction_a, nano::block const & block_a)
{
	block_fit_visitor visitor (*this, transaction_a);
	block_a.visit (visitor);
	return visitor.result;
}

bool nano::ledger::is_epoch_link (nano::uint256_union const & link_a)
{
	return network_params.ledger.epochs.is_epoch_link (link_a);
}

nano::account const & nano::ledger::signer (nano::uint256_union const & link_a) const
{
	return network_params.ledger.epochs.signer (network_params.ledger.epochs.epoch (link_a));
}

nano::uint256_union const & nano::ledger::link (nano::epoch epoch_a) const
{
	return network_params.ledger.epochs.link (nano::epoch::epoch_1);
}

nano::account_state nano::ledger::account_state (nano::transaction const & transaction_a, nano::account const & account_a)
{
	nano::account_state result (transaction_a, store, account_a);
	return result;
}

nano::account_state nano::ledger::account_state (nano::transaction const & transaction_a, nano::account_info const & info_a)
{
	nano::account_state result (transaction_a, store, info_a);
	return result;
}

void nano::ledger::change_latest (nano::write_transaction const & transaction_a, nano::account const & account_a, nano::account_state const & old_a, nano::account_info const & new_a, nano::epoch new_epoch)
{
	if (!new_a.head.is_zero ())
	{
		if (old_a.head ().is_zero () && new_a.open_block == new_a.head)
		{
			assert (!store.confirmation_height_exists (transaction_a, account_a));
			store.confirmation_height_put (transaction_a, account_a, 0);
		}
		if (!old_a.head ().is_zero () && old_a.epoch () != new_epoch)
		{
			// store.account_put won't erase existing entries if they're in different tables
			store.account_del (transaction_a, account_a);
		}
		store.account_put (transaction_a, account_a, new_a, new_epoch);
	}
	else
	{
		store.confirmation_height_del (transaction_a, account_a);
		store.account_del (transaction_a, account_a);
	}
}

std::shared_ptr<nano::block> nano::ledger::successor (nano::transaction const & transaction_a, nano::qualified_root const & root_a)
{
	nano::block_hash successor (0);
	if (root_a.uint256s[0].is_zero () && store.account_exists (transaction_a, root_a.uint256s[1]))
	{
		nano::account_info info;
		auto error (store.account_get (transaction_a, root_a.uint256s[1], info));
		(void)error;
		assert (!error);
		successor = info.open_block;
	}
	else
	{
		successor = store.block_successor (transaction_a, root_a.uint256s[0]);
	}
	std::shared_ptr<nano::block> result;
	if (!successor.is_zero ())
	{
		result = store.block_get (transaction_a, successor);
	}
	assert (successor.is_zero () || result != nullptr);
	return result;
}

std::shared_ptr<nano::block> nano::ledger::forked_block (nano::transaction const & transaction_a, nano::block const & block_a)
{
	assert (!store.block_exists (transaction_a, block_a.type (), block_a.hash ()));
	auto root (block_a.root ());
	assert (store.block_exists (transaction_a, root) || store.account_exists (transaction_a, root));
	auto result (store.block_get (transaction_a, store.block_successor (transaction_a, root)));
	if (result == nullptr)
	{
		nano::account_info info;
		auto error (store.account_get (transaction_a, root, info));
		(void)error;
		assert (!error);
		result = store.block_get (transaction_a, info.open_block);
		assert (result != nullptr);
	}
	return result;
}

bool nano::ledger::block_confirmed (nano::transaction const & transaction_a, nano::block_hash const & hash_a) const
{
	auto confirmed (false);
	auto block_height (store.block_account_height (transaction_a, hash_a));
	if (block_height > 0) // 0 indicates that the block doesn't exist
	{
		uint64_t confirmation_height;
		release_assert (!store.confirmation_height_get (transaction_a, account (transaction_a, hash_a), confirmation_height));
		confirmed = (confirmation_height >= block_height);
	}
	return confirmed;
}

bool nano::ledger::block_not_confirmed_or_not_exists (nano::block const & block_a) const
{
	bool result (true);
	auto hash (block_a.hash ());
	auto transaction (store.tx_begin_read ());
	if (store.block_exists (transaction, block_a.type (), hash))
	{
		result = !block_confirmed (transaction, hash);
	}
	return result;
}

size_t nano::ledger::block_count () const
{
	auto transaction (store.tx_begin_read ());
	auto result (store.block_count (transaction).sum ());
	return result;
}

namespace nano
{
std::unique_ptr<seq_con_info_component> collect_seq_con_info (ledger & ledger, const std::string & name)
{
	auto composite = std::make_unique<seq_con_info_composite> (name);
	auto count = ledger.bootstrap_weights_size.load ();
	auto sizeof_element = sizeof (decltype (ledger.bootstrap_weights)::value_type);
	composite->add_component (std::make_unique<seq_con_info_leaf> (seq_con_info{ "bootstrap_weights", count, sizeof_element }));
	composite->add_component (collect_seq_con_info (ledger.rep_weights, "rep_weights"));
	return composite;
}
}
