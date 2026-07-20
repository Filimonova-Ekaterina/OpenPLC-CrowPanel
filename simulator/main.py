"""Entry point for the compressed-air OPC UA equipment simulator."""

from __future__ import annotations

import argparse
import asyncio
import logging
import sys
from pathlib import Path
from typing import Any

import yaml

from compressor_logic import StationModel
from fault_scenarios import FaultManager
from opcua_server import OpcUaStationServer


LOGGER = logging.getLogger(__name__)
DEFAULT_CONFIGURATION_DIRECTORY = (
    Path(sys.executable).resolve().parent
    if getattr(sys, "frozen", False)
    else Path(__file__).resolve().parent
) / "configuration"


def load_yaml(path: Path) -> dict[str, Any]:
    """Load one YAML mapping and report an actionable configuration error."""

    with path.open("r", encoding="utf-8") as configuration_file:
        value = yaml.safe_load(configuration_file)
    if not isinstance(value, dict):
        raise ValueError(f"Configuration root must be a mapping: {path}")
    return value


async def run_simulator(configuration_directory: Path) -> None:
    """Run the model and OPC UA publication loop until interrupted."""

    equipment_configuration = load_yaml(
        configuration_directory / "equipment.yaml"
    )
    alarm_configuration = load_yaml(configuration_directory / "alarms.yaml")["alarms"]
    station = StationModel(equipment_configuration)
    fault_manager = FaultManager(alarm_configuration)
    opcua_server = OpcUaStationServer(equipment_configuration)
    await opcua_server.initialize()

    update_interval = float(
        equipment_configuration["simulation"]["update_interval_seconds"]
    )
    status_interval = float(
        equipment_configuration["simulation"]["status_log_interval_seconds"]
    )
    time_since_status = status_interval

    async with opcua_server.server:
        LOGGER.info("Compressed-air station simulator started")
        while True:
            cycle_started = asyncio.get_running_loop().time()
            reset_requested = await opcua_server.read_commands(station)
            if reset_requested:
                for transition in fault_manager.reset(station):
                    await opcua_server.emit_alarm_event(transition)
                await opcua_server.acknowledge_reset_command()
                LOGGER.info("All alarms were reset by OPC UA command")

            station.update(update_interval)
            transitions = fault_manager.evaluate(station, update_interval)
            await opcua_server.publish(station, fault_manager)
            for transition in transitions:
                await opcua_server.emit_alarm_event(transition)

            time_since_status += update_interval
            if time_since_status >= status_interval:
                running_names = [
                    item.display_name
                    for item in station.compressors
                    if item.state.is_running
                ]
                LOGGER.info(
                    "Demand %.1f m3/h | Receiver %.2f bar | Running: %s | Power %.1f kW",
                    station.air_demand_m3h,
                    station.receiver_pressure_bar,
                    ", ".join(running_names) if running_names else "none",
                    station.total_power_kw,
                )
                time_since_status = 0.0

            cycle_duration = asyncio.get_running_loop().time() - cycle_started
            await asyncio.sleep(max(0.0, update_interval - cycle_duration))


def parse_arguments() -> argparse.Namespace:
    """Parse command-line options."""

    parser = argparse.ArgumentParser(
        description="Compressed-air station OPC UA simulator"
    )
    parser.add_argument(
        "--config-dir",
        type=Path,
        default=DEFAULT_CONFIGURATION_DIRECTORY,
        help="Directory containing equipment.yaml and alarms.yaml",
    )
    return parser.parse_args()


def main() -> None:
    """Configure logging and start the asynchronous simulator."""

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s | %(levelname)-8s | %(name)s | %(message)s",
    )
    arguments = parse_arguments()
    try:
        asyncio.run(run_simulator(arguments.config_dir.resolve()))
    except KeyboardInterrupt:
        LOGGER.info("Simulator stopped by user")


if __name__ == "__main__":
    main()
