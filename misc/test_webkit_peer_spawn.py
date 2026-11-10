#!/usr/bin/env python3
"""Contracts for the net20 unique-peer WebKit spawn handshake."""
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOOKS = (ROOT / "libmachook" / "mac_hooks.m").read_text()
PROXY = (ROOT / "ViewBridgeChrootProxy" / "main.c").read_text()
ENV_H = (ROOT / "include" / "macws_chroot_environment.h").read_text()


class WebKitPeerSpawnContract(unittest.TestCase):
    def test_unique_peer_prefix_is_not_public_macosbooter(self):
        self.assertIn('#define MACWS_WK_PEER_PREFIX "com.macwsguide.wk.i."', HOOKS)
        self.assertIn("macws_wk_peer_name_valid", HOOKS)
        self.assertIn("MACWS_WK_PEER_HEX_LEN 32", HOOKS)
        self.assertNotIn(
            'MACWS_WK_PEER_PREFIX "com.apple.macosbooter.WebKit.', HOOKS)

    def test_client_posix_spawns_proxy_and_skips_add_bundle_on_success(self):
        self.assertIn("macws_xpc_spawn_webkit_peer", HOOKS)
        self.assertIn("posix_spawn(&child, path, NULL, NULL, argv, envp)", HOOKS)
        # Success path returns before register/add_bundle.
        spawn = HOOKS.index("macws_xpc_spawn_webkit_peer(activate, leaf, targetq)")
        fallback = HOOKS.index("wk-peer fallback activate=", spawn)
        add = HOOKS.index("macws_register_webkit_bundle_if_needed(leaf);", fallback)
        self.assertLess(spawn, fallback)
        self.assertLess(fallback, add)

    def test_helper_listens_only_on_validated_unique_name(self):
        self.assertIn("macws_wk_peer_name_valid(peer)", HOOKS)
        self.assertIn("XPC_CONNECTION_MACH_SERVICE_LISTENER", HOOKS)
        self.assertIn("unique peer listener", HOOKS)
        # Must not check in the public remapped name on this path.
        start = HOOKS.index("xpc_main WebKit unique peer listener")
        end = HOOKS.index("macws_xpc_main_raw(macws_webkit_xpc_main_connection);", start)
        chunk = HOOKS[start:end]
        self.assertNotIn("WEBKIT_WEBCONTENT_SERVICE_NEW", chunk)
        self.assertNotIn("bootstrap_check_in", chunk)

    def test_sandbox_allows_unique_peer_prefix(self):
        self.assertIn('!strncmp(name, "com.macwsguide.wk.i.", 20)', HOOKS)
        metal = (ROOT / "libmachook" / "Metal_hooks.x").read_text()
        self.assertIn('!strncmp(svc, "com.macwsguide.wk.i.", 20)', metal)

    def test_proxy_reuses_inherited_host_root_instead_of_exit_118(self):
        self.assertIn("MacWSAdoptInheritedChrootRootEnvironment", PROXY)
        self.assertIn("MacWSExit(118)", PROXY)
        self.assertLess(
            PROXY.index("MacWSAdoptInheritedChrootRootEnvironment"),
            PROXY.rindex("MacWSExit(118)"))
        self.assertIn("MacWSAdoptInheritedChrootRootEnvironment", ENV_H)
        self.assertIn("inherited[0] != '/'", ENV_H)
        self.assertIn("!inherited[1]", ENV_H)


if __name__ == "__main__":
    unittest.main()
