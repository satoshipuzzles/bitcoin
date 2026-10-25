// Copyright (c) 2026 The Bitcoin Knots developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <consensus/consensus.h>
#include <coins.h>
#include <consensus/amount.h>
#include <consensus/params.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <primitives/transaction.h>
#include <script/script.h>

#include <boost/test/unit_test.hpp>

#include <limits>
#include <vector>

BOOST_AUTO_TEST_SUITE(coinbase_maturity_tests)

BOOST_AUTO_TEST_CASE(long_maturity_window_boundaries)
{
    Consensus::Params params;
    params.CoinbaseMaturityLongEnforceHeight = 200;
    params.CoinbaseMaturityLongReleaseHeight = 300;

    BOOST_CHECK(!params.CoinbaseMaturityLongActiveAt(199));
    BOOST_CHECK(params.CoinbaseMaturityLongActiveAt(200));
    BOOST_CHECK(params.CoinbaseMaturityLongActiveAt(299));
    BOOST_CHECK(!params.CoinbaseMaturityLongActiveAt(300));
    BOOST_CHECK(!params.CoinbaseMaturityLongActiveAt(std::numeric_limits<int>::max()));
}

BOOST_AUTO_TEST_CASE(long_maturity_derived_from_schedule)
{
    const auto main{CChainParams::Main()};
    const auto testnet4{CChainParams::TestNet4()};
    for (const auto* params : {main.get(), testnet4.get()}) {
        const auto& consensus{params->GetConsensus()};
        BOOST_CHECK_EQUAL(consensus.CoinbaseMaturityLong, consensus.CoinbaseMaturityLongReleaseHeight - consensus.CoinbaseMaturityLongStartHeight);
        BOOST_CHECK_GT(consensus.CoinbaseMaturityLong, COINBASE_MATURITY);
        BOOST_REQUIRE_GE(consensus.chainstate_revalidation_deployments.size(), 1);
        const auto& deployment{consensus.chainstate_revalidation_deployments.front()};
        BOOST_CHECK_EQUAL(deployment.name, "long_coinbase_maturity");
        BOOST_CHECK_EQUAL(deployment.start_height, consensus.CoinbaseMaturityLongEnforceHeight);
        BOOST_CHECK_EQUAL(deployment.stop_height, consensus.CoinbaseMaturityLongReleaseHeight - 1);
    }

    const auto regtest_default{CChainParams::RegTest({})};
    const auto& default_consensus{regtest_default->GetConsensus()};
    BOOST_CHECK_EQUAL(default_consensus.CoinbaseMaturityLong, COINBASE_MATURITY);
    BOOST_CHECK(!default_consensus.CoinbaseMaturityLongActiveAt(0));
    BOOST_CHECK(default_consensus.chainstate_revalidation_deployments.empty());

    CChainParams::RegTestOptions options;
    options.coinbase_maturity_long_start_height = 10;
    options.coinbase_maturity_long_enforce_height = 20;
    options.coinbase_maturity_long_release_height = 150;
    const auto regtest{CChainParams::RegTest(options)};
    const auto& consensus{regtest->GetConsensus()};
    BOOST_CHECK_EQUAL(consensus.CoinbaseMaturityLong, 140);
    BOOST_CHECK(!consensus.CoinbaseMaturityLongActiveAt(19));
    BOOST_CHECK(consensus.CoinbaseMaturityLongActiveAt(20));
    BOOST_CHECK(consensus.CoinbaseMaturityLongActiveAt(149));
    BOOST_CHECK(!consensus.CoinbaseMaturityLongActiveAt(150));
    BOOST_REQUIRE_EQUAL(consensus.chainstate_revalidation_deployments.size(), 1);
    const auto& deployment{consensus.chainstate_revalidation_deployments.front()};
    BOOST_CHECK_EQUAL(deployment.name, "long_coinbase_maturity");
    BOOST_CHECK_EQUAL(deployment.start_height, 20);
    BOOST_CHECK_EQUAL(deployment.stop_height, 149);
}

