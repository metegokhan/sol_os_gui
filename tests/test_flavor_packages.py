import importlib.util
from pathlib import Path
import sys
import unittest


REPOSITORY = Path(__file__).resolve().parents[1]
GENERATOR_PATH = REPOSITORY / "scripts" / "generate_flavor_config.py"
SPEC = importlib.util.spec_from_file_location("generate_flavor_config", GENERATOR_PATH)
assert SPEC is not None and SPEC.loader is not None
generate_flavor_config = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = generate_flavor_config
SPEC.loader.exec_module(generate_flavor_config)


class FlavorPackagesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.catalog = generate_flavor_config.load_catalog(
            REPOSITORY / "packages" / "solar_os_packages.toml"
        )

    def resolve(self, flavor):
        return generate_flavor_config.load_flavor(
            REPOSITORY / "flavors" / f"{flavor}.toml",
            self.catalog,
        )

    def test_granular_group_ownership(self):
        self.assertEqual(
            set(self.catalog.group_defs["maintenance_jobs"].members),
            {"job_log", "job_batmon"},
        )
        self.assertEqual(
            set(self.catalog.group_defs["hardware_jobs"].members),
            {"job_bridge", "job_daq", "job_sump"},
        )
        self.assertEqual(
            set(self.catalog.group_defs["writing"].members),
            {"app_reader", "app_writer", "app_files", "app_notes"},
        )
        self.assertEqual(
            set(self.catalog.group_defs["utils"].members),
            {"app_clock", "app_calc", "app_plot", "app_logic", "app_sheet"},
        )
        self.assertEqual(
            set(self.catalog.group_defs["retro"].members),
            {"app_gameboy"},
        )
        self.assertIn("service_synth", self.catalog.group_defs["system"].members)
        self.assertIn("service_synth", self.catalog.group_defs["audio"].members)
        self.assertEqual(
            self.catalog.package_defs["service_synth"].depends,
            ("service_audio",),
        )
        self.assertEqual(
            self.catalog.package_defs["app_gameboy"].depends,
            ("service_synth",),
        )

    def test_writerdeck_selects_writing_without_hardware_jobs_or_utils(self):
        name, _, groups, packages = self.resolve("writerdeck")

        self.assertEqual(name, "writerdeck")
        self.assertTrue(groups["writing"])
        self.assertTrue(groups["maintenance_jobs"])
        self.assertFalse(groups["hardware_jobs"])
        self.assertFalse(groups["utils"])
        for package in ("app_reader", "app_writer", "app_files", "app_notes", "job_log"):
            self.assertTrue(packages[package], package)
        for package in (
            "job_bridge",
            "job_daq",
            "job_sump",
            "app_clock",
            "app_calc",
            "app_plot",
            "app_logic",
            "app_sheet",
        ):
            self.assertFalse(packages[package], package)

    def test_rover_flavors_share_an_expansion_capable_baseline(self):
        rover_name, _, rover_groups, rover_packages = self.resolve("rover")
        python_name, _, python_groups, python_packages = self.resolve("rover-python")
        lua_name, _, lua_groups, lua_packages = self.resolve("rover-lua")

        self.assertEqual(rover_name, "rover")
        self.assertEqual(python_name, "rover-python")
        self.assertEqual(lua_name, "rover-lua")
        for groups in (rover_groups, python_groups, lua_groups):
            self.assertTrue(groups["expansions"])
            self.assertFalse(groups["maintenance_apps"])
            self.assertFalse(groups["maintenance_jobs"])
            self.assertFalse(groups["hardware_jobs"])
            self.assertFalse(groups["audio"])
            self.assertFalse(groups["agent"])
            self.assertTrue(groups["net"])
            self.assertTrue(groups["media"])
            self.assertTrue(groups["writing"])
            self.assertTrue(groups["utils"])
        for packages in (rover_packages, python_packages, lua_packages):
            self.assertTrue(packages["service_expansion"])
            self.assertTrue(packages["app_files"])
            self.assertFalse(packages["service_ota"])
            self.assertFalse(packages["service_docs"])
            self.assertTrue(packages["job_log"])
            self.assertTrue(packages["job_bridge"])
            self.assertFalse(packages["job_batmon"])
            self.assertFalse(packages["job_daq"])
            self.assertFalse(packages["job_sump"])
            self.assertFalse(packages["app_agent"])
            self.assertFalse(packages["app_logic"])

        self.assertTrue(rover_groups["games"])
        self.assertTrue(rover_packages["app_invaders"])
        self.assertFalse(rover_packages["app_python"])
        self.assertFalse(rover_packages["app_lua"])
        self.assertFalse(python_groups["games"])
        self.assertFalse(python_packages["app_invaders"])
        self.assertTrue(python_groups["python"])
        self.assertTrue(python_packages["app_python"])
        self.assertFalse(python_packages["app_lua"])
        self.assertFalse(lua_groups["games"])
        self.assertFalse(lua_packages["app_invaders"])
        self.assertTrue(lua_groups["lua"])
        self.assertTrue(lua_packages["app_lua"])
        self.assertFalse(lua_packages["app_python"])

        python_difference = {
            package
            for package in rover_packages
            if rover_packages[package] != python_packages[package]
        }
        lua_difference = {
            package
            for package in rover_packages
            if rover_packages[package] != lua_packages[package]
        }
        self.assertEqual(
            python_difference,
            {
                "app_invaders",
                "service_playground",
                "service_script_runner",
                "app_python",
                "app_playground",
            },
        )
        self.assertEqual(
            lua_difference,
            {
                "app_invaders",
                "service_playground",
                "service_script_runner",
                "app_lua",
                "app_playground",
            },
        )

    def test_existing_flavors_preserve_hardware_job_selection(self):
        for flavor in ("core", "full", "netrunner"):
            with self.subTest(flavor=flavor):
                _, _, groups, packages = self.resolve(flavor)
                self.assertTrue(groups["hardware_jobs"])
                self.assertTrue(packages["job_bridge"])
                self.assertTrue(packages["job_daq"])
                self.assertTrue(packages["job_sump"])

        _, _, full_groups, full_packages = self.resolve("full")
        self.assertTrue(full_groups["writing"])
        for package in ("app_reader", "app_writer", "app_files", "app_notes"):
            self.assertTrue(full_packages[package], package)

    def test_retro_is_a_strict_superset_of_full(self):
        _, _, full_groups, full_packages = self.resolve("full")
        name, _, retro_groups, retro_packages = self.resolve("retro")

        self.assertEqual(name, "retro")
        self.assertFalse(full_groups["retro"])
        self.assertTrue(retro_groups["retro"])
        self.assertFalse(full_packages["app_gameboy"])
        self.assertTrue(retro_packages["app_gameboy"])
        for package, enabled in full_packages.items():
            if enabled:
                self.assertTrue(retro_packages[package], package)


if __name__ == "__main__":
    unittest.main()
