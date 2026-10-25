Consensus
---------

- Rolling coinbase maturity (1 year). On mainnet, from block 979920, every
  generated coin mined at height 973440 or later must be buried 52560 blocks
  (365 days of 144 blocks) before it can be spent. Each reward counts from its
  own block, so exactly one reward unlocks per block, the first at 1026000.

  This replaces the shared release of the existing long coinbase maturity
  window: the rewards it holds (973440 to 979919) are no longer all released
  together at block 979920. Through block 979919 the rules are unchanged.
  Rewards mined before 973440 keep ordinary 100-block maturity.

  Coverage ends with RDTS. A reward mined in a block whose parent's median
  time past has reached the RDTS expiry (2027 September 1st 00:00 UTC on
  mainnet) needs only the ordinary 100-block maturity. This is the same
  boundary RDTS uses for its own rules, and the same one knots#468 uses for its
  180-day maturity. The end is set by when a reward was mined, not when it is
  spent: every reward covered before the expiry still waits its full 52560
  blocks, so nothing is released early. If RDTS is extended, the coverage
  follows the new expiry.

  Upgrading nodes check every block from 979920 again using the existing
  chainstate revalidation, under the marker `rolling_coinbase_maturity_1y`.
  A node that upgrades before block 979920 rewinds nothing. A pruned node that
  upgrades later and no longer has those blocks must reindex.

  `getdeploymentinfo` reports the rule as `rolling_coinbase_maturity`, with
  the expiry in `coinbase_expiry_time` and, once the chain has reached it, the
  last covered reward in `coinbase_end_height`; and
  `getblocktemplate` lists `rolling_coinbase_maturity` in `rules` once it is
  active. Wallets and the GUI show the maturity that applies to each reward.
  Testnet4, signet and regtest are unchanged; regtest can opt in with
  `-testcoinbasematurityrolling=<start_height>:<enforce_height>:<maturity>`, and
  bind it to an RDTS expiry with `-testcoinbasematurityrollingexpiry=<time>`.