BOOST_AUTO_TEST_CASE(rolling_maturity_mainnet_schedule)
{
    const auto main{CChainParams::Main()};
    const auto& consensus{main->GetConsensus()};

    // Covers every reward the long maturity window covers, takes over at the
    // window's release height, and lasts 365 days of 144 blocks.
    BOOST_CHECK_EQUAL(consensus.CoinbaseMaturityRollingStartHeight, 973440);
    BOOST_CHECK_EQUAL(consensus.CoinbaseMaturityRollingStartHeight, consensus.CoinbaseMaturityLongStartHeight);
    BOOST_CHECK_EQUAL(consensus.CoinbaseMaturityRollingEnforceHeight, 979920);
    BOOST_CHECK_EQUAL(consensus.CoinbaseMaturityRollingEnforceHeight, consensus.CoinbaseMaturityLongReleaseHeight);
    BOOST_CHECK_EQUAL(consensus.CoinbaseMaturityRolling, 365 * 144);

    BOOST_CHECK(!consensus.CoinbaseMaturityRollingActiveAt(979919));
    BOOST_CHECK(consensus.CoinbaseMaturityRollingActiveAt(979920));
    BOOST_CHECK(consensus.CoinbaseMaturityRollingActiveAt(std::numeric_limits<int>::max() - 1));

    // Coverage ends with RDTS (September 1st, 2027 00:00 UTC): a reward
    // mined in the first block whose parent has reached that median time past,
    // or later, keeps the maturity the earlier rules require (plain
    // COINBASE_MATURITY, the long window being over by then). A reward
    // covered before keeps the full depth, even when spent after expiry.
    BOOST_CHECK(consensus.CoinbaseMaturityRollingUntilRdts);
    BOOST_CHECK_EQUAL(consensus.RdtsExpiryTime, 1819756800);
    constexpr int END{1030000}; // stand-in for the first height past expiry
    BOOST_CHECK_EQUAL(consensus.CoinbaseMaturityRequired(END - 1, END - 1 + 52560 - 1, END), 52560);
    BOOST_CHECK_EQUAL(consensus.CoinbaseMaturityRequired(END - 1, END - 1 + 52560, END), 52560);
    BOOST_CHECK_EQUAL(consensus.CoinbaseMaturityRequired(END, END + COINBASE_MATURITY, END), COINBASE_MATURITY);
    BOOST_CHECK_EQUAL(consensus.CoinbaseMaturityRequired(END + 1, END + 1 + COINBASE_MATURITY, END), COINBASE_MATURITY);
    // Until the chain reaches the expiry, every reward from the start is covered.
    BOOST_CHECK_EQUAL(consensus.CoinbaseMaturityRequired(END + 1, END + 1 + COINBASE_MATURITY), 52560);

    BOOST_REQUIRE_EQUAL(consensus.chainstate_revalidation_deployments.size(), 2);
    const auto& deployment{consensus.chainstate_revalidation_deployments.back()};
    BOOST_CHECK_EQUAL(deployment.name, "rolling_coinbase_maturity_1y");
    BOOST_CHECK_EQUAL(deployment.start_height, 979920);
    BOOST_CHECK_EQUAL(deployment.stop_height, std::numeric_limits<int>::max());

    // Other chains are untouched.
    const auto testnet4{CChainParams::TestNet4()};
    BOOST_CHECK(!testnet4->GetConsensus().CoinbaseMaturityRollingActiveAt(std::numeric_limits<int>::max() - 1));
    const auto regtest_default{CChainParams::RegTest({})};
    BOOST_CHECK(!regtest_default->GetConsensus().CoinbaseMaturityRollingActiveAt(std::numeric_limits<int>::max() - 1));

    CChainParams::RegTestOptions options;
    options.coinbase_maturity_rolling_start_height = 10;
    options.coinbase_maturity_rolling_enforce_height = 20;
    options.coinbase_maturity_rolling = 150;
    const auto regtest{CChainParams::RegTest(options)};
    BOOST_CHECK_EQUAL(regtest->GetConsensus().CoinbaseMaturityRolling, 150);
    BOOST_CHECK(!regtest->GetConsensus().CoinbaseMaturityRollingActiveAt(19));
    BOOST_CHECK(regtest->GetConsensus().CoinbaseMaturityRollingActiveAt(20));
    // Without an expiry the coverage never ends.
    BOOST_CHECK(!regtest->GetConsensus().CoinbaseMaturityRollingUntilRdts);

    options.coinbase_maturity_rolling_expiry = 1700000000;
    const auto regtest_end{CChainParams::RegTest(options)};
    BOOST_CHECK(regtest_end->GetConsensus().CoinbaseMaturityRollingUntilRdts);
    BOOST_CHECK_EQUAL(regtest_end->GetConsensus().RdtsExpiryTime, 1700000000);
}

