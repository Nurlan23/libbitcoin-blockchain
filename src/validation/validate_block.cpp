/**
 * Copyright (c) 2011-2015 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * libbitcoin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License with
 * additional permissions to the one published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version. For more information see LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/blockchain/validation/validate_block.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/blockchain/interface/simple_chain.hpp>
#include <bitcoin/blockchain/settings.hpp>
#include <bitcoin/blockchain/validation/fork.hpp>
#include <bitcoin/blockchain/validation/populate_block.hpp>

#ifdef WITH_CONSENSUS
#include <bitcoin/consensus.hpp>
#endif

namespace libbitcoin {
namespace blockchain {

using namespace bc::chain;
using namespace std::placeholders;

#define NAME "validate_block"

validate_block::validate_block(threadpool& pool, const simple_chain& chain,
    const settings& settings)
  : stopped_(false),
    use_libconsensus_(settings.use_libconsensus),
    populator_(pool, chain, settings),
    dispatch_(pool, NAME "_dispatch")
{
}

// Stop sequence.
//-----------------------------------------------------------------------------

void validate_block::stop()
{
    populator_.stop();
    stopped_.store(true);
}

bool validate_block::stopped() const
{
    return stopped_.load();
}

// Check.
//-----------------------------------------------------------------------------

// These checks are context free.
code validate_block::check(block_const_ptr block) const
{
    return block->check();
}

// Accept sequence.
//-----------------------------------------------------------------------------
// These checks require height or other chain context.

void validate_block::accept(fork::const_ptr fork, size_t index,
    result_handler handler) const
{
    BITCOIN_ASSERT(!fork->empty());
    BITCOIN_ASSERT(index < fork->size());

    const auto block = fork->block_at(index);

    // TODO: bypass population if state is populated.
    populator_.populate(fork, index,
        std::bind(&validate_block::handle_accepted,
            this, _1, block, handler));
}

void validate_block::handle_accepted(const code& ec, block_const_ptr block,
    result_handler handler) const
{
    handler(ec ? ec : block->accept());
}

// Connect sequence.
//-----------------------------------------------------------------------------
// These checks require output traversal and validation.

// We do not use block/tx.connect here because we want to fan out by input.
void validate_block::connect(fork::const_ptr fork, size_t index,
    result_handler handler) const
{
    BITCOIN_ASSERT(!fork->empty());
    BITCOIN_ASSERT(index < fork->size());
    const auto block = fork->block_at(index);

    // TODO: just populate from here if there is no state.
    if (!block->validation.state)
    {
        // We must complete on a new thread.
        dispatch_.concurrent(handler, error::operation_failed);
        return;
    }

    const auto& txs = block->transactions;
    const result_handler complete_handler =
        std::bind(&validate_block::handle_connect,
            this, _1, block, asio::steady_clock::now(), handler);

    if (txs.size() < 2 || !block->validation.state->use_full_validation())
    {
        // We must complete on a new thread.
        dispatch_.concurrent(complete_handler, error::success);
        return;
    }

    const auto non_coinbase_inputs = block->total_inputs(false);
    const result_handler join_handler = synchronize(complete_handler,
        non_coinbase_inputs, NAME "_validate");

    const auto flags = block->validation.state->enabled_forks();

    // Skip coinbase.
    for (auto tx = txs.begin() + 1; tx != txs.end(); ++tx)
        for (size_t index = 0; index < tx->inputs.size(); ++index)
            dispatch_.concurrent(&validate_block::connect_input,
                this, std::ref(*tx), index, flags, join_handler);
}

void validate_block::connect_input(const transaction& tx, size_t input_index,
    uint32_t flags, result_handler handler) const
{
    BITCOIN_ASSERT(!tx.is_coinbase());
    BITCOIN_ASSERT(input_index < tx.inputs.size());

    if (stopped())
    {
        handler(error::service_stopped);
        return;
    }

    const auto& prevout = tx.inputs[input_index].previous_output.validation;

    if (!prevout.cache.is_valid())
    {
        handler(error::input_not_found);
        return;
    }

    const auto index32 = static_cast<uint32_t>(input_index);
    handler(verify_script(tx, index32, flags, use_libconsensus_));
}

void validate_block::handle_connect(const code& ec, block_const_ptr block,
    asio::time_point start_time, result_handler handler) const
{
    BITCOIN_ASSERT(block->validation.state);
    const auto skipped = !block->validation.state->use_full_validation();
    const auto validated = skipped ? "accepted " : "validated";
    const auto token = ec ? "INVALIDATED" : validated;
    report(block, start_time, token);
    handler(ec);
}

// Utility.
//-----------------------------------------------------------------------------

void validate_block::report(block_const_ptr block, asio::time_point start_time,
    const std::string& token)
{
    BITCOIN_ASSERT(block->validation.state);
    static constexpr size_t micro_per_milliseconds = 1000;
    const auto delta = asio::steady_clock::now() - start_time;
    const auto elapsed = std::chrono::duration_cast<asio::microseconds>(delta);
    const auto micro_per_block = static_cast<float>(elapsed.count());
    const auto micro_per_input = micro_per_block / block->total_inputs();
    const auto milli_per_block = micro_per_block / micro_per_milliseconds;
    const auto transactions = block->transactions.size();
    const auto next_height = block->validation.state->next_height();

    log::info(LOG_BLOCKCHAIN)
        << "Block [" << next_height << "] " << token << " (" << transactions
        << ") txs in (" << milli_per_block << ") ms or (" << micro_per_input
        << ") μs/input";
}

// Verify script.
//-----------------------------------------------------------------------------

#ifdef WITH_CONSENSUS

// TODO: move to libconsensus.hpp/cpp
static uint32_t convert_flags(uint32_t native_flags)
{
    using namespace bc::consensus;
    uint32_t consensus_flags = verify_flags_none;

    if (script::is_enabled(native_flags, rule_fork::bip16_rule))
        consensus_flags |= verify_flags_p2sh;

    if (script::is_enabled(native_flags, rule_fork::bip65_rule))
        consensus_flags |= verify_flags_checklocktimeverify;

    if (script::is_enabled(native_flags, rule_fork::bip66_rule))
        consensus_flags |= verify_flags_dersig;

    return consensus_flags;
}

// TODO: move to libconsensus.hpp/cpp
static code convert_result(consensus::verify_result_type result)
{
    using namespace bc::consensus;
    switch (result)
    {
        // Logical true result.
        case verify_result_type::verify_result_eval_true:
            return error::success;

        // Logical false result.
        case verify_result_type::verify_result_eval_false:
            return error::validate_inputs_failed;

        // Max size errors.
        case verify_result_type::verify_result_script_size:
        case verify_result_type::verify_result_push_size:
        case verify_result_type::verify_result_op_count:
        case verify_result_type::verify_result_stack_size:
        case verify_result_type::verify_result_sig_count:
        case verify_result_type::verify_result_pubkey_count:
            return error::size_limits;

        // Failed verify operations.
        case verify_result_type::verify_result_verify:
        case verify_result_type::verify_result_equalverify:
        case verify_result_type::verify_result_checkmultisigverify:
        case verify_result_type::verify_result_checksigverify:
        case verify_result_type::verify_result_numequalverify:
            return error::validate_inputs_failed;

        // Logical/Format/Canonical errors.
        case verify_result_type::verify_result_bad_opcode:
        case verify_result_type::verify_result_disabled_opcode:
        case verify_result_type::verify_result_invalid_stack_operation:
        case verify_result_type::verify_result_invalid_altstack_operation:
        case verify_result_type::verify_result_unbalanced_conditional:
            return error::validate_inputs_failed;

        // BIP62 errors (should not see these unless requsted).
        case verify_result_type::verify_result_sig_hashtype:
        case verify_result_type::verify_result_sig_der:
        case verify_result_type::verify_result_minimaldata:
        case verify_result_type::verify_result_sig_pushonly:
        case verify_result_type::verify_result_sig_high_s:
        case verify_result_type::verify_result_sig_nulldummy:
        case verify_result_type::verify_result_pubkeytype:
        case verify_result_type::verify_result_cleanstack:
            return error::validate_inputs_failed;

        // Softfork safeness
        case verify_result_type::verify_result_discourage_upgradable_nops:
            return error::validate_inputs_failed;

        // Other
        case verify_result_type::verify_result_op_return:
        case verify_result_type::verify_result_unknown_error:
            return error::validate_inputs_failed;

        // augmention codes for tx deserialization
        case verify_result_type::verify_result_tx_invalid:
        case verify_result_type::verify_result_tx_size_invalid:
        case verify_result_type::verify_result_tx_input_invalid:
            return error::validate_inputs_failed;

        // BIP65 errors
        case verify_result_type::verify_result_negative_locktime:
        case verify_result_type::verify_result_unsatisfied_locktime:
            return error::validate_inputs_failed;

        default:
            return error::validate_inputs_failed;
    }
}

code validate_block::verify_script(const transaction& tx, uint32_t input_index,
    uint32_t flags, bool use_libconsensus)
{
    if (!use_libconsensus)
        return script::verify(tx, input_index, flags);

    BITCOIN_ASSERT(input_index < tx.inputs.size());
    const auto& prevout = tx.inputs[input_index].previous_output.validation;
    const auto script_data = prevout.cache.script.to_data(false);
    const auto tx_data = tx.to_data();

    // libconsensus
    return convert_result(consensus::verify_script(tx_data.data(),
        tx_data.size(), script_data.data(), script_data.size(), input_index,
        convert_flags(flags)));
}

#else

code validate_block::verify_script(const transaction& tx,
    uint32_t input_index, uint32_t flags, bool)
{
    return script::verify(tx, input_index, flags);
}

#endif

} // namespace blockchain
} // namespace libbitcoin