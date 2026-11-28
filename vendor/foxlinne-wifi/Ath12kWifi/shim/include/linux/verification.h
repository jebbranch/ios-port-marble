/* Signed regulatory-database verification needs a native provider when enabled. */
#ifdef CONFIG_CFG80211_REQUIRE_SIGNED_REGDB
#error Native verification for signed regulatory databases is not implemented yet.
#endif
