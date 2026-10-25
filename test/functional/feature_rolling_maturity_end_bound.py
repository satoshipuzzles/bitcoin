#!/usr/bin/env python3
# Copyright (c) 2026 The Bitcoin Knots developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Rolling maturity coverage ends with RDTS and starts at the enforce height.

Blocks are mined with fixed timestamps (one per 600 seconds), so the median
time past of block h is the timestamp of block h - 5 and the expiry boundary
lands on a known height: the first reward not covered is the one mined in the
first block whose parent's median time past has reached the expiry.
"""
from test_framework.blocktools import add_witness_commitment, create_block, create_coinbase
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.wallet import MiniWallet

START = 200    # first covered coin height
ENFORCE = 320  # first height enforcing the rolling rule
DEPTH = 300
EXPIRY_PARENT = 209    # first block whose median time past reaches the expiry
END = EXPIRY_PARENT + 1  # first coin height no longer covered


class RollingMaturityEndBoundTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def setup_network(self):
        # The expiry depends on the genesis timestamp, known only once the
        # node runs; start it again with the schedule.
        self.setup_nodes()
        node = self.nodes[0]
        self.genesis_time = node.getblockheader(node.getblockhash(0))["time"]
        self.expiry = self.block_time(EXPIRY_PARENT - 5)
        self.args = [
            f"-testcoinbasematurityrolling={START}:{ENFORCE}:{DEPTH}",
            f"-testcoinbasematurityrollingexpiry={self.expiry}",
            "-checkmempool=1",
        ]
        self.restart_node(0, extra_args=self.args)

    def block_time(self, height):
        return self.genesis_time + 600 * height

    def block(self, txs=None):
        node = self.nodes[0]
        height = node.getblockcount() + 1
        tip = node.getbestblockhash()
        block = create_block(int(tip, 16), create_coinbase(
            height, script_pubkey=self.mini.get_output_script()),
            self.block_time(height), height=height, txlist=txs)
        add_witness_commitment(block)
        block.solve()
        return block

    def submit(self, block, expected=None):
        assert_equal(self.nodes[0].submitblock(block.serialize().hex()), expected)

    def mine_to(self, height):
        while self.nodes[0].getblockcount() < height:
            self.submit(self.block())

    def spend(self, height):
        node = self.nodes[0]
        self.mini.rescan_utxos()
        txid = node.getblock(node.getblockhash(height))["tx"][0]
        return self.mini.create_self_transfer(utxo_to_spend=self.mini.get_utxo(txid=txid))

    def deployment(self, blockhash=None):
        node = self.nodes[0]
        info = node.getdeploymentinfo(blockhash) if blockhash else node.getdeploymentinfo()
        return info["deployments"]["rolling_coinbase_maturity"]

    def run_test(self):
        node = self.nodes[0]
        self.mini = MiniWallet(node)

        self.log.info("The expiry boundary falls on the expected block")
        self.mine_to(310)
        assert_equal(node.getblockheader(node.getblockhash(EXPIRY_PARENT - 1))["mediantime"], self.expiry - 600)
        assert_equal(node.getblockheader(node.getblockhash(EXPIRY_PARENT))["mediantime"], self.expiry)

        self.log.info("Deployment reports the expiry, and the last covered coin once it is reached")
        dep = self.deployment()
        assert_equal(dep["type"], "flagday")
        assert_equal(dep["height"], ENFORCE)
        assert_equal(dep["coinbase_start_height"], START)
        assert_equal(dep["coinbase_expiry_time"], self.expiry)
        assert_equal(dep["coinbase_end_height"], END - 1)
        assert_equal(dep["maturity"], DEPTH)
        assert_equal(dep["active"], False)
        before = self.deployment(node.getblockhash(EXPIRY_PARENT - 1))
        assert_equal(before["coinbase_expiry_time"], self.expiry)
        assert "coinbase_end_height" not in before
        assert_equal(self.deployment(node.getblockhash(EXPIRY_PARENT))["coinbase_end_height"], END - 1)

        self.log.info("Before enforcement a covered reward spends at 100, in mempool and in a block")
        pre = self.spend(START)  # depth 111: past 100, far short of DEPTH
        node.sendrawtransaction(pre["hex"])  # policy follows consensus: no early hold
        self.submit(self.block(txs=[pre["tx"]]))

        self.log.info("From enforcement covered rewards need the full depth")
        self.mine_to(ENFORCE)
        assert_equal(self.deployment()["active"], True)
        covered = self.spend(START + 1)  # depth ~120 < DEPTH
        assert_raises_rpc_error(-26, "premature-spend-of-coinbase",
                                node.sendrawtransaction, covered["hex"])
        self.submit(self.block(txs=[covered["tx"]]), "bad-txns-premature-spend-of-coinbase")

        self.log.info("The last reward mined before the expiry is covered, the first one after it is not")
        last_covered = self.spend(END - 1)
        assert_raises_rpc_error(-26, "premature-spend-of-coinbase",
                                node.sendrawtransaction, last_covered["hex"])
        self.submit(self.block(txs=[last_covered["tx"]]), "bad-txns-premature-spend-of-coinbase")
        first_free = self.spend(END)
        node.sendrawtransaction(first_free["hex"])
        self.submit(self.block(txs=[first_free["tx"]]))

        self.log.info("Rewards from before the start height are untouched too")
        old = self.spend(START - 1)
        node.sendrawtransaction(old["hex"])
        self.submit(self.block(txs=[old["tx"]]))

        self.log.info("The rule survives a restart")
        self.restart_node(0, extra_args=self.args)
        self.mini.rescan_utxos()
        assert_equal(self.deployment()["active"], True)
        assert_equal(self.deployment()["coinbase_end_height"], END - 1)
        still_covered = self.spend(START + 1)
        assert_raises_rpc_error(-26, "premature-spend-of-coinbase",
                                node.sendrawtransaction, still_covered["hex"])

        self.log.info("Exact depth boundary: DEPTH-1 fails, DEPTH succeeds")
        target = START + 1 + DEPTH  # first height whose block may spend coin START+1
        self.mine_to(target - 2)  # next block: depth DEPTH-1
        too_early = self.spend(START + 1)
        self.submit(self.block(txs=[too_early["tx"]]), "bad-txns-premature-spend-of-coinbase")
        self.mine_to(target - 1)  # next block height == target: depth DEPTH
        at_depth = self.spend(START + 1)
        self.submit(self.block(txs=[at_depth["tx"]]))

        self.log.info("The last covered reward keeps its full depth long after the expiry")
        self.mine_to(END - 1 + DEPTH - 2)  # next block: depth DEPTH-1
        early = self.spend(END - 1)
        assert_raises_rpc_error(-26, "premature-spend-of-coinbase",
                                node.sendrawtransaction, early["hex"])
        self.submit(self.block(txs=[early["tx"]]), "bad-txns-premature-spend-of-coinbase")
        self.mine_to(END - 1 + DEPTH - 1)  # next block: depth DEPTH
        unlocked = self.spend(END - 1)
        node.sendrawtransaction(unlocked["hex"])
        self.submit(self.block(txs=[unlocked["tx"]]))


if __name__ == '__main__':
    RollingMaturityEndBoundTest(__file__).main()