BOOST_AUTO_TEST_CASE(rolling_maturity_rdts_end_height)
{
    // A chain with one block per 600 seconds. The rolling rule stops covering
    // rewards from the first block whose parent's median time past has
    // reached the RDTS expiry, the same boundary RDTS itself uses.
    constexpr int64_t T0{1700000000};
    constexpr int N{200};
    std::vector<CBlockIndex> blocks(N);
    for (int i{0}; i < N; ++i) {
        blocks[i].nHeight = i;
        blocks[i].nTime = T0 + 600 * i;
        blocks[i].pprev = i ? &blocks[i - 1] : nullptr;
        blocks[i].BuildSkip();
    }
    // MTP of block i is the time of block i - 5 (for i >= 10).
    BOOST_REQUIRE_EQUAL(blocks[100].GetMedianTimePast(), T0 + 600 * 95);

    Consensus::Params params;
    params.CoinbaseMaturityRollingStartHeight = 0;
    params.CoinbaseMaturityRollingEnforceHeight = 0;
    params.CoinbaseMaturityRolling = 500;
    params.RdtsExpiryTime = T0 + 600 * 95; // reached by the MTP of block 100

    // Not bound to RDTS: never ends.
    BOOST_CHECK_EQUAL(Consensus::CoinbaseMaturityRollingEndHeight(params, &blocks[N - 1]), std::numeric_limits<int>::max());

    params.CoinbaseMaturityRollingUntilRdts = true;
    BOOST_CHECK_EQUAL(Consensus::CoinbaseMaturityRollingEndHeight(params, nullptr), std::numeric_limits<int>::max());
    // Before the expiry is reached nothing is known to end.
    BOOST_CHECK_EQUAL(Consensus::CoinbaseMaturityRollingEndHeight(params, &blocks[99]), std::numeric_limits<int>::max());
    // Block 100 is the first parent at expiry, so block 101 is the first
    // uncovered reward, whichever later tip the spend builds on.
    for (const int tip : {100, 101, 150, N - 1}) {
        BOOST_CHECK_EQUAL(Consensus::CoinbaseMaturityRollingEndHeight(params, &blocks[tip]), 101);
    }
    // One second later the boundary moves one block later.
    params.RdtsExpiryTime += 1;
    BOOST_CHECK_EQUAL(Consensus::CoinbaseMaturityRollingEndHeight(params, &blocks[100]), std::numeric_limits<int>::max());
    BOOST_CHECK_EQUAL(Consensus::CoinbaseMaturityRollingEndHeight(params, &blocks[N - 1]), 102);

    // Covered reward 100 keeps the full depth after the expiry; reward 101
    // needs only COINBASE_MATURITY.
    const int end{Consensus::CoinbaseMaturityRollingEndHeight(params, &blocks[N - 1])};
    BOOST_CHECK_EQUAL(params.CoinbaseMaturityRequired(101, 101 + 500, end), 500);
    BOOST_CHECK_EQUAL(params.CoinbaseMaturityRequired(102, 102 + COINBASE_MATURITY, end), COINBASE_MATURITY);
}

namespace {
//! First height at which a block may spend a reward mined at coin_height.
//! Spendability only changes at these breakpoints, so checking each is exact.
int ReleaseHeight(const Consensus::Params& params, int coin_height)
{
    int release{std::numeric_limits<int>::max()};
    for (const int maturity : {COINBASE_MATURITY, params.CoinbaseMaturityLong, params.CoinbaseMaturityRolling}) {
        for (const int candidate : {coin_height + maturity, params.CoinbaseMaturityLongEnforceHeight,
                                    params.CoinbaseMaturityLongReleaseHeight, params.CoinbaseMaturityRollingEnforceHeight}) {
            if (candidate <= coin_height || candidate == std::numeric_limits<int>::max()) continue;
            if (candidate - coin_height >= params.CoinbaseMaturityRequired(coin_height, candidate)) {
                release = std::min(release, candidate);
            }
        }
    }
    return release;
}
} // namespace

