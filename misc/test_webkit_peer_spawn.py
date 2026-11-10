#!/usr/bin/env python3
"""Contracts for the unique-peer WebKit spawn handshake."""
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
        self.assertIn("posix_spawn(&child, path, NULL, &attr, argv, envp)", HOOKS)
        self.assertIn("posix_spawnattr_set_registered_ports_np", HOOKS)
        self.assertIn("xpc_connection_create_from_endpoint", HOOKS)
        self.assertIn("xpc_pipe_simpleroutine", HOOKS)
        self.assertIn("MACWS_WK_RV_POP_MS 3000", HOOKS)
        self.assertIn("MACWS_WK_RV_GRACE_MS 5000", HOOKS)
        self.assertIn("macws_wk_prewarm_if_needed", HOOKS)
        self.assertIn("wk-peer pop-timeout", HOOKS)
        self.assertIn("wk-peer grace-timeout", HOOKS)
        self.assertIn("recv kept until grace", HOOKS)
        self.assertNotIn("macws_wk_wait_for_peer", HOOKS)
        self.assertNotIn("lookup-timeout", HOOKS)
        self.assertIn("macws_wk_leaf_for_public_name", HOOKS)
        # Success path is attempted for public WebKit / SandboxBroker
        # names before any TSV-hit add_bundle / create_fn return.
        spawn = HOOKS.index("macws_xpc_spawn_webkit_peer(activate, leaf, targetq)")
        fallback = HOOKS.index("wk-peer fallback activate=", spawn)
        tsv = HOOKS.index("macws_register_xpc_app_proxy_bundle(row->relativeProxy);")
        add = HOOKS.index("macws_register_webkit_bundle_if_needed(leaf);", fallback)
        self.assertLess(spawn, fallback)
        self.assertLess(spawn, tsv)
        self.assertLess(fallback, tsv)
        self.assertLess(fallback, add)
        # Do not spawn again when the hook re-enters on a macosbooter remap.
        self.assertIn("macws_wk_leaf_for_public_name", HOOKS)
        self.assertIn("Not macosbooter.*", HOOKS)

    def test_spawn_covers_public_webkit_names_only(self):
        start = HOOKS.index("macws_wk_leaf_for_public_name")
        end = HOOKS.index("macws_wk_activate_for_public_name")
        chunk = HOOKS[start:end]
        self.assertIn("WEBKIT_WEBCONTENT_SERVICE)", chunk)
        self.assertIn("WEBKIT_NETWORKING_SERVICE)", chunk)
        self.assertIn("WEBKIT_GPU_SERVICE)", chunk)
        self.assertIn("SAFARI_SANDBOX_BROKER_SERVICE)", chunk)
        self.assertNotIn("WEBKIT_WEBCONTENT_SERVICE_NEW", chunk)

    def test_helper_listens_only_on_validated_unique_name(self):
        self.assertIn("macws_wk_peer_name_valid(peer)", HOOKS)
        self.assertIn("unique peer anonymous listener", HOOKS)
        self.assertIn("xpc_endpoint_create", HOOKS)
        self.assertIn("macws_wk_send_peer_endpoint", HOOKS)
        # Must not check in the public remapped name on this path,
        # and must not use a named Mach LISTENER (not look-up-able).
        start = HOOKS.index("Anonymous peer via xpc_connection_create(NULL)")
        end = HOOKS.index("macws_xpc_main_raw(macws_webkit_xpc_main_connection);", start)
        chunk = HOOKS[start:end]
        self.assertNotIn("WEBKIT_WEBCONTENT_SERVICE_NEW", chunk)
        self.assertNotIn("bootstrap_check_in", chunk)
        self.assertNotIn("XPC_CONNECTION_MACH_SERVICE_LISTENER", chunk)
        self.assertIn("macws_xpc_connection_create_raw", chunk)
        # Treat create(NULL) as an anonymous LISTENER: accept
        # XPC_TYPE_CONNECTION, hand that incoming conn to WebKit, and
        # drain main with dispatch_main — do not call NSApp, and do not
        # hand the listener itself to the stock xpc_main handler.
        self.assertIn("XPC_TYPE_CONNECTION", chunk)
        self.assertIn("macws_webkit_xpc_main_connection(incoming)", chunk)
        self.assertNotIn("macws_webkit_xpc_main_connection(listener)", chunk)
        self.assertIn("listening for incoming", chunk)
        self.assertIn("entering dispatch_main", chunk)
        self.assertNotIn('sel_registerName("sharedApplication")', chunk)
        self.assertNotIn('sel_registerName("run")', chunk)
        self.assertLess(
            chunk.index("XPC_TYPE_CONNECTION"),
            chunk.index("xpc_endpoint_create"))
        self.assertLess(
            chunk.index("xpc_connection_resume(listener)"),
            chunk.index("xpc_endpoint_create"))
        self.assertLess(
            chunk.index("xpc_endpoint_create"),
            chunk.index("entering dispatch_main"))
        self.assertIn("macws_xpc_remember_app_conn(listener, peer)", chunk)
        self.assertIn("macws_xpc_remember_app_conn(incoming, peer_for_accept)", chunk)
        self.assertIn("com.macwsguide.wk.peer-listener", chunk)
        self.assertIn("macws_xpc_connection_set_event_handler_raw(listener", chunk)
        self.assertIn("xpc_dictionary_set_mach_send", HOOKS)
        self.assertIn("xpc_endpoint_copy_listener_port_4sim", HOOKS)
        self.assertIn("xpc_endpoint_create_mach_port_4sim", HOOKS)
        self.assertIn("wk-peer recv-endpoint", HOOKS)
        self.assertIn("kept_port=", HOOKS)
        self.assertIn("p->ep_port = pport", HOOKS)
        self.assertIn('epsrc = "orig"', HOOKS)
        # Prefer the authentic xpc_endpoint_create object; keep the
        # 4sim send right; use the rebuilt endpoint only as fallback.
        recv = HOOKS[HOOKS.index("Prefer the authentic xpc_endpoint_create"):HOOKS.index("wk-peer ready child=")]
        self.assertIn("p->ep_port = pport", recv)
        self.assertIn('epsrc = "orig"', recv)
        self.assertIn('epsrc = "rebuilt"', recv)
        self.assertIn("pport = MACH_PORT_NULL", recv)
        # Must retain the original object and only assign ep = rebuilt
        # in the fallback branch after the orig path.
        self.assertLess(recv.index("xpc_retain(ep)"), recv.index('epsrc = "orig"') + 80)
        self.assertGreater(recv.index("ep = rebuilt"), recv.index('epsrc = "orig"'))
        self.assertGreater(recv.index("ep = rebuilt"), recv.index("else if (rebuilt)"))
        self.assertLess(
            chunk.index("macws_xpc_remember_app_conn(incoming, peer_for_accept)"),
            chunk.index("macws_webkit_xpc_main_connection(incoming)"))
        self.assertIn("wk-peer xpc-event", HOOKS)

    def test_rendezvous_is_fail_fast_and_not_bootstrap_lookup(self):
        start = HOOKS.index("#define MACWS_WK_RV_POP_MS 3000")
        end = HOOKS.index("macws_xpc_create_application_service")
        chunk = HOOKS[start:end]
        self.assertIn("xpc_pipe_receive", chunk)
        self.assertIn("wk-peer pop-timeout", chunk)
        self.assertIn("wk-peer grace-timeout", chunk)
        self.assertIn("macws_wk_prewarm_if_needed", chunk)
        self.assertIn("MACWS_WK_POOL_WEBCONTENT 8", chunk)
        self.assertIn("MACWS_WK_RV_SLOT", chunk)
        self.assertNotIn("usleep(20 * 1000)", chunk)
        self.assertNotIn("endpoint-timeout", chunk)
        # Caller-thread pop is a single cap, not N serial long waits.
        # Recv stays alive until grace so a slower WebContent can send.
        self.assertIn("MACWS_WK_RV_POP_MS 3000", chunk)
        self.assertIn("MACWS_WK_RV_GRACE_MS 5000", chunk)
        self.assertLess(3000, 5000)
        self.assertIn("recv kept until grace", chunk)
        # Parent publishes the unique name; helper may look it up after
        # exec if slot 1 did not survive the 0755 proxy. Not a 2 s loop
        # on Safari's application-service thread, and not macosbooter.
        self.assertIn("macws_bootstrap_register_raw", chunk)
        self.assertIn("macws_wk_rendezvous_send", chunk)
        self.assertIn("macws_bootstrap_look_up_raw", chunk)
        self.assertNotIn("WEBKIT_WEBCONTENT_SERVICE_NEW);", chunk)

    def test_unique_peer_skips_shared_cache_set_instance(self):
        self.assertIn("MACWS_XPC_APP_CONN_MAX 128", HOOKS)
        self.assertIn("wk-peer skip-set-instance", HOOKS)
        metal = (ROOT / "libmachook" / "Metal_hooks.x").read_text()
        self.assertIn("hooked_xpc_connection_set_instance", metal)
        self.assertIn("_xpc_connection_set_instance", metal)
        self.assertIn("wk-peer resume", metal)
        self.assertIn("wk-peer send-message", metal)
        self.assertIn("wk-peer reply", metal)
        self.assertIn("wk-peer set-event-handler", HOOKS)

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

    def test_unique_peer_skips_managed_xpc_flags(self):
        # posix_spawn envp must not advertise Application-type 8019.
        start = HOOKS.index("static char **macws_wk_peer_envp")
        end = HOOKS.index("static void macws_wk_peer_envp_free")
        envp = HOOKS[start:end]
        self.assertIn('strdup("XPC_FLAGS=0")', envp)
        self.assertNotIn('strdup("XPC_FLAGS=8019")', envp)
        # Fallback still reapplies 8019; the unique path must not.
        self.assertIn("macws_webkit_reapply_packed_xpc_flags", HOOKS)
        self.assertIn("if (!uniquePeer)", HOOKS)
        self.assertIn("Do not advertise Application-type", HOOKS)
        self.assertIn("unique peer cleared XPC_FLAGS", HOOKS)
        self.assertIn("skip 8019 reconstitution", HOOKS)
        start = HOOKS.index("Anonymous peer via xpc_connection_create(NULL)")
        end = HOOKS.index("macws_xpc_main_raw(macws_webkit_xpc_main_connection);", start)
        chunk = HOOKS[start:end]
        self.assertNotIn("macws_webkit_reapply_packed_xpc_flags", chunk)
        # Proxy must not reconstitute 8019 when MACWS_WK_PEER is set.
        self.assertIn("Do not reconstitute XPC_FLAGS=8019", PROXY)
        self.assertIn('uniqueFlags[] = "XPC_FLAGS=0"', PROXY)
        self.assertIn("normalizeUniqueFlags", PROXY)
        self.assertIn("unique_peer=", PROXY)
        self.assertIn("MACWS_WK_PEER", PROXY)


if __name__ == "__main__":
    unittest.main()
