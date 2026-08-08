import contextlib
import io
import unittest
from unittest import mock

import drone_operator


class ArgumentTests(unittest.TestCase):
    def test_every_documented_action_parses(self):
        actions = [
            "check",
            "status",
            "set-sim",
            "set-real",
            "match-sim",
            "match-dry",
            "match-first",
            "match-next",
            "classmate-sim",
            "classmate-real",
            "classmate-dry",
            "classmate-manual-sim",
            "classmate-manual-real",
            "classmate-manual-dry",
            "classmate-first",
            "classmate-next",
            "stop-test",
            "stop-match",
            "stop-classmate",
            "stop-classmate-manual",
        ]
        for action in actions:
            with self.subTest(action=action):
                self.assertEqual(
                    drone_operator.parse_arguments([action]).action, action
                )

    def test_test_height_defaults_to_three(self):
        args = drone_operator.parse_arguments(["test-sim"])
        self.assertEqual(args.altitude, 3.0)

    def test_test_height_accepts_twenty(self):
        args = drone_operator.parse_arguments(["test-real", "20"])
        self.assertEqual(args.altitude, 20.0)

    def test_non_test_action_rejects_extra_argument(self):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                drone_operator.parse_arguments(["status", "7"])


class CommandTests(unittest.TestCase):
    @mock.patch.object(drone_operator, "run_remote")
    @mock.patch.object(drone_operator, "build_status")
    def test_stale_build_is_rebuilt_and_rechecked(self, status, run_remote):
        status.side_effect = [(False, "newer input: source.cpp"), (True, "current")]
        run_remote.return_value.returncode = 0
        run_remote.return_value.stdout = "tests passed"
        run_remote.return_value.stderr = ""
        drone_operator.ensure_build_current("test")
        self.assertEqual(status.call_count, 2)
        self.assertEqual(
            run_remote.call_args.args[0],
            "/opt/iking/portable_performance_test/scripts/build.sh",
        )

    @mock.patch.object(drone_operator, "run_foreground", return_value=0)
    @mock.patch.object(drone_operator, "ensure_build_current")
    @mock.patch.object(drone_operator, "preflight")
    def test_sim_performance_command(self, preflight, build, foreground):
        self.assertEqual(drone_operator.run_performance("sim", 3.0), 0)
        preflight.assert_called_once_with("sim")
        build.assert_called_once_with("test")
        command = foreground.call_args.args[0]
        self.assertIn("--environment sim", command)
        self.assertIn("--site-radius 20", command)
        self.assertIn("--site-altitude-limit 20", command)
        self.assertNotIn("--confirm-battery-ready", command)

    @mock.patch.object(drone_operator, "run_foreground", return_value=0)
    @mock.patch.object(drone_operator, "ensure_build_current")
    @mock.patch.object(drone_operator, "preflight")
    def test_real_performance_command(self, preflight, build, foreground):
        self.assertEqual(drone_operator.run_performance("real", 20.0), 0)
        preflight.assert_called_once_with("real")
        command = foreground.call_args.args[0]
        self.assertIn("--altitude 20", command)
        self.assertIn("--confirm-battery-ready", command)

    def test_performance_rejects_out_of_range_height(self):
        with self.assertRaisesRegex(RuntimeError, "1 到 20"):
            drone_operator.run_performance("sim", 20.1)

    @mock.patch.object(drone_operator.ProxyTunnel, "close")
    @mock.patch.object(drone_operator.ProxyTunnel, "start")
    @mock.patch.object(drone_operator, "run_foreground", return_value=0)
    @mock.patch.object(drone_operator, "ensure_build_current")
    @mock.patch.object(drone_operator, "preflight")
    def test_match_execute_checks_simulation_and_opens_vision_tunnel(
        self, preflight, build, foreground, proxy_start, proxy_close
    ):
        self.assertEqual(drone_operator.run_match(False), 0)
        preflight.assert_called_once_with("sim")
        build.assert_called_once_with("match")
        proxy_start.assert_called_once_with("https://openrouter.ai/")
        proxy_close.assert_called_once()
        self.assertIn("./scripts/run.sh --execute", foreground.call_args.args[0])
        self.assertIn("HTTPS_PROXY=http://127.0.0.1:18088", foreground.call_args.args[0])

    @mock.patch.object(drone_operator, "run_foreground", return_value=0)
    @mock.patch.object(drone_operator, "ensure_build_current")
    @mock.patch.object(drone_operator, "ensure_no_controllers")
    def test_match_dry_run_is_read_only(
        self, no_controllers, build, foreground
    ):
        self.assertEqual(drone_operator.run_match(True), 0)
        no_controllers.assert_called_once()
        build.assert_called_once_with("match")
        self.assertIn("./scripts/run.sh --dry-run", foreground.call_args.args[0])

    @mock.patch.object(drone_operator, "run_remote")
    def test_manual_match_round_commands(self, run_remote):
        run_remote.return_value.returncode = 0
        self.assertEqual(drone_operator.trigger_match_round("first"), 0)
        self.assertIn("trigger-round.sh first", run_remote.call_args.args[0])
        self.assertEqual(drone_operator.trigger_match_round("next"), 0)
        self.assertIn("trigger-round.sh next", run_remote.call_args.args[0])

    def test_manual_match_round_rejects_unknown_name(self):
        with self.assertRaisesRegex(RuntimeError, "未知"):
            drone_operator.trigger_match_round("third")

    @mock.patch.object(drone_operator, "run_foreground", return_value=0)
    @mock.patch.object(drone_operator, "ensure_no_controllers")
    def test_classmate_dry_run_preserves_original_command(
        self, no_controllers, foreground
    ):
        self.assertEqual(drone_operator.run_classmate("sim", True), 0)
        no_controllers.assert_called_once()
        self.assertEqual(
            foreground.call_args.args[0],
            "cd /opt/iking/match_agent && exec ./match --dry-run",
        )

    @mock.patch.object(drone_operator.ProxyTunnel, "close")
    @mock.patch.object(drone_operator.ProxyTunnel, "start")
    @mock.patch.object(drone_operator, "run_foreground", return_value=0)
    @mock.patch.object(drone_operator, "preflight")
    def test_classmate_execute_preserves_original_command(
        self, preflight, foreground, proxy_start, proxy_close
    ):
        self.assertEqual(drone_operator.run_classmate("sim", False), 0)
        preflight.assert_called_once_with("sim")
        proxy_start.assert_called_once()
        proxy_close.assert_called_once()
        command = foreground.call_args.args[0]
        self.assertIn("exec ./match --execute", command)
        self.assertNotIn("--origin", command)

    @mock.patch.object(drone_operator.ProxyTunnel, "close")
    @mock.patch.object(drone_operator.ProxyTunnel, "start")
    @mock.patch.object(drone_operator, "run_foreground", return_value=0)
    @mock.patch.object(drone_operator, "install_manual_api_key")
    @mock.patch.object(drone_operator, "ensure_build_current")
    @mock.patch.object(drone_operator, "preflight")
    def test_classmate_manual_execute_uses_isolated_program(
        self, preflight, build, install_key, foreground, proxy_start, proxy_close
    ):
        self.assertEqual(drone_operator.run_classmate_manual("sim", False), 0)
        preflight.assert_called_once_with("sim")
        build.assert_called_once_with("classmate-manual")
        install_key.assert_called_once_with()
        proxy_start.assert_called_once()
        proxy_close.assert_called_once()
        command = foreground.call_args.args[0]
        self.assertIn("cd /opt/iking/match_agent_manual", command)
        self.assertIn("./scripts/run.sh --execute --confirm-preflight", command)
        self.assertNotIn("cd /opt/iking/match_agent &&", command)

    @mock.patch.object(drone_operator.subprocess, "run")
    @mock.patch.object(drone_operator, "load_local_api_key", return_value="test-key")
    def test_manual_api_key_uses_stdin_not_command_line(self, load_key, run):
        run.return_value.returncode = 0
        run.return_value.stdout = ""
        run.return_value.stderr = ""
        drone_operator.install_manual_api_key()
        load_key.assert_called_once_with()
        self.assertEqual(run.call_args.kwargs["input"], "test-key\n")
        command_arguments = " ".join(run.call_args.args[0])
        self.assertNotIn("test-key", command_arguments)
        self.assertIn("dashscope_api_key", command_arguments)

    @mock.patch.object(drone_operator, "run_foreground", return_value=0)
    @mock.patch.object(drone_operator, "ensure_build_current")
    @mock.patch.object(drone_operator, "ensure_no_controllers")
    def test_classmate_manual_dry_run_is_read_only(
        self, no_controllers, build, foreground
    ):
        self.assertEqual(drone_operator.run_classmate_manual("sim", True), 0)
        no_controllers.assert_called_once()
        build.assert_called_once_with("classmate-manual")
        self.assertEqual(
            foreground.call_args.args[0],
            "cd /opt/iking/match_agent_manual && exec ./scripts/run.sh --dry-run",
        )

    @mock.patch.object(drone_operator, "run_remote")
    def test_manual_classmate_round_commands(self, run_remote):
        run_remote.return_value.returncode = 0
        self.assertEqual(drone_operator.trigger_classmate_round("first"), 0)
        self.assertIn("match_agent_manual", run_remote.call_args.args[0])
        self.assertIn("trigger-round.sh first", run_remote.call_args.args[0])
        self.assertEqual(drone_operator.trigger_classmate_round("next"), 0)
        self.assertIn("trigger-round.sh next", run_remote.call_args.args[0])

    def test_manual_classmate_round_rejects_unknown_name(self):
        with self.assertRaisesRegex(RuntimeError, "未知"):
            drone_operator.trigger_classmate_round("third")

    @mock.patch.object(drone_operator, "run_remote")
    def test_stop_commands_target_exact_executables(self, run_remote):
        run_remote.return_value.returncode = 0
        for name, executable in drone_operator.CONTROLLERS.items():
            with self.subTest(name=name):
                drone_operator.stop_controller(name)
                self.assertIn(executable, run_remote.call_args.args[0])


if __name__ == "__main__":
    unittest.main()