BOOST_AUTO_TEST_CASE(rolling_maturity_releases_one_reward_per_block)
{
    const auto main{CChainParams::Main()};
    const auto& consensus{main->GetConsensus()};

    // Without the rolling rule, the long window releases every reward it holds
    // together at 979920.
    Consensus::Params window_only{consensus};
    window_only.CoinbaseMaturityRollingStartHeight = std::numeric_limits<int>::max();
    window_only.CoinbaseMaturityRollingEnforceHeight = std::numeric_limits<int>::max();
    int cliff{0};
    for (int coin_height{973440}; coin_height < 979920; ++coin_height) {
        if (ReleaseHeight(window_only, coin_height) == 979920) ++cliff;
    }
    BOOST_CHECK_GT(cliff, 6000);

    // With it, every reward from 973440 on unlocks exactly 52560 blocks after
    // it was mined, so no two rewards ever unlock at the same height.
    for (int coin_height{973440}; coin_height < 1200000; ++coin_height) {
        const int release{ReleaseHeight(consensus, coin_height)};
        if (release != coin_height + 52560) {
            BOOST_FAIL("reward at " << coin_height << " unlocks at " << release);
        }
    }
    BOOST_CHECK_EQUAL(ReleaseHeight(consensus, 973440), 1026000);

    // Rewards from before the window keep ordinary maturity.
    BOOST_CHECK_EQUAL(ReleaseHeight(consensus, 973439), 973439 + COINBASE_MATURITY);
    BOOST_CHECK_EQUAL(ReleaseHeight(consensus, 960000), 960000 + COINBASE_MATURITY);

    // Through 979919 nothing changes from the rules already enforced.
    for (int spend_height{973440}; spend_height < 979920; spend_height += 7) {
        for (int coin_height{spend_height - 7000}; coin_height < spend_height; coin_height += 13) {
            BOOST_CHECK_EQUAL(consensus.CoinbaseMaturityRequired(coin_height, spend_height),
                              window_only.CoinbaseMaturityRequired(coin_height, spend_height));
        }
    }
}

