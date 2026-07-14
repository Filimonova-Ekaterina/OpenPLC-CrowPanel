"""Alarm evaluation and fault injection for the equipment simulator."""

from __future__ import annotations

import random
from dataclasses import dataclass
from typing import Any

from compressor_logic import CompressorModel, StationModel


@dataclass(frozen=True)
class AlarmTransition:
    """Describe a newly activated or cleared alarm."""

    source_identifier: str
    source_name: str
    alarm_code: str
    message: str
    severity: int
    active: bool


class FaultManager:
    """Latch alarms, evaluate delays, and provide optional sensor corruption."""

    def __init__(self, configuration: dict[str, Any]) -> None:
        self.configuration = configuration
        self.active_alarms: dict[tuple[str, str], AlarmTransition] = {}
        self.high_temperature_durations: dict[str, float] = {}
        self.random_generator = random.Random(
            int(configuration.get("sensor_fault_random_seed", 2026))
        )

    def evaluate(
        self, station: StationModel, elapsed_seconds: float
    ) -> list[AlarmTransition]:
        """Evaluate all alarm rules and return only state transitions."""

        transitions: list[AlarmTransition] = []
        for compressor in station.compressors:
            temperature_threshold = float(self.configuration["high_temperature_c"])
            if compressor.state.temperature_celsius > temperature_threshold:
                self.high_temperature_durations[compressor.identifier] = (
                    self.high_temperature_durations.get(compressor.identifier, 0.0)
                    + elapsed_seconds
                )
            else:
                self.high_temperature_durations[compressor.identifier] = 0.0

            temperature_alarm = (
                self.high_temperature_durations[compressor.identifier]
                >= float(self.configuration["high_temperature_delay_seconds"])
            )
            transitions.extend(
                self._set_alarm(
                    compressor.identifier,
                    compressor.display_name,
                    "HIGH_TEMPERATURE",
                    f"Temperature exceeded {temperature_threshold:.1f} °C",
                    int(self.configuration["high_temperature_severity"]),
                    temperature_alarm,
                )
            )

            current_threshold = float(self.configuration["motor_overload_current_a"])
            transitions.extend(
                self._set_alarm(
                    compressor.identifier,
                    compressor.display_name,
                    "MOTOR_OVERLOAD",
                    f"Motor current exceeded {current_threshold:.1f} A",
                    int(self.configuration["motor_overload_severity"]),
                    compressor.state.motor_current_amperes > current_threshold,
                )
            )
            transitions.extend(
                self._set_alarm(
                    compressor.identifier,
                    compressor.display_name,
                    "SENSOR_FAULT",
                    "Temperature and current sensor values are unreliable",
                    int(self.configuration["sensor_fault_severity"]),
                    compressor.sensor_fault_command,
                )
            )
            compressor.state.alarm_active = self.has_alarm(compressor.identifier)

        station_identifier = str(station.configuration["id"])
        station_name = str(station.configuration["display_name"])
        pressure_threshold = float(self.configuration["high_pressure_bar"])
        transitions.extend(
            self._set_alarm(
                station_identifier,
                station_name,
                "HIGH_PRESSURE",
                f"Receiver pressure exceeded {pressure_threshold:.1f} bar",
                int(self.configuration["high_pressure_severity"]),
                station.receiver_pressure_bar > pressure_threshold,
            )
        )
        return transitions

    def reset(self, station: StationModel) -> list[AlarmTransition]:
        """Clear all latched alarm states and their timing history."""

        transitions = [
            AlarmTransition(
                source_identifier=alarm.source_identifier,
                source_name=alarm.source_name,
                alarm_code=alarm.alarm_code,
                message=f"Reset: {alarm.message}",
                severity=0,
                active=False,
            )
            for alarm in self.active_alarms.values()
        ]
        self.active_alarms.clear()
        self.high_temperature_durations.clear()
        for compressor in station.compressors:
            compressor.state.alarm_active = False
            compressor.sensor_fault_command = False
        return transitions

    def displayed_temperature(self, compressor: CompressorModel) -> float:
        """Return measured temperature, including an injected sensor fault."""

        if compressor.sensor_fault_command:
            return self.random_generator.uniform(-40.0, 180.0)
        return compressor.state.temperature_celsius

    def displayed_current(self, compressor: CompressorModel) -> float:
        """Return measured motor current, including an injected sensor fault."""

        if compressor.sensor_fault_command:
            return self.random_generator.uniform(-10.0, 60.0)
        return compressor.state.motor_current_amperes

    def has_alarm(self, source_identifier: str) -> bool:
        """Return whether a source currently owns at least one active alarm."""

        return any(key[0] == source_identifier for key in self.active_alarms)

    def _set_alarm(
        self,
        source_identifier: str,
        source_name: str,
        alarm_code: str,
        message: str,
        severity: int,
        condition: bool,
    ) -> list[AlarmTransition]:
        """Latch an alarm and report changes without repeating events each cycle."""

        key = (source_identifier, alarm_code)
        if condition and key not in self.active_alarms:
            transition = AlarmTransition(
                source_identifier,
                source_name,
                alarm_code,
                message,
                severity,
                True,
            )
            self.active_alarms[key] = transition
            return [transition]
        return []
