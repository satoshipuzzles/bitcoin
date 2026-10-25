#!/usr/bin/env python3
# Copyright (c) 2026 The Bitcoin Knots developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test rolling coinbase maturity policy and consensus.

Mirrors mainnet at a small scale: a long maturity window holds rewards from
START until RELEASE, and the rolling rule takes over at RELEASE for every
reward from START on. Each reward then unlocks ROLLING_MATURITY blocks after
its own block, one per block, instead of all of them together at RELEASE.

The same checks run on three nodes, without RDTS, with RDTS active, and with
RDTS already expired: unless the schedule also sets
-testcoinbasematurityrollingexpiry (see feature_rolling_maturity_end_bound.py),
the rule does not follow RDTS.
"""

from test_framework.blocktools import (
    COINBASE_MATURITY,
    add_witness_commitment,
    create_block,
    create_coinbase,
)
from test_framework.test_node import ErrorMatch
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.wallet import MiniWallet

START_HEIGHT = 110
RELEASE_HEIGHT = START_HEIGHT + COINBASE_MATURITY + 6
LONG_MATURITY = RELEASE_HEIGHT - START_HEIGHT
ROLLING_MATURITY = COINBASE_MATURITY + 50
DEPLOYMENT = "rolling_coinbase_maturity"
FORK_HEIGHT = 2
PREMATURE = "bad-txns-premature-spend-of-coinbase"

BASE_ARGS = [
    f"-testcoinbasematuritylong={START_HEIGHT}:{START_HEIGHT}:{RELEASE_HEIGHT}",
    f"-testcoinbasematurityrolling={START_HEIGHT}:{RELEASE_HEIGHT}:{ROLLING_MATURITY}",
    "-checkmempool=1",
]
# name, extra args, whether RDTS is enforced once blocks are being mined
MODES = [
    ("without RDTS", [], None),
    ("with RDTS active", [f"-testactivationheight=blake2b@{FORK_HEIGHT}", "-rdtsexpiry=4102444800"], True),
    ("with RDTS expired", [f"-testactivationheight=blake2b@{FORK_HEIGHT}", "-rdtsexpiry=1296688603"], False),
]


class RollingCoinbaseMaturityTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = len(MODES)
        self.setup_clean_chain = True
        self.extra_args = [BASE_ARGS + extra for _, extra, _ in MODES]

    def setup_network(self):
        # Each node is its own chain.
        self.setup_nodes()

    def create_next_block(self, node, txs=None):
        tip = node.getbestblockhash()
        height = node.getblockcount() + 1
        block = create_block(
            int(tip, 16),
            create_coinbase(height, script_pubkey=self.wallet.get_output_script()),
            node.getblockheader(tip)["time"] + 1,
            height=height,
            txlist=txs,
            header_v2=self.rdts is not None and height >= FORK_HEIGHT,
        )
        add_witness_commitment(block)
        block.solve()
        return block

    def submit(self, node, txs=None):
        return node.submitblock(self.create_next_block(node, txs).serialize().hex())

    def coinbase_spend(self, node, height):
        txid = node.getblock(node.getblockhash(height))["tx"][0]
        if not any(u["txid"] == txid for u in self.wallet._utxos):
            self.wallet.rescan_utxos(include_mempool=False)
        return self.wallet.create_self_transfer(utxo_to_spend=self.wallet.get_utxo(txid=txid, mark_as_spent=False))

    def mine_to(self, node, height):
        while node.getblockcount() < height:
            assert_equal(self.submit(node), None)

    def assert_deployment(self, node, active):
        deployment = node.getdeploymentinfo()["deployments"][DEPLOYMENT]
        assert_equal(deployment["type"], "flagday")
        assert_equal(deployment["height"], RELEASE_HEIGHT)
        assert_equal(deployment["coinbase_start_height"], START_HEIGHT)
        assert_equal(deployment["maturity"], ROLLING_MATURITY)
        assert "height_end" not in deployment
        assert "expiry_time" not in deployment
        assert "coinbase_expiry_time" not in deployment
        assert "coinbase_end_height" not in deployment
        assert_equal(deployment["active"], active)
        rules = ["segwit"] + (["blake2b"] if self.rdts is not None else [])
        assert_equal(DEPLOYMENT in node.getblocktemplate({"rules": rules})["rules"], active)

    def assert_rdts(self, node):
        if self.rdts is None:
            return
        assert_equal(node.getdeploymentinfo()["deployments"]["reduced_data"]["active"], self.rdts)

    def check_rejected_schedules(self):
        node = self.nodes[0]
        self.stop_node(0)
        for args, msg in [
            (["-testcoinbasematurityrolling=150:200"], "Invalid format"),
            (["-testcoinbasematurityrolling=1:2:3:4:5"], "Invalid format"),
            (["-testcoinbasematurityrolling=-1:200:200"], "Invalid start height"),
            (["-testcoinbasematurityrolling=99999999999999999999:200:200"], "Invalid start height"),
            (["-testcoinbasematurityrolling=150:1:200"], "Invalid enforce height"),
            ([f"-testcoinbasematurityrolling=150:200:{COINBASE_MATURITY}"], "Invalid maturity"),
            (["-testcoinbasematurityrolling=150:200:200:300"], "Invalid format"),
            (["-testcoinbasematurityrollingexpiry=1700000000"], "Invalid rolling coinbase maturity expiry"),
            (["-testcoinbasematurityrolling=150:200:200", "-testcoinbasematurityrollingexpiry=1296688602"], "Invalid rolling coinbase maturity expiry"),
            (["-testcoinbasematurityrolling=150:200:200", "-testcoinbasematurityrollingexpiry=1700000000",
              f"-testactivationheight=blake2b@{FORK_HEIGHT}", "-rdtsexpiry=1700000001"], "must match RDTS expiry"),
        ]:
            node.assert_start_raises_init_error(extra_args=args, expected_msg=msg, match=ErrorMatch.PARTIAL_REGEX)
        self.start_node(0, extra_args=self.extra_args[0].copy())

    def run_mode(self, n):
        name, _, self.rdts = MODES[n]
        node = self.nodes[n]
        self.log.info(f"Node {n}, {name}")
        self.wallet = MiniWallet(node)
        self.generate(self.wallet, 10, sync_fun=self.no_op)
        self.mine_to(node, START_HEIGHT - 1 + COINBASE_MATURITY - 1)
        self.assert_rdts(node)

        self.log.info("  A reward from before the start height keeps ordinary maturity")
        old = self.coinbase_spend(node, START_HEIGHT - 1)
        assert_equal(self.submit(node, [old["tx"]]), None)

        self.mine_to(node, RELEASE_HEIGHT - 2)
        self.assert_deployment(node, active=False)
        self.mine_to(node, RELEASE_HEIGHT - 1)
        self.assert_deployment(node, active=True)

        self.log.info("  Nothing is released together at the window's release height")
        # Without the rolling rule, every reward from START to RELEASE - 100
        # would become spendable in this one block.
        for coin_height in range(START_HEIGHT, RELEASE_HEIGHT - COINBASE_MATURITY + 1):
            spend = self.coinbase_spend(node, coin_height)
            assert_equal(self.submit(node, [spend["tx"]]), PREMATURE)
            assert_raises_rpc_error(-26, PREMATURE, node.sendrawtransaction, spend["hex"])
        assert_equal(node.getblockcount(), RELEASE_HEIGHT - 1)

        self.log.info("  Each reward unlocks on its own block, one per block")
        self.mine_to(node, START_HEIGHT + ROLLING_MATURITY - 2)
        first = self.coinbase_spend(node, START_HEIGHT)
        assert_equal(self.submit(node, [first["tx"]]), PREMATURE)
        assert_raises_rpc_error(-26, PREMATURE, node.sendrawtransaction, first["hex"])
        for coin_height in range(START_HEIGHT, START_HEIGHT + 4):
            # The next block is exactly ROLLING_MATURITY above this reward and
            # one short for the reward mined right after it.
            self.mine_to(node, coin_height + ROLLING_MATURITY - 1)
            spend = self.coinbase_spend(node, coin_height)
            nxt = self.coinbase_spend(node, coin_height + 1)
            assert_equal(self.submit(node, [nxt["tx"]]), PREMATURE)
            assert_raises_rpc_error(-26, PREMATURE, node.sendrawtransaction, nxt["hex"])
            node.sendrawtransaction(spend["hex"])
            assert spend["txid"] in node.getrawmempool()

            if coin_height == START_HEIGHT:
                self.log.info("  A reorg that makes the spend premature again evicts it from the mempool")
                tip_hash = node.getbestblockhash()
                node.invalidateblock(tip_hash)
                assert spend["txid"] not in node.getrawmempool()
                node.reconsiderblock(tip_hash)
                node.sendrawtransaction(spend["hex"])

            assert_equal(self.submit(node, [spend["tx"]]), None)
            assert_equal(node.getblockcount(), coin_height + ROLLING_MATURITY)

        self.assert_rdts(node)
        self.assert_deployment(node, active=True)

        self.log.info("  The rule still holds after a restart")
        self.restart_node(n)
        self.assert_deployment(node, active=True)
        spend = self.coinbase_spend(node, START_HEIGHT + 10)
        assert_raises_rpc_error(-26, PREMATURE, node.sendrawtransaction, spend["hex"])
        assert_equal(self.submit(node, [spend["tx"]]), PREMATURE)

    def run_test(self):
        self.check_rejected_schedules()
        for n in range(len(MODES)):
            self.run_mode(n)


if __name__ == "__main__":
    RollingCoinbaseMaturityTest(__file__).main()