BOOST_AUTO_TEST_CASE(rolling_maturity_independent_of_rdts)
{
    // The rule is decided by heights alone. Moving or removing the RDTS expiry,
    // or the BLAKE2b fork it hangs from, changes nothing about it.
    const auto main{CChainParams::Main()};
    const auto& consensus{main->GetConsensus()};
    for (const int64_t expiry : {std::numeric_limits<int64_t>::min(), int64_t{0}, int64_t{1819756800}, std::numeric_limits<int64_t>::max()}) {
        Consensus::Params changed{consensus};
        changed.RdtsExpiryTime = expiry;
        for (const int blake2b_height : {consensus.Blake2bHeight, std::numeric_limits<int>::max()}) {
            changed.Blake2bHeight = blake2b_height;
            for (int coin_height{973000}; coin_height < 1100000; coin_height += 997) {
                for (const int depth : {100, 6479, 6480, 52559, 52560}) {
                    BOOST_CHECK_EQUAL(changed.CoinbaseMaturityRequired(coin_height, coin_height + depth),
                                      consensus.CoinbaseMaturityRequired(coin_height, coin_height + depth));
                }
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(rolling_maturity_clean_activation_after_release)
{
    // An enforce height A is "clean" when no covered reward was ever
    // spendable under the earlier rules before A, which holds whenever
    // A <= StartHeight + required_prior: enforcement then only extends
    // maturities that are still pending and never re-locks a coin some
    // node may already have seen spent.
    //
    // Mainnet mapping for an enforce height after 979920: if the shipped
    // interim rule is the rolling-180d maturity (25920 blocks), the bound
    // is enforce < 979919 + 25920 = 1005839; if the shipped interim rule
    // is the freeze-by-RDTS-expiry, covered rewards stay locked until the
    // expiry, so any enforce height before it is clean.
    Consensus::Params params;
    params.CoinbaseMaturityLongStartHeight = 200;
    params.CoinbaseMaturityLongEnforceHeight = 200;
    params.CoinbaseMaturityLongReleaseHeight = 300;
    params.CoinbaseMaturityLong = params.CoinbaseMaturityLongReleaseHeight - params.CoinbaseMaturityLongStartHeight;
    params.CoinbaseMaturityRollingStartHeight = 200;
    params.CoinbaseMaturityRollingEnforceHeight = 290;
    params.CoinbaseMaturityRolling = 500;

    const int A{params.CoinbaseMaturityRollingEnforceHeight};
    // The clean-activation precondition: every covered reward mined before A
    // still needs at least A - StartHeight blocks under the earlier rules.
    BOOST_CHECK_LE(A, params.CoinbaseMaturityLongStartHeight + params.CoinbaseMaturityLong);

    for (int coin_height{params.CoinbaseMaturityRollingStartHeight}; coin_height < A; ++coin_height) {
        // Before A no covered reward was ever spendable: the earlier rules
        // alone already required more depth than any pre-A block offers.
        for (int spend_height{coin_height + 1}; spend_height < A; ++spend_height) {
            BOOST_CHECK_GT(params.CoinbaseMaturityRequired(coin_height, spend_height), spend_height - coin_height);
        }
        // From A on the requirement is the larger of the earlier rules and
        // the rolling depth; with the rolling depth dominating here, exactly
        // the rolling depth.
        for (const int spend_height : {A, A + 1, coin_height + 499, coin_height + 500, coin_height + 5000}) {
            if (spend_height < A) continue;
            Consensus::Params prior{params};
            prior.CoinbaseMaturityRollingEnforceHeight = std::numeric_limits<int>::max();
            BOOST_CHECK_EQUAL(params.CoinbaseMaturityRequired(coin_height, spend_height),
                              std::max(prior.CoinbaseMaturityRequired(coin_height, spend_height), params.CoinbaseMaturityRolling));
        }
    }
}

BOOST_AUTO_TEST_CASE(rolling_maturity_check_tx_inputs)
{
    // Each covered reward needs the full rolling depth from its own block;
    // older rewards keep the rules that applied before.
    constexpr int ROLLING_START{1000};
    constexpr int ROLLING{5000};
    constexpr int LONG_START{500};
    constexpr int LONG{700};

    CCoinsView base;
    CCoinsViewCache view{&base};

    const auto spend_at{[&](int coin_height, int spend_height, int long_start, int rolling_end = std::numeric_limits<int>::max()) {
        CMutableTransaction cb;
        cb.vin.resize(1);
        cb.vin[0].prevout.SetNull();
        cb.vout.resize(1);
        cb.vout[0].nValue = 50 * COIN;
        cb.vout[0].scriptPubKey = CScript() << OP_TRUE;
        cb.nLockTime = coin_height; // distinct txid per height
        const CTransaction coinbase{cb};
        AddCoins(view, coinbase, coin_height);

        CMutableTransaction spend;
        spend.vin.emplace_back(COutPoint{coinbase.GetHash(), 0});
        spend.vout.emplace_back(49 * COIN, CScript() << OP_TRUE);
        TxValidationState state;
        CAmount fee;
        return Consensus::CheckTxInputs(CTransaction{spend}, state, view, spend_height, fee, CheckTxInputsRules::None,
                                        LONG, long_start, ROLLING, ROLLING_START, rolling_end);
    }};

    // Before the rolling start height: only the existing rules.
    BOOST_CHECK(spend_at(ROLLING_START - 1, ROLLING_START - 1 + COINBASE_MATURITY, std::numeric_limits<int>::max()));
    BOOST_CHECK(!spend_at(ROLLING_START - 2, ROLLING_START - 2 + LONG - 1, LONG_START));
    BOOST_CHECK(spend_at(ROLLING_START - 3, ROLLING_START - 3 + LONG, LONG_START));

    // From the rolling start height: one block short fails, exact depth passes.
    for (const int coin_height : {ROLLING_START, ROLLING_START + 1, ROLLING_START + 12345}) {
        BOOST_CHECK(!spend_at(coin_height, coin_height + COINBASE_MATURITY, std::numeric_limits<int>::max()));
        BOOST_CHECK(!spend_at(coin_height, coin_height + ROLLING - 1, std::numeric_limits<int>::max()));
        BOOST_CHECK(spend_at(coin_height, coin_height + ROLLING, std::numeric_limits<int>::max()));
    }

    // When the long rule also covers the coin, the longer of the two applies.
    BOOST_CHECK(!spend_at(ROLLING_START + 7, ROLLING_START + 7 + LONG, 0));
    BOOST_CHECK(spend_at(ROLLING_START + 8, ROLLING_START + 8 + ROLLING, 0));

    // With an end height (the first reward mined after RDTS expiry), the
    // reward mined just below it stays covered while the one mined at it
    // keeps only the earlier rules.
    constexpr int ROLLING_END{ROLLING_START + 100};
    BOOST_CHECK(!spend_at(ROLLING_END - 1, ROLLING_END - 1 + ROLLING - 1, std::numeric_limits<int>::max(), ROLLING_END));
    BOOST_CHECK(spend_at(ROLLING_END - 1, ROLLING_END - 1 + ROLLING, std::numeric_limits<int>::max(), ROLLING_END));
    BOOST_CHECK(spend_at(ROLLING_END, ROLLING_END + COINBASE_MATURITY, std::numeric_limits<int>::max(), ROLLING_END));
    BOOST_CHECK(!spend_at(ROLLING_END + 1, ROLLING_END + 1 + LONG - 1, 0, ROLLING_END));
    BOOST_CHECK(spend_at(ROLLING_END + 2, ROLLING_END + 2 + LONG, 0, ROLLING_END));
}

BOOST_AUTO_TEST_SUITE_END()
