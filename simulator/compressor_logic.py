"""Physical models for a configurable compressed-air station.

This module contains no OPC UA code.  It can therefore be tested or reused by
another transport without coupling the equipment model to communications.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Any


@dataclass
class CompressorState:
    """Current simulated values for one compressor."""

    temperature_celsius: float
    motor_current_amperes: float
    runtime_hours: float = 0.0
    is_running: bool = False
    alarm_active: bool = False


class CompressorModel:
    """Simulate smooth thermal, electrical, and runtime behavior."""

    def __init__(self, configuration: dict[str, Any]) -> None:
        self.configuration = configuration
        self.identifier = str(configuration["id"])
        self.display_name = str(configuration["display_name"])
        self.dispatch_start_percent = float(configuration["dispatch_start_percent"])
        self.rated_flow = float(configuration["rated_flow_m3h"])
        self.rated_power = float(configuration["rated_power_kw"])
        self.manual_run_command = False
        self.sensor_fault_command = False
        self.state = CompressorState(
            temperature_celsius=float(configuration["stopped_temperature_c"]),
            motor_current_amperes=0.0,
        )

    def update(self, automatic_run_request: bool, elapsed_seconds: float) -> None:
        """Advance this compressor by one simulation interval."""

        should_run = automatic_run_request or self.manual_run_command
        self.state.is_running = should_run

        target_temperature = float(
            self.configuration[
                "running_temperature_c" if should_run else "stopped_temperature_c"
            ]
        )
        temperature_rate = float(
            self.configuration[
                "heating_rate_c_per_second"
                if should_run
                else "cooling_rate_c_per_second"
            ]
        )
        self.state.temperature_celsius = _move_toward(
            self.state.temperature_celsius,
            target_temperature,
            temperature_rate * elapsed_seconds,
        )

        target_current = float(self.configuration["running_current_a"]) if should_run else 0.0
        current_rate = float(self.configuration["current_ramp_a_per_second"])
        self.state.motor_current_amperes = _move_toward(
            self.state.motor_current_amperes,
            target_current,
            current_rate * elapsed_seconds,
        )

        if should_run:
            self.state.runtime_hours += elapsed_seconds / 3600.0

    @property
    def delivered_flow(self) -> float:
        """Return current delivered air flow in cubic metres per hour."""

        return self.rated_flow if self.state.is_running else 0.0

    @property
    def power_consumption(self) -> float:
        """Return current power using motor-current ramp as a load proxy."""

        rated_current = float(self.configuration["running_current_a"])
        if rated_current <= 0.0:
            return 0.0
        load_fraction = min(self.state.motor_current_amperes / rated_current, 1.0)
        return self.rated_power * load_fraction


class StationModel:
    """Coordinate demand, compressor dispatch, pressure, and auxiliary equipment."""

    def __init__(self, configuration: dict[str, Any]) -> None:
        station_configuration = configuration["station"]
        self.configuration = station_configuration
        self.compressors = [
            CompressorModel(item) for item in configuration["compressors"]
        ]
        self.elapsed_simulation_seconds = 0.0
        self.air_demand_m3h = float(station_configuration["initial_demand_m3h"])
        self.receiver_pressure_bar = float(
            station_configuration["initial_receiver_pressure_bar"]
        )
        self.network_pressure_bar = self.receiver_pressure_bar
        self.total_power_kw = 0.0
        self.dryer_running = False

    def update(self, elapsed_seconds: float) -> None:
        """Advance the complete station while keeping all changes continuous."""

        self.elapsed_simulation_seconds += elapsed_seconds
        self.air_demand_m3h = self._calculate_demand()
        demand_percent = 100.0 * self.air_demand_m3h / float(
            self.configuration["maximum_demand_m3h"]
        )

        for compressor in self.compressors:
            automatic_request = demand_percent >= compressor.dispatch_start_percent
            compressor.update(automatic_request, elapsed_seconds)

        total_supply = sum(item.delivered_flow for item in self.compressors)
        pressure_change = (
            total_supply - self.air_demand_m3h
        ) * float(self.configuration["pressure_response_bar_per_m3h_second"])
        self.receiver_pressure_bar = min(
            float(self.configuration["maximum_pressure_bar"]),
            max(
                float(self.configuration["minimum_pressure_bar"]),
                self.receiver_pressure_bar + pressure_change * elapsed_seconds,
            ),
        )

        network_drop = self.air_demand_m3h * float(
            self.configuration["network_pressure_drop_bar_per_m3h"]
        )
        target_network_pressure = max(0.0, self.receiver_pressure_bar - network_drop)
        self.network_pressure_bar = _move_toward(
            self.network_pressure_bar,
            target_network_pressure,
            float(self.configuration["network_pressure_response_bar_per_second"])
            * elapsed_seconds,
        )
        self.dryer_running = any(item.state.is_running for item in self.compressors)
        self.total_power_kw = sum(
            item.power_consumption for item in self.compressors
        )

    def _calculate_demand(self) -> float:
        """Create a repeatable, slowly changing industrial demand profile."""

        maximum_demand = float(self.configuration["maximum_demand_m3h"])
        base_fraction = float(self.configuration["demand_base_fraction"])
        slow_wave = float(self.configuration["demand_slow_wave_fraction"]) * math.sin(
            2.0
            * math.pi
            * self.elapsed_simulation_seconds
            / float(self.configuration["demand_slow_period_seconds"])
        )
        fast_wave = float(self.configuration["demand_fast_wave_fraction"]) * math.sin(
            2.0
            * math.pi
            * self.elapsed_simulation_seconds
            / float(self.configuration["demand_fast_period_seconds"])
        )
        return min(maximum_demand, max(0.0, maximum_demand * (base_fraction + slow_wave + fast_wave)))


def _move_toward(current_value: float, target_value: float, maximum_step: float) -> float:
    """Move a numeric value toward a target without overshoot."""

    if current_value < target_value:
        return min(current_value + maximum_step, target_value)
    return max(current_value - maximum_step, target_value)
