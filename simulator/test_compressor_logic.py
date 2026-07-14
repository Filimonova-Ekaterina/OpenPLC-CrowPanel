"""Dependency-free tests for the physical station model."""

from pathlib import Path
import unittest

import yaml

from compressor_logic import StationModel


class StationModelTests(unittest.TestCase):
    """Verify dispatch and smooth physical behavior."""

    def setUp(self) -> None:
        configuration_path = Path(__file__).parent / "configuration" / "equipment.yaml"
        with configuration_path.open("r", encoding="utf-8") as configuration_file:
            self.configuration = yaml.safe_load(configuration_file)

    def test_dispatch_thresholds(self) -> None:
        station = StationModel(self.configuration)
        station._calculate_demand = lambda: 20.0
        station.update(0.1)
        self.assertEqual([True, False, False], [item.state.is_running for item in station.compressors])

        station._calculate_demand = lambda: 45.0
        station.update(0.1)
        self.assertEqual([True, True, False], [item.state.is_running for item in station.compressors])

        station._calculate_demand = lambda: 80.0
        station.update(0.1)
        self.assertEqual([True, True, True], [item.state.is_running for item in station.compressors])

    def test_temperature_current_runtime_and_pressure_change_smoothly(self) -> None:
        station = StationModel(self.configuration)
        station._calculate_demand = lambda: 20.0
        initial_pressure = station.receiver_pressure_bar
        station.update(0.1)
        compressor = station.compressors[0]

        self.assertGreater(compressor.state.temperature_celsius, 25.0)
        self.assertLess(compressor.state.temperature_celsius, 80.0)
        self.assertGreater(compressor.state.motor_current_amperes, 0.0)
        self.assertLess(compressor.state.motor_current_amperes, 20.0)
        self.assertGreater(compressor.state.runtime_hours, 0.0)
        self.assertGreater(station.receiver_pressure_bar, initial_pressure)

    def test_pressure_falls_when_all_compressors_are_stopped(self) -> None:
        station = StationModel(self.configuration)
        station._calculate_demand = lambda: 20.0
        for compressor in station.compressors:
            compressor.dispatch_start_percent = 101.0
        initial_pressure = station.receiver_pressure_bar
        station.update(1.0)
        self.assertLess(station.receiver_pressure_bar, initial_pressure)


if __name__ == "__main__":
    unittest.main()
