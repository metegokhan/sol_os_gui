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
